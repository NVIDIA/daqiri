/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// DAQIRI -> TensorRT ResNet inference (GitHub issue #73).
//
//   wire int8 packets (raw / DPDK GPUDirect)
//     -> config-based GPU reorder + int8→fp16 (one burst = one batch)
//     -> SPSC handoff (RX producer | inference consumer)
//     -> ResNet feature extraction (TensorRT, FP16 tensor cores)
//     -> per-class mean-feature stats (example mode)
//
// Primary path: Spark-to-Spark xhost (--mode tx on stacked-01, --mode rx on
// stacked-02). See configs/resnet50_{tx,rx}_spark_xhost.yaml and
// tools/run_resnet_xhost.sh.

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "app_config.h"
#include "feature_sink.h"
#include "inference_pipeline.h"
#include "pcap_replayer.h"
#include "raw_bench_common.h"
#include "spsc_queue.h"

#include <cuda_runtime.h>

#include <daqiri/daqiri.h>

namespace {

const char* find_flag_value(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc - 1; ++i) {
    if (flag == argv[i]) return argv[i + 1];
  }
  return nullptr;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc; ++i) {
    if (flag == argv[i]) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <config.yaml> [--mode tx|rx] [--seconds N] [--dataset <pcap>]"
                 " [--replay-once|--loop] [--images-per-batch N]"
                 " [--expected-images N] [--model resnet18|34|50|101|152]\n";
    return 1;
  }

  namespace app = daqiri::apps::resnet;
  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);

  app::AppConfig cfg;
  YAML::Node root;
  try {
    root = YAML::LoadFile(argv[1]);
    cfg = app::AppConfig::from_yaml(root);
    cfg.materialize_daqiri_reorder(root);
  } catch (const std::exception& e) {
    std::cerr << "Invalid config: " << e.what() << "\n";
    return 1;
  }

  if (const char* mode = find_flag_value(argc, argv, "--mode")) {
    const std::string m = mode;
    if (m == "tx") {
      cfg.role = app::AppRole::Tx;
      if (!cfg.has_tx) {
        std::cerr << "--mode tx but config has no bench_tx\n";
        return 1;
      }
    } else if (m == "rx") {
      cfg.role = app::AppRole::Rx;
      if (!cfg.has_rx) {
        std::cerr << "--mode rx but config has no bench_rx\n";
        return 1;
      }
    } else {
      std::cerr << "Unknown --mode " << m << " (use tx|rx)\n";
      return 1;
    }
  }

  if (const char* ds = find_flag_value(argc, argv, "--dataset")) {
    cfg.dataset_pcap = ds;
  }
  if (const char* ipb = find_flag_value(argc, argv, "--images-per-batch")) {
    if (!cfg.apply_images_per_batch_override(static_cast<uint32_t>(std::stoul(ipb)))) {
      std::cerr << "Invalid --images-per-batch (must be power-of-two divisor of "
                   "packets_per_batch/packets_per_image)\n";
      return 1;
    }
  }
  if (const char* model = find_flag_value(argc, argv, "--model")) {
    const std::string m = model;
    cfg.trt.onnx_path = "models/" + m + "_features.onnx";
    cfg.trt.engine_path = "models/" + m + "_features.fp16in.engine";
    cfg.trt.feature_dim = (m == "resnet18" || m == "resnet34") ? 512 : 2048;
  }

  bool loop = !cfg.example_mode();
  if (has_flag(argc, argv, "--loop")) loop = true;
  if (has_flag(argc, argv, "--replay-once")) loop = false;

  std::string eth_dst = cfg.tx.eth_dst_addr;
  if (const char* env = std::getenv("ETH_DST_ADDR")) {
    if (env[0] != '\0') eth_dst = env;
  }

  std::vector<int> labels;
  if (cfg.role != app::AppRole::Tx && cfg.example_mode()) {
    if (cfg.labels_path.empty()) cfg.labels_path = cfg.dataset_pcap + ".labels";
    labels = app::load_labels(cfg.labels_path);
    if (labels.empty()) {
      std::cerr << "Warning: no labels loaded; per-class stats disabled\n";
    }
  }

  const char* role_str =
      cfg.role == app::AppRole::Tx ? "tx" : (cfg.role == app::AppRole::Rx ? "rx" : "both");
  std::cerr << "ResNet inference: role=" << role_str
            << " mode=" << (cfg.example_mode() ? "example (dataset)" : "benchmark (synthetic)")
            << " feature_dim=" << cfg.trt.feature_dim
            << " packets_per_image=" << cfg.packets_per_image
            << " out_payload_len=" << cfg.out_payload_len
            << " packets_per_batch=" << cfg.packets_per_batch
            << " images_per_batch=" << cfg.images_per_batch;
  if (cfg.role != app::AppRole::Rx) std::cerr << " eth_dst=" << eth_dst;
  std::cerr << "\n";

  std::vector<app::PcapFrame> frames;
  if (cfg.role != app::AppRole::Rx) {
    if (cfg.example_mode()) {
      app::PcapReplayer replayer;
      if (!replayer.load(cfg.dataset_pcap)) {
        std::cerr << "Failed to load dataset pcap: " << cfg.dataset_pcap << "\n";
        return 1;
      }
      frames = replayer.frames();
    } else {
      frames = app::build_synthetic_frames(cfg);
    }
  }

  uint64_t expected_images = 0;
  if (cfg.role != app::AppRole::Tx) {
    if (const char* ei = find_flag_value(argc, argv, "--expected-images")) {
      expected_images = std::stoull(ei);
    } else if (!labels.empty()) {
      expected_images = labels.size();
    } else if (cfg.role == app::AppRole::Both && cfg.example_mode() &&
               cfg.packets_per_image > 0) {
      expected_images = frames.size() / cfg.packets_per_image;
    }
  }

  // App reorder: is authoritative; materialize_daqiri_reorder() already injected
  // daqiri…rx.reorder_configs into `root` for RX roles.
  if (daqiri::daqiri_init_from_yaml_string(YAML::Dump(root)) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  cudaStream_t reorder_stream = nullptr;
  if (cfg.role != app::AppRole::Tx) {
    if (cudaStreamCreateWithFlags(&reorder_stream, cudaStreamNonBlocking) != cudaSuccess) {
      std::cerr << "cudaStreamCreate(reorder_stream) failed\n";
      daqiri::shutdown();
      return 1;
    }
    if (daqiri::set_reorder_cuda_stream(cfg.rx.interface_name, cfg.reorder_name, reorder_stream) !=
        daqiri::Status::SUCCESS) {
      std::cerr << "set_reorder_cuda_stream(" << cfg.rx.interface_name << ", " << cfg.reorder_name
                << ") failed\n";
      cudaStreamDestroy(reorder_stream);
      daqiri::shutdown();
      return 1;
    }
    std::cerr << "set_reorder_cuda_stream OK (" << cfg.reorder_name << ")\n";
  }

  const bool example_stats =
      cfg.role != app::AppRole::Tx && cfg.example_mode() && !labels.empty() && cfg.stats_enabled;
  app::FeatureSink sink(example_stats, cfg.trt.feature_dim, cfg.stats_top_k, labels);

  std::atomic<bool> stop{false};
  std::atomic<bool> rx_ready{false};
  std::atomic<bool> tx_done{false};
  std::atomic<bool> producer_done{false};

  app::InferenceQueue<app::kInferenceQueueCap> inf_queue;
  std::thread consumer_thread;
  std::thread producer_thread;
  std::thread tx_thread;

  if (cfg.role != app::AppRole::Tx) {
    consumer_thread = std::thread(app::inference_consumer_worker, std::cref(cfg), std::ref(sink),
                                  std::ref(inf_queue), std::ref(producer_done), std::ref(rx_ready),
                                  std::ref(stop));
    while (!rx_ready.load() && !stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    producer_thread =
        std::thread(app::rx_producer_worker, std::cref(cfg), std::ref(inf_queue), expected_images,
                    std::ref(producer_done), std::ref(stop));
  } else {
    rx_ready.store(true);
  }

  const auto run_t0 = std::chrono::steady_clock::now();
  if (cfg.role != app::AppRole::Rx) {
    tx_thread = std::thread(app::pcap_tx_worker, std::cref(cfg), std::cref(frames),
                            std::cref(eth_dst), loop, std::ref(tx_done), std::ref(stop));
  } else {
    tx_done.store(true);
  }

  double summary_seconds = run_seconds;
  if (cfg.role == app::AppRole::Tx) {
    if (loop) {
      daqiri::bench::wait_for_stop(run_seconds, stop);
    }
    if (tx_thread.joinable()) tx_thread.join();
    summary_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - run_t0).count();
  } else if (cfg.role == app::AppRole::Rx) {
    if (loop || expected_images == 0) {
      daqiri::bench::wait_for_stop(run_seconds, stop);
      stop.store(true);
    }
    if (producer_thread.joinable()) producer_thread.join();
    producer_done.store(true);
    if (consumer_thread.joinable()) consumer_thread.join();
    summary_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - run_t0).count();
  } else {
    // Combined single-host path (optional loopback smoke).
    if (loop) {
      daqiri::bench::wait_for_stop(run_seconds, stop);
    } else {
      if (tx_thread.joinable()) tx_thread.join();
    }
    if (tx_thread.joinable()) tx_thread.join();
    if (producer_thread.joinable()) producer_thread.join();
    producer_done.store(true);
    if (consumer_thread.joinable()) consumer_thread.join();
    summary_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - run_t0).count();
  }

  if (cfg.role != app::AppRole::Tx) {
    sink.log_final_summary(summary_seconds);
  }
  daqiri::print_stats();
  daqiri::shutdown();
  if (reorder_stream != nullptr) {
    cudaStreamDestroy(reorder_stream);
  }
  return 0;
}
