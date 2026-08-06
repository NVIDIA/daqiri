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

#include "inference_pipeline.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

#include "raw_bench_common.h"
#include "trt_runner.hpp"

#include <daqiri/daqiri.h>

namespace daqiri::apps::resnet {

void rx_producer_worker(const AppConfig& cfg, InferenceQueue<kInferenceQueueCap>& queue,
                        uint64_t expected_images, std::atomic<bool>& producer_done,
                        std::atomic<bool>& stop) {
  if (!daqiri::bench::set_current_thread_affinity(cfg.rx.cpu_core, "resnet_rx_prod")) {
    stop.store(true);
    producer_done.store(true);
    return;
  }

  const int port_id = daqiri::get_port_id(cfg.rx.interface_name);
  if (port_id < 0) {
    std::cerr << "rx_producer_worker: invalid RX interface " << cfg.rx.interface_name << "\n";
    stop.store(true);
    producer_done.store(true);
    return;
  }
  const int queue_id = cfg.rx.queue_id >= 0 ? cfg.rx.queue_id : 0;

  uint64_t images_pushed = 0;
  uint64_t bursts_reordered = 0;
  uint64_t bursts_not_reordered = 0;
  uint64_t packets_not_reordered = 0;
  uint64_t bursts_partial = 0;
  uint64_t bursts_dropped = 0;
  uint64_t partial_packets = 0;
  uint64_t info_failures = 0;
  // ~100us per empty poll. Post-traffic quiescence is short (a stalled tail);
  // the pre-traffic window is long so a slow peer TX start is not mistaken for
  // "no traffic ever" (RX-only runs are started before the TX host).
  constexpr uint32_t kQuiesceIters = 50000;        // ~5 s after data started
  constexpr uint32_t kStartupIdleIters = 1800000;  // ~3 min before any data
  uint32_t idle_polls = 0;
  const bool backpressure = cfg.example_mode();

  while (!stop.load()) {
    daqiri::BurstParams* burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id, queue_id) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      if (expected_images > 0 && images_pushed >= expected_images) break;
      ++idle_polls;
      if (images_pushed > 0 ? (idle_polls >= kQuiesceIters) : (idle_polls >= kStartupIdleIters)) {
        std::cerr << "rx_producer_worker: idle timeout after " << images_pushed << " images\n";
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    idle_polls = 0;

    const uint32_t flags = burst->hdr.hdr.burst_flags;
    if ((flags & daqiri::DAQIRI_BURST_FLAG_REORDERED) == 0U) {
      ++bursts_not_reordered;
      packets_not_reordered +=
          static_cast<uint64_t>(std::max<int64_t>(0, daqiri::get_num_packets(burst)));
      daqiri::free_all_packets_and_burst_rx(burst);
      continue;
    }
    ++bursts_reordered;

    uint32_t n_img = cfg.images_per_batch;
    if ((flags & daqiri::DAQIRI_BURST_FLAG_REORDER_TIMEOUT) != 0U) {
      ++bursts_partial;
      daqiri::ReorderBurstInfo info{};
      // A timeout-flushed burst can still have its reorder event in flight, in
      // which case the info query reports NOT_READY: wait it out instead of
      // discarding a partially filled batch.
      constexpr uint32_t kInfoRetries = 2000;  // ~100 ms at 50 us per retry
      daqiri::Status info_status = daqiri::Status::NOT_READY;
      for (uint32_t attempt = 0; attempt < kInfoRetries; ++attempt) {
        info_status = daqiri::get_reorder_burst_info(burst, &info);
        if (info_status != daqiri::Status::NOT_READY) break;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      if (info_status != daqiri::Status::SUCCESS || cfg.packets_per_image == 0) {
        ++bursts_dropped;
        ++info_failures;
        std::cerr << "rx_producer_worker: get_reorder_burst_info failed (status "
                  << static_cast<int>(info_status) << "), dropping partial batch\n";
        daqiri::free_all_packets_and_burst_rx(burst);
        continue;
      }
      partial_packets += info.source_packet_count;
      std::cerr << "rx_producer_worker: partial burst #" << bursts_reordered
                << " batch_id=" << info.batch_id << " packets=" << info.source_packet_count << "\n";
      n_img = info.source_packet_count / cfg.packets_per_image;
      if (n_img == 0) {
        ++bursts_dropped;
        std::cerr << "rx_producer_worker: partial reorder burst with " << info.source_packet_count
                  << " packets (< packets_per_image " << cfg.packets_per_image << "), dropped\n";
        daqiri::free_all_packets_and_burst_rx(burst);
        continue;
      }
    }

    void* dev_input = daqiri::get_packet_ptr(burst, 0);
    if (dev_input == nullptr) {
      ++bursts_dropped;
      daqiri::free_all_packets_and_burst_rx(burst);
      continue;
    }

    InferenceJob job{burst, dev_input, n_img, burst->event};
    bool pushed = false;
    if (backpressure) {
      while (!(pushed = queue.try_push(job)) && !stop.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    } else {
      pushed = queue.try_push(job);
    }
    if (!pushed) {
      ++bursts_dropped;
      daqiri::free_all_packets_and_burst_rx(burst);
      if (backpressure) break;
      continue;
    }

    images_pushed += n_img;
    if (expected_images > 0 && images_pushed >= expected_images) break;
  }

  producer_done.store(true);
  std::cerr << "rx_producer_worker: pushed " << images_pushed
            << " images (reordered_bursts=" << bursts_reordered << " partial=" << bursts_partial
            << " dropped=" << bursts_dropped << " partial_packets=" << partial_packets
            << " info_failures=" << info_failures
            << " non_reordered_bursts=" << bursts_not_reordered
            << " non_reordered_packets=" << packets_not_reordered << ")\n";
}

void inference_consumer_worker(const AppConfig& cfg, FeatureSink& sink,
                               InferenceQueue<kInferenceQueueCap>& queue,
                               std::atomic<bool>& producer_done, std::atomic<bool>& ready,
                               std::atomic<bool>& stop) {
  if (cfg.inference_cpu_core >= 0) {
    if (!daqiri::bench::set_current_thread_affinity(cfg.inference_cpu_core, "resnet_inf")) {
      stop.store(true);
      ready.store(true);
      return;
    }
  }

  cudaStream_t inf_stream = nullptr;
  if (cudaStreamCreateWithFlags(&inf_stream, cudaStreamNonBlocking) != cudaSuccess) {
    std::cerr << "inference_consumer_worker: cudaStreamCreate failed\n";
    stop.store(true);
    ready.store(true);
    return;
  }

  TrtRunner trt(cfg.trt, inf_stream);
  trt.initialize();

  cudaEvent_t release_evt = nullptr;
  cudaEventCreateWithFlags(&release_evt, cudaEventDisableTiming);

  ready.store(true);

  daqiri::BurstParams* prev_burst = nullptr;
  const auto t0 = std::chrono::steady_clock::now();

  while (true) {
    InferenceJob job{};
    if (!queue.try_pop(job)) {
      if (producer_done.load() && queue.empty()) break;
      if (stop.load() && queue.empty()) break;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }

    float* host_prev = nullptr;
    uint32_t n_prev = 0;
    const bool ok =
        trt.infer(job.dev_input, job.batch_size, job.input_ready, release_evt, host_prev, n_prev);
    if (ok) {
      if (host_prev != nullptr) sink.consume(host_prev, n_prev);
      if (prev_burst != nullptr) {
        daqiri::free_all_packets_and_burst_rx(prev_burst);
      }
      prev_burst = job.burst;
    } else {
      daqiri::free_all_packets_and_burst_rx(job.burst);
    }
  }

  float* host_final = nullptr;
  uint32_t n_final = 0;
  trt.drain_final(host_final, n_final);
  if (host_final != nullptr) sink.consume(host_final, n_final);
  if (prev_burst != nullptr) {
    daqiri::free_all_packets_and_burst_rx(prev_burst);
  }

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cerr << "inference_consumer_worker: " << trt.total_batches_inferred()
            << " inference batches in " << secs << " s\n";

  std::vector<float> lat = trt.batch_latencies_ms();
  if (!lat.empty()) {
    std::sort(lat.begin(), lat.end());
    const double mean =
        std::accumulate(lat.begin(), lat.end(), 0.0) / static_cast<double>(lat.size());
    const auto pct = [&lat](double p) {
      const size_t idx =
          std::min(lat.size() - 1, static_cast<size_t>(p * (static_cast<double>(lat.size()) - 1)));
      return lat[idx];
    };
    std::cerr << "inference latency (ms): mean=" << mean << " p50=" << pct(0.50)
              << " p99=" << pct(0.99) << " (per batch of " << cfg.images_per_batch
              << " images, n=" << lat.size() << ")\n";
  }

  cudaEventDestroy(release_evt);
  cudaStreamDestroy(inf_stream);
}

}  // namespace daqiri::apps::resnet
