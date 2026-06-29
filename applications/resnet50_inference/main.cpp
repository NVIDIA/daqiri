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

// DAQIRI -> TensorRT ResNet inference example (GitHub issue #73).
//
//   received packets (raw / DPDK GPUDirect)
//     -> GPU sequence-number reorder (image reassembly)
//     -> ResNet feature extraction (TensorRT, FP16)
//     -> per-class mean-feature stats (example mode)
//
// Example mode (--dataset <pcap>) replays preprocessed CIFAR-10 images and
// prints per-class mean-feature stats. Without --dataset the TX synthesizes
// frames for the throughput benchmark (no stats). Runs over the DGX Spark
// physical p0->p1 cabled loopback (see configs/ + README).

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
              << " <config.yaml> [--seconds N] [--dataset <pcap>]"
                 " [--replay-once|--loop] [--images-per-batch N]"
                 " [--model resnet18|34|50|101|152]\n";
    return 1;
  }

  namespace app = daqiri::apps::resnet;
  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);

  app::AppConfig cfg;
  try {
    const auto root = YAML::LoadFile(argv[1]);
    cfg = app::AppConfig::from_yaml(root);
  } catch (const std::exception& e) {
    std::cerr << "Invalid config: " << e.what() << "\n";
    return 1;
  }

  // CLI overrides.
  if (const char* ds = find_flag_value(argc, argv, "--dataset")) {
    cfg.dataset_pcap = ds;
  }
  if (const char* ipb = find_flag_value(argc, argv, "--images-per-batch")) {
    cfg.images_per_batch = std::min<uint32_t>(static_cast<uint32_t>(std::stoul(ipb)),
                                              static_cast<uint32_t>(cfg.trt.opt_max));
  }
  if (const char* model = find_flag_value(argc, argv, "--model")) {
    const std::string m = model;
    cfg.trt.onnx_path = "models/" + m + "_features.onnx";
    cfg.trt.engine_path = "models/" + m + "_features.fp16.engine";
    cfg.trt.feature_dim = (m == "resnet18" || m == "resnet34") ? 512 : 2048;
    // image_bytes / geometry are independent of feature_dim (input dims fixed).
  }
  // Example mode replays the dataset ONCE by default: a small dataset fits
  // entirely in the RX ring (num_images * packets_per_image < num_bufs), so no
  // packets drop even though ResNet inference is slower than line rate -- which
  // keeps the drop-free, in-order reassembly (and label mapping) intact.
  // Benchmark mode loops to sustain throughput (drops are tolerable there since
  // it only counts). Override with --loop / --replay-once.
  bool loop = !cfg.example_mode();
  if (has_flag(argc, argv, "--loop")) loop = true;
  if (has_flag(argc, argv, "--replay-once")) loop = false;

  // Destination MAC for the replayed frames: ETH_DST_ADDR env (the RX port MAC,
  // matching the Spark report) overrides the config placeholder.
  std::string eth_dst = cfg.tx.eth_dst_addr;
  if (const char* env = std::getenv("ETH_DST_ADDR")) {
    if (env[0] != '\0') eth_dst = env;
  }

  // Labels sidecar (example mode only).
  std::vector<int> labels;
  if (cfg.example_mode()) {
    if (cfg.labels_path.empty()) cfg.labels_path = cfg.dataset_pcap + ".labels";
    labels = app::load_labels(cfg.labels_path);
    if (labels.empty()) {
      std::cerr << "Warning: no labels loaded; per-class stats disabled\n";
    }
  }

  std::cerr << "ResNet inference: mode="
            << (cfg.example_mode() ? "example (dataset)" : "benchmark (synthetic)")
            << " feature_dim=" << cfg.trt.feature_dim
            << " packets_per_image=" << cfg.packets_per_image
            << " out_payload_len=" << cfg.out_payload_len
            << " images_per_batch=" << cfg.images_per_batch << " eth_dst=" << eth_dst << "\n";

  // Build / load the TX frame source before init so a bad dataset fails fast.
  std::vector<app::PcapFrame> frames;
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

  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  app::FeatureSink sink(cfg.example_mode() && !labels.empty(), cfg.trt.feature_dim, cfg.stats_top_k,
                        labels);

  std::atomic<bool> stop{false};
  std::atomic<bool> rx_ready{false};
  std::atomic<bool> tx_done{false};
  // Example mode drains exactly the dataset; benchmark mode (0) runs until stop.
  const uint64_t expected_images =
      cfg.example_mode() && cfg.packets_per_image > 0 ? frames.size() / cfg.packets_per_image : 0;
  std::thread rx_thread(app::inference_rx_worker, std::cref(cfg), std::ref(sink), expected_images,
                        std::ref(rx_ready), std::ref(tx_done), std::ref(stop));

  // The first-run TensorRT engine build can take ~a minute -- longer than the
  // whole --seconds window. Wait until the RX path is ready (engine built/loaded
  // and buffers allocated) before starting the TX and the run-duration timer, so
  // the timer measures actual receive/inference time, not the engine build. An
  // init failure sets `stop`, which also breaks this wait.
  while (!rx_ready.load() && !stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const auto run_t0 = std::chrono::steady_clock::now();
  std::thread tx_thread(app::pcap_tx_worker, std::cref(cfg), std::cref(frames), std::cref(eth_dst),
                        loop, std::ref(tx_done), std::ref(stop));

  double summary_seconds = run_seconds;
  if (loop) {
    // Benchmark / continuous replay: measure a fixed wall-clock window.
    daqiri::bench::wait_for_stop(run_seconds, stop);
  } else {
    // Replay-once (example mode): the TX sends the dataset once, then the RX
    // worker drains the ring to quiescence (it sees tx_done) and flushes the
    // final partial batch -- so the whole dataset is inferred even though
    // inference runs slower than line rate. No fixed timer: just let TX finish
    // and RX drain, then report the actual elapsed time.
    if (tx_thread.joinable()) tx_thread.join();
    if (rx_thread.joinable()) rx_thread.join();
    summary_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - run_t0).count();
  }

  if (tx_thread.joinable()) tx_thread.join();
  if (rx_thread.joinable()) rx_thread.join();

  sink.log_final_summary(summary_seconds);
  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
