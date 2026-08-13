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
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
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
  uint64_t partial_packets = 0;
  uint64_t info_failures = 0;
  // Drop reasons are counted separately: a burst lost to a full SPSC queue means
  // inference could not keep up, while the others mean the RX side produced
  // something unusable. Folding them into one counter makes backpressure
  // indistinguishable from NIC/reorder loss.
  uint64_t dropped_queue_full = 0;
  uint64_t dropped_short_batch = 0;
  uint64_t dropped_info_failure = 0;
  uint64_t dropped_null_input = 0;
  // How far short of packets_per_batch the timeout-flushed batches closed.
  //
  // This is NOT a loss count. A batch closes early either because packets were
  // lost or because the source simply paused mid-batch, and ReorderBurstInfo
  // reports only a count -- not which sequence slots are covered -- so the two
  // are indistinguishable here. Reported descriptively; see the remainder
  // counter below for the part that is unambiguous.
  uint64_t early_flush_shortfall = 0;
  // Packets discarded because they did not complete an image: a flush that lands
  // mid-image leaves source_packet_count % packets_per_image stragglers, which
  // integer division drops. Unambiguous, and directly attributable.
  uint64_t remainder_packets_dropped = 0;
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
        ++dropped_info_failure;
        ++info_failures;
        std::cerr << "rx_producer_worker: get_reorder_burst_info failed (status "
                  << static_cast<int>(info_status) << "), dropping partial batch\n";
        daqiri::free_all_packets_and_burst_rx(burst);
        continue;
      }
      partial_packets += info.source_packet_count;
      if (cfg.packets_per_batch > info.source_packet_count) {
        early_flush_shortfall += cfg.packets_per_batch - info.source_packet_count;
      }
      remainder_packets_dropped += info.source_packet_count % cfg.packets_per_image;
      std::cerr << "rx_producer_worker: partial burst #" << bursts_reordered
                << " batch_id=" << info.batch_id << " packets=" << info.source_packet_count
                << " (expected " << cfg.packets_per_batch << ")\n";
      n_img = info.source_packet_count / cfg.packets_per_image;
      if (n_img == 0) {
        ++dropped_short_batch;
        std::cerr << "rx_producer_worker: partial reorder burst with " << info.source_packet_count
                  << " packets (< packets_per_image " << cfg.packets_per_image << "), dropped\n";
        daqiri::free_all_packets_and_burst_rx(burst);
        continue;
      }
    }

    void* dev_input = daqiri::get_packet_ptr(burst, 0);
    if (dev_input == nullptr) {
      ++dropped_null_input;
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
      ++dropped_queue_full;
      daqiri::free_all_packets_and_burst_rx(burst);
      if (backpressure) break;
      continue;
    }

    images_pushed += n_img;
    if (expected_images > 0 && images_pushed >= expected_images) break;
  }

  producer_done.store(true);
  const uint64_t dropped_total =
      dropped_queue_full + dropped_short_batch + dropped_info_failure + dropped_null_input;
  // Built as one string and written once: the consumer thread is still logging
  // concurrently, and chained operator<< on std::cerr is not atomic, so a split
  // write interleaves mid-line. The runner greps these lines with ^-anchored
  // patterns, so an interleaved prefix silently loses the counters.
  //
  // Reported as a chain so each stage's loss is attributable: the engine
  // delivers reordered bursts, the producer pushes a subset to the queue, and
  // the consumer (see inference_consumer_worker) infers a subset of those.
  std::ostringstream os;
  os << "rx_producer_worker: delivered_bursts=" << bursts_reordered
     << " pushed_images=" << images_pushed << " dropped_bursts=" << dropped_total
     << " [queue_full=" << dropped_queue_full << " short_batch=" << dropped_short_batch
     << " info_failure=" << dropped_info_failure << " null_input=" << dropped_null_input << "]\n";
  os << "rx_producer_worker: partial_bursts=" << bursts_partial
     << " partial_packets=" << partial_packets
     << " early_flush_shortfall=" << early_flush_shortfall
     << " remainder_dropped=" << remainder_packets_dropped
     << " info_failures=" << info_failures << " non_reordered_bursts=" << bursts_not_reordered
     << " non_reordered_packets=" << packets_not_reordered << "\n";
  if (dropped_queue_full > 0) {
    os << "rx_producer_worker: WARNING " << dropped_queue_full
       << " bursts dropped on a full inference queue -- throughput is consumer-bound "
          "and these are NOT link losses\n";
  }
  if (remainder_packets_dropped > 0) {
    os << "rx_producer_worker: WARNING " << remainder_packets_dropped
       << " packets discarded as incomplete-image remainders after a mid-image flush\n";
  }
  // A source that cannot sustain packets_per_batch between idle gaps closes
  // nearly every batch on the timeout. That is a TX-rate symptom, not loss --
  // call it out so the shortfall above is not misread as packets dropped.
  if (bursts_reordered > 0 && bursts_partial * 2 > bursts_reordered) {
    os << "rx_producer_worker: NOTE " << bursts_partial << "/" << bursts_reordered
       << " batches closed on the idle timeout rather than filling "
       << cfg.packets_per_batch
       << " packets. The source is not sustaining a full batch between gaps; this is a "
          "TX-rate symptom. Whether any packets were also LOST cannot be determined here -- "
          "ReorderBurstInfo reports a count, not which sequence slots are covered. Compare "
          "offered TX frames against partial_packets to tell the two apart.\n";
  }
  std::cerr << os.str();
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
  // Per-interval throughput. A terminal aggregate cannot distinguish a run that
  // held its rate from one that started high and decayed, so the warmup knee is
  // invisible without this.
  constexpr double kReportIntervalSec = 1.0;
  auto interval_t0 = t0;
  uint64_t interval_images = 0;

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

    interval_images += job.batch_size;
    const auto now = std::chrono::steady_clock::now();
    const double interval_s = std::chrono::duration<double>(now - interval_t0).count();
    if (interval_s >= kReportIntervalSec) {
      const double elapsed_s = std::chrono::duration<double>(now - t0).count();
      std::cerr << "inference_consumer_worker: t=" << std::fixed << std::setprecision(1)
                << elapsed_s << "s " << std::setprecision(0)
                << (static_cast<double>(interval_images) / interval_s) << " img/s"
                << " (queue_depth=" << queue.depth() << ")\n"
                << std::defaultfloat;
      interval_t0 = now;
      interval_images = 0;
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
  // One write, for the same reason as the producer summary above: the producer
  // may still be logging, and the runner greps these lines ^-anchored.
  std::ostringstream os;
  os << "inference_consumer_worker: " << trt.total_batches_inferred() << " inference batches in "
     << secs << " s\n";

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
    os << "inference latency (ms): mean=" << mean << " p50=" << pct(0.50) << " p99=" << pct(0.99)
       << " (per batch of " << cfg.images_per_batch << " images, n=" << lat.size() << ")\n";
  }
  std::cerr << os.str();

  cudaEventDestroy(release_evt);
  cudaStreamDestroy(inf_stream);
}

}  // namespace daqiri::apps::resnet
