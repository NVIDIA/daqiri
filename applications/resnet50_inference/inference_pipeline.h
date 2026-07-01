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

#pragma once

#include <atomic>

#include "app_config.h"
#include "feature_sink.h"

namespace daqiri::apps::resnet {

// Single RX thread: dequeue DAQIRI bursts, reassemble each image on the GPU via
// the sequence-number reorder kernel, batch images into a contiguous FP32 NCHW
// buffer, run TensorRT inference, and hand the resulting feature vectors to the
// sink. All GPU work shares one CUDA stream so the reorder kernel orders before
// the inference; the burst is freed only after the stream drains (so the reorder
// has finished reading the burst's device pointers).
//
// `ready` is set true once the (slow, first-run) TensorRT engine build/load and
// all GPU buffers are initialized, so the caller can defer starting the TX and
// the run-duration timer until the receive path is actually ready to consume --
// a cold engine build can otherwise exceed the whole --seconds window. On any
// init failure the worker sets `stop` (which also releases a caller waiting on
// `ready`).
//
// `expected_images` (example mode) is the dataset size: the worker drains the RX
// ring until it has reassembled that many images, then flushes the final partial
// inference batch -- so the whole dataset is inferred even though inference runs
// slower than line rate, without abandoning un-drained bursts or an un-inferred
// partial batch when a wall-clock `stop` fires mid-stream. Pass 0 in benchmark
// mode to run until `stop`.
//
// `tx_done` lets the TX signal it has finished sending; it is only used as a
// drop-tolerant safety exit (quiescence) once at least one image has been
// received, so a stray dropped packet can't make the worker hang waiting for an
// image count it will never reach. `ready` is set true once the (slow, first-run)
// TensorRT engine build/load and all GPU buffers are initialized, so the caller
// can defer starting the TX until the receive path is ready to consume. On any
// init failure the worker sets `stop` (which also releases a caller waiting on
// `ready`).
void inference_rx_worker(const AppConfig& cfg, FeatureSink& sink, uint64_t expected_images,
                         std::atomic<bool>& ready, std::atomic<bool>& tx_done,
                         std::atomic<bool>& stop);

}  // namespace daqiri::apps::resnet
