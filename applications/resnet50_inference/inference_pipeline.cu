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

#include <cuda_fp16.h>
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

namespace {

__global__ void contiguous_int8_to_fp16_kernel(const int8_t* input, __half* output,
                                               size_t elements) {
  const size_t stride = static_cast<size_t>(blockDim.x) * gridDim.x;
  for (size_t i = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < elements;
       i += stride) {
    output[i] = __float2half(static_cast<float>(input[i]));
  }
}

class PostReorderConverter {
 public:
  PostReorderConverter(const AppConfig& cfg, cudaStream_t stream, bool profile)
      : enabled_(cfg.needs_post_reorder_conversion()),
        stream_(stream),
        profile_(profile),
        elements_per_image_(static_cast<size_t>(cfg.trt.channels) * cfg.trt.height * cfg.trt.width),
        max_elements_(static_cast<size_t>(cfg.images_per_batch) * elements_per_image_) {}

  ~PostReorderConverter() {
    if (ready_ != nullptr) cudaEventDestroy(ready_);
    if (start_ != nullptr) cudaEventDestroy(start_);
    if (output_ != nullptr) cudaFree(output_);
  }

  bool initialize() {
    if (!enabled_) return true;
    if (cudaMalloc(&output_, max_elements_ * sizeof(__half)) != cudaSuccess ||
        (profile_ && cudaEventCreate(&start_) != cudaSuccess) ||
        cudaEventCreate(&ready_) != cudaSuccess) {
      std::cerr << "hardware post-reorder conversion setup failed\n";
      return false;
    }

    // Trigger CUDA module/context initialization before advertising readiness.
    // A zero-element launch exercises the measured kernel without touching
    // packet data or contributing to the profile.
    contiguous_int8_to_fp16_kernel<<<1, 1, 0, stream_>>>(nullptr, output_, 0);
    const cudaError_t status = cudaStreamSynchronize(stream_);
    if (status != cudaSuccess) {
      std::cerr << "hardware post-reorder conversion warm-up failed: " << cudaGetErrorString(status)
                << "\n";
      return false;
    }
    std::cerr << "hardware reorder INT8->FP16 conversion enabled\n";
    return true;
  }

  bool enabled() const {
    return enabled_;
  }
  void* output() const {
    return output_;
  }
  cudaEvent_t ready_event() const {
    return ready_;
  }

  bool enqueue(const InferenceJob& job) {
    if (!enabled_) return true;
    if (job.input_ready != nullptr &&
        cudaStreamWaitEvent(stream_, job.input_ready, 0) != cudaSuccess) {
      return false;
    }
    if (profile_ && cudaEventRecord(start_, stream_) != cudaSuccess) return false;

    const size_t elements = static_cast<size_t>(job.batch_size) * elements_per_image_;
    constexpr uint32_t kThreads = 256;
    const uint32_t blocks = static_cast<uint32_t>((elements + kThreads - 1) / kThreads);
    contiguous_int8_to_fp16_kernel<<<blocks, kThreads, 0, stream_>>>(
        static_cast<const int8_t*>(job.dev_input), output_, elements);
    return cudaEventRecord(ready_, stream_) == cudaSuccess;
  }

  bool synchronize_and_profile() {
    if (!enabled_) return true;
    if (cudaEventSynchronize(ready_) != cudaSuccess) return false;
    if (!profile_) return true;

    float elapsed_ms = 0.0f;
    if (cudaEventElapsedTime(&elapsed_ms, start_, ready_) != cudaSuccess) return false;
    total_us_ += static_cast<double>(elapsed_ms) * 1000.0;
    ++samples_;
    return true;
  }

  void append_profile(std::ostringstream& os, double active_secs) const {
    if (!enabled_ || !profile_) return;
    const double mean_us = samples_ == 0 ? 0.0 : total_us_ / samples_;
    const double share_pct = active_secs > 0.0 ? total_us_ / (active_secs * 1.0e6) * 100.0 : 0.0;
    os << "post-reorder INT8->FP16 profile: samples=" << samples_
       << " total_kernel_us=" << total_us_ << " mean_kernel_us=" << mean_us
       << " active_share_pct=" << share_pct << "\n";
  }

 private:
  bool enabled_ = false;
  cudaStream_t stream_ = nullptr;
  bool profile_ = false;
  size_t elements_per_image_ = 0;
  size_t max_elements_ = 0;
  __half* output_ = nullptr;
  cudaEvent_t start_ = nullptr;
  cudaEvent_t ready_ = nullptr;
  double total_us_ = 0.0;
  uint64_t samples_ = 0;
};

}  // namespace

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
  uint64_t batches_delivered = 0;
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

    const auto delivered_at = std::chrono::steady_clock::now();
    const auto delivered_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
    InferenceJob job{burst, dev_input, n_img, burst->event, delivered_at};
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

    // Preserve the first-delivery timestamp used by the campaign summary and
    // sparse checkpoints for diagnostics without putting synchronous stderr
    // writes on every burst-recycle path.
    if (cfg.stats_enabled && (batches_delivered == 0 || batches_delivered % 1024 == 0)) {
      std::ostringstream delivery;
      delivery << "rx_producer_worker: batch_delivered index=" << batches_delivered
               << " unix_ns=" << delivered_unix_ns << " images=" << n_img << "\n";
      std::cerr << delivery.str();
    }
    ++batches_delivered;

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
     << " partial_packets=" << partial_packets << " early_flush_shortfall=" << early_flush_shortfall
     << " remainder_dropped=" << remainder_packets_dropped << " info_failures=" << info_failures
     << " non_reordered_bursts=" << bursts_not_reordered
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
       << " batches closed on the idle timeout rather than filling " << cfg.packets_per_batch
       << " packets. The source is not sustaining a full batch between gaps; this is a "
          "TX-rate symptom. Whether any packets were also LOST cannot be determined here -- "
          "ReorderBurstInfo reports a count, not which sequence slots are covered. Compare "
          "offered TX frames against partial_packets to tell the two apart.\n";
  }
  std::cerr << os.str();
}

void no_inference_consumer_worker(const AppConfig& cfg, FeatureSink& sink,
                                  InferenceQueue<kInferenceQueueCap>& queue,
                                  std::atomic<bool>& producer_done, std::atomic<bool>& ready,
                                  std::atomic<bool>& stop) {
  if (cfg.inference_cpu_core >= 0 &&
      !daqiri::bench::set_current_thread_affinity(cfg.inference_cpu_core, "resnet_inf")) {
    stop.store(true);
    ready.store(true);
    return;
  }

  cudaStream_t sync_stream = nullptr;
  if (cudaStreamCreateWithFlags(&sync_stream, cudaStreamNonBlocking) != cudaSuccess) {
    std::cerr << "inference_consumer_worker: cudaStreamCreate failed\n";
    stop.store(true);
    ready.store(true);
    return;
  }

  // Hardware reorder direct-places wire INT8 bytes. The shared converter keeps
  // the application contract identical to the software-reorder FP16 path.
  PostReorderConverter converter(cfg, sync_stream, /*profile=*/true);
  if (!converter.initialize()) {
    cudaStreamDestroy(sync_stream);
    stop.store(true);
    ready.store(true);
    return;
  }

  std::cerr << "no-inference mode: TensorRT skipped, measuring the DAQIRI RX path alone\n";
  ready.store(true);

  daqiri::BurstParams* prev_burst = nullptr;
  uint32_t prev_n = 0;
  uint64_t batches = 0;
  std::vector<float> waits_ms;
  const auto t0 = std::chrono::steady_clock::now();
  constexpr double kReportIntervalSec = 1.0;
  auto interval_t0 = t0;
  uint64_t interval_images = 0;
  std::chrono::steady_clock::time_point t_first_job{};
  bool first_job_seen = false;
  bool first_result_logged = false;

  while (true) {
    InferenceJob job{};
    if (!queue.try_pop(job)) {
      if ((producer_done.load() || stop.load()) && queue.empty()) break;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }

    if (!first_job_seen) {
      first_job_seen = true;
      t_first_job = std::chrono::steady_clock::now();
    }

    const auto wait_start = std::chrono::steady_clock::now();
    bool sync_ok = true;
    if (converter.enabled()) {
      sync_ok = converter.enqueue(job) && converter.synchronize_and_profile();
    } else {
      sync_ok = cudaStreamWaitEvent(sync_stream, job.input_ready, 0) == cudaSuccess &&
                cudaStreamSynchronize(sync_stream) == cudaSuccess;
    }
    if (!sync_ok) {
      std::cerr << "no-inference consumer: reorder wait failed: "
                << cudaGetErrorString(cudaGetLastError()) << "\n";
      daqiri::free_all_packets_and_burst_rx(job.burst);
      stop.store(true);
      break;
    }
    waits_ms.push_back(std::chrono::duration<float, std::milli>(
                           std::chrono::steady_clock::now() - wait_start)
                           .count());

    if (prev_n > 0) {
      if (!first_result_logged) {
        const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        std::cerr << "inference_consumer_worker: first_application_result unix_ns=" << unix_ns
                  << "\n";
        first_result_logged = true;
      }
      sink.consume(nullptr, prev_n);
    }
    if (prev_burst != nullptr) daqiri::free_all_packets_and_burst_rx(prev_burst);
    prev_burst = job.burst;
    prev_n = job.batch_size;
    ++batches;

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

  if (prev_n > 0) {
    if (!first_result_logged) {
      const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      std::cerr << "inference_consumer_worker: first_application_result unix_ns=" << unix_ns
                << "\n";
    }
    sink.consume(nullptr, prev_n);
  }
  if (prev_burst != nullptr) daqiri::free_all_packets_and_burst_rx(prev_burst);

  const auto t_end = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t_end - t0).count();
  const double active_secs =
      first_job_seen ? std::chrono::duration<double>(t_end - t_first_job).count() : 0.0;
  std::ostringstream os;
  os << "inference_consumer_worker: " << batches << " inference batches in " << secs
     << " s (active " << active_secs << " s, idle-before-traffic " << (secs - active_secs)
     << " s)\n";
  if (!waits_ms.empty()) {
    std::sort(waits_ms.begin(), waits_ms.end());
    const double mean = std::accumulate(waits_ms.begin(), waits_ms.end(), 0.0) /
                        static_cast<double>(waits_ms.size());
    const auto pct = [&waits_ms](double p) {
      const size_t idx = std::min(
          waits_ms.size() - 1, static_cast<size_t>(p * (static_cast<double>(waits_ms.size()) - 1)));
      return waits_ms[idx];
    };
    os << "no-inference reorder wait (ms): mean=" << mean << " p50=" << pct(0.50)
       << " p99=" << pct(0.99) << " (per batch of " << cfg.images_per_batch
       << " images, n=" << waits_ms.size() << ")\n";
  }
  converter.append_profile(os, active_secs);
  std::cerr << os.str();
  cudaStreamDestroy(sync_stream);
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

  PostReorderConverter converter(cfg, inf_stream, /*profile=*/false);
  if (!converter.initialize()) {
    cudaStreamDestroy(inf_stream);
    stop.store(true);
    ready.store(true);
    return;
  }

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
  // Start of the active window: when the first job actually arrives.
  std::chrono::steady_clock::time_point t_first_job{};
  bool first_job_seen = false;
  std::vector<double> delivery_to_submit_us;
  std::vector<double> delivery_to_result_ms;
  std::chrono::steady_clock::time_point prev_delivery_at{};
  bool prev_delivery_valid = false;
  bool first_result_logged = false;

  while (true) {
    InferenceJob job{};
    if (!queue.try_pop(job)) {
      if (producer_done.load() && queue.empty()) break;
      if (stop.load() && queue.empty()) break;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }

    if (!first_job_seen) {
      first_job_seen = true;
      t_first_job = std::chrono::steady_clock::now();
    }

    const auto submit_at = std::chrono::steady_clock::now();
    float* host_prev = nullptr;
    uint32_t n_prev = 0;
    void* trt_input = job.dev_input;
    cudaEvent_t trt_input_ready = job.input_ready;
    bool conversion_ok = true;
    if (converter.enabled()) {
      conversion_ok = converter.enqueue(job);
      trt_input = converter.output();
      trt_input_ready = converter.ready_event();
    }
    const bool ok = conversion_ok && trt.infer(trt_input, job.batch_size, trt_input_ready,
                                               release_evt, host_prev, n_prev);
    if (ok) {
      delivery_to_submit_us.push_back(
          std::chrono::duration<double, std::micro>(submit_at - job.delivered_at).count());
      if (host_prev != nullptr) {
        if (!first_result_logged) {
          const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
          std::cerr << "inference_consumer_worker: first_application_result unix_ns=" << unix_ns
                    << "\n";
          first_result_logged = true;
        }
        if (prev_delivery_valid) {
          delivery_to_result_ms.push_back(std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - prev_delivery_at)
                                              .count());
        }
        sink.consume(host_prev, n_prev);
      }
      if (prev_burst != nullptr) {
        daqiri::free_all_packets_and_burst_rx(prev_burst);
      }
      prev_burst = job.burst;
      prev_delivery_at = job.delivered_at;
      prev_delivery_valid = true;
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
  if (host_final != nullptr) {
    if (!first_result_logged) {
      const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      std::cerr << "inference_consumer_worker: first_application_result unix_ns=" << unix_ns
                << "\n";
    }
    if (prev_delivery_valid) {
      delivery_to_result_ms.push_back(std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - prev_delivery_at)
                                          .count());
    }
    sink.consume(host_final, n_final);
  }
  if (prev_burst != nullptr) {
    daqiri::free_all_packets_and_burst_rx(prev_burst);
  }

  const auto t_end = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t_end - t0).count();
  // t0 is thread start, so secs includes the wait for the peer to start
  // transmitting. Report the active window separately -- it is the one that
  // describes the receiver rather than the harness's startup skew.
  const double active_secs =
      first_job_seen ? std::chrono::duration<double>(t_end - t_first_job).count() : 0.0;
  // One write, for the same reason as the producer summary above: the producer
  // may still be logging, and the runner greps these lines ^-anchored.
  std::ostringstream os;
  os << "inference_consumer_worker: " << trt.total_batches_inferred() << " inference batches in "
     << secs << " s (active " << active_secs << " s, idle-before-traffic " << (secs - active_secs)
     << " s)\n";

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
  const auto append_latency_summary = [&os](const char* label, std::vector<double> values,
                                            const char* unit) {
    if (values.empty()) return;
    const double first = values.front();
    std::sort(values.begin(), values.end());
    const double mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    const auto pct = [&values](double p) {
      const size_t idx = std::min(
          values.size() - 1, static_cast<size_t>(p * (static_cast<double>(values.size()) - 1)));
      return values[idx];
    };
    os << label << " (" << unit << "): first=" << first << " mean=" << mean << " p50=" << pct(0.50)
       << " p99=" << pct(0.99) << " n=" << values.size() << "\n";
  };
  append_latency_summary("batch delivery->inference submit", delivery_to_submit_us, "us");
  append_latency_summary("batch delivery->application-visible result", delivery_to_result_ms, "ms");
  std::cerr << os.str();

  cudaEventDestroy(release_evt);
  cudaStreamDestroy(inf_stream);
}

}  // namespace daqiri::apps::resnet
