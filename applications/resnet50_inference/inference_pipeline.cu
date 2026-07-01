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

#include "bench_pipeline.h"
#include "raw_bench_common.h"
#include "trt_runner.hpp"

#include <daqiri/daqiri.h>

namespace daqiri::apps::resnet {

void inference_rx_worker(const AppConfig& cfg, FeatureSink& sink, uint64_t expected_images,
                         std::atomic<bool>& ready, std::atomic<bool>& tx_done,
                         std::atomic<bool>& stop) {
  if (!daqiri::bench::set_current_thread_affinity(cfg.rx.cpu_core, "resnet_rx")) {
    stop.store(true);
    return;
  }

  cudaStream_t stream = nullptr;
  if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
    std::cerr << "inference_rx_worker: cudaStreamCreate failed\n";
    stop.store(true);
    return;
  }

  TrtRunner trt(cfg.trt, stream);
  trt.initialize();

  // One image's worth of reorder slots (packets_per_image), shared CUDA stream.
  daqiri::bench::ReorderPipeline pipe;
  if (!pipe.init(daqiri::bench::ReorderMode::SeqReorder, cfg.packets_per_image, cfg.out_payload_len,
                 cfg.payload_byte_offset, cfg.seq_bit_offset, cfg.seq_bit_width,
                 /*staging_needed=*/false, stream) ||
      !pipe.enabled()) {
    std::cerr << "inference_rx_worker: reorder pipeline init failed\n";
    cudaStreamDestroy(stream);
    stop.store(true);
    return;
  }

  const size_t img_bytes = cfg.image_bytes;
  const uint32_t batch = cfg.images_per_batch;
  char* d_nchw_batch = nullptr;
  if (cudaMalloc(&d_nchw_batch, static_cast<size_t>(batch) * img_bytes) != cudaSuccess) {
    std::cerr << "inference_rx_worker: d_nchw_batch alloc failed\n";
    cudaStreamDestroy(stream);
    stop.store(true);
    return;
  }

  cudaEvent_t input_ready = nullptr;
  cudaEvent_t release_evt = nullptr;
  cudaEventCreateWithFlags(&input_ready, cudaEventDisableTiming);
  cudaEventCreateWithFlags(&release_evt, cudaEventDisableTiming);

  const int port_id = daqiri::get_port_id(cfg.rx.interface_name);
  if (port_id < 0) {
    std::cerr << "inference_rx_worker: invalid RX interface " << cfg.rx.interface_name << "\n";
    stop.store(true);
  }
  const int queue_id = cfg.rx.queue_id >= 0 ? cfg.rx.queue_id : 0;

  // img_filled / img_in_batch persist ACROSS bursts so an image (or a batch)
  // that spans a burst boundary still reassembles correctly. The per-burst
  // finish_batch() + stream sync run the reorder kernel before the burst is
  // freed, so the persistent reorder buffer accumulates safely.
  uint32_t img_filled = 0;
  uint32_t img_in_batch = 0;
  uint64_t images_completed = 0;  // total images reassembled (across all batches)

  // Receive path is fully initialized (engine built/loaded, buffers allocated):
  // release the caller so it can start the TX and the run-duration timer.
  ready.store(true);

  // The NORMAL example-mode exit is the expected-image count below. This idle
  // backstop only catches a genuine stall (e.g. a dropped packet means the count
  // never completes): a long run of empty polls (~100us each) after the TX is
  // done and we have already received at least one image. It must be far longer
  // than the sub-second gaps between TX bursts (the per-packet H2D copies make
  // the TX bursty), or it would quiesce mid-stream and drop the tail -- which is
  // exactly what a 200ms window did (stopped at 243/256). Gating on
  // images_completed>0 also prevents quiescing before the first frame arrives
  // (the TX finishes *queuing* well before the first frame reaches the RX ring).
  constexpr uint32_t kQuiesceIters = 50000;  // ~5 s of continuous empty polls
  uint32_t idle_polls = 0;

  const auto t0 = std::chrono::steady_clock::now();
  while (!stop.load()) {
    daqiri::BurstParams* burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id, queue_id) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      if (tx_done.load() && images_completed > 0 && ++idle_polls >= kQuiesceIters) break;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    idle_polls = 0;

    const int num_pkts = static_cast<int>(daqiri::get_num_packets(burst));
    pipe.reset_batch();
    for (int i = 0; i < num_pkts; ++i) {
      pipe.add_device_packet(daqiri::get_segment_packet_ptr(burst, 0, i));
      ++img_filled;
      if (img_filled == cfg.packets_per_image) {
        // Scatter this burst's contribution to the current image into the
        // persistent reorder buffer, then copy the completed image into its
        // slot in the NCHW inference batch (all on the shared stream).
        const void* ordered = pipe.finish_batch();
        if (ordered != nullptr) {
          cudaMemcpyAsync(d_nchw_batch + static_cast<size_t>(img_in_batch) * img_bytes, ordered,
                          img_bytes, cudaMemcpyDeviceToDevice, stream);
        }
        ++img_in_batch;
        ++images_completed;
        img_filled = 0;
        pipe.reset_batch();

        if (img_in_batch == batch) {
          cudaEventRecord(input_ready, stream);
          float* host_prev = nullptr;
          uint32_t n_prev = 0;
          trt.infer(reinterpret_cast<float*>(d_nchw_batch), batch, input_ready, release_evt,
                    host_prev, n_prev);
          if (host_prev != nullptr) sink.consume(host_prev, n_prev);
          img_in_batch = 0;
        }
      }
    }
    // Flush the in-progress image's this-burst packets into the reorder buffer
    // BEFORE freeing the burst (its device pointers die at free).
    pipe.finish_batch();
    pipe.reset_batch();

    // Safe-free barrier: the reorder kernel reads the burst's device pointers,
    // so the burst must not be freed until the stream drains.
    cudaStreamSynchronize(stream);
    daqiri::free_all_packets_and_burst_rx(burst);

    // Normal example-mode exit: the whole dataset has been reassembled. (The
    // last full inference batch already fired inside the loop; any trailing
    // partial batch is flushed below.)
    if (expected_images > 0 && images_completed >= expected_images) break;
  }

  // Flush a final partial inference batch (img_in_batch < batch): the dataset
  // size need not be a multiple of images_per_batch, so the last few completed
  // images may not have triggered an infer in the loop. This inference also
  // inline-delivers the previous full batch's now-ready features.
  if (img_in_batch > 0) {
    cudaEventRecord(input_ready, stream);
    float* host_prev = nullptr;
    uint32_t n_prev = 0;
    trt.infer(reinterpret_cast<float*>(d_nchw_batch), img_in_batch, input_ready, release_evt,
              host_prev, n_prev);
    if (host_prev != nullptr) sink.consume(host_prev, n_prev);
    img_in_batch = 0;
  }

  // Drain the final in-flight inference batch so its features are not lost.
  cudaStreamSynchronize(stream);
  float* host_final = nullptr;
  uint32_t n_final = 0;
  trt.drain_final(host_final, n_final);
  if (host_final != nullptr) sink.consume(host_final, n_final);

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cerr << "inference_rx_worker: " << trt.total_batches_inferred() << " inference batches in "
            << secs << " s\n";

  // Per-batch inference latency (batch-ready -> features on host). Report
  // mean/p50/p99 over all batches in a greppable line for the bench sweep.
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
              << " p99=" << pct(0.99) << " (per batch of " << batch << " images, n=" << lat.size()
              << ")\n";
  }

  cudaFree(d_nchw_batch);
  cudaEventDestroy(input_ready);
  cudaEventDestroy(release_evt);
  cudaStreamDestroy(stream);
}

}  // namespace daqiri::apps::resnet
