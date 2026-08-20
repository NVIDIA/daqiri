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
#include <cstdint>

#include "app_config.h"
#include "feature_sink.h"
#include "spsc_queue.h"

namespace daqiri::apps::resnet {

// RX producer: dequeue DAQIRI reordered bursts and push InferenceJobs onto the
// SPSC ring. Does not own TRT or free bursts after a successful push (the
// consumer frees them one batch late). Non-REORDERED bursts are freed
// immediately. Example mode backpressures on a full queue; bench mode drops.
void rx_producer_worker(const AppConfig& cfg, InferenceQueue<kInferenceQueueCap>& queue,
                        uint64_t expected_images, std::atomic<bool>& producer_done,
                        std::atomic<bool>& stop);

// Inference consumer: owns inf_stream + TrtRunner. Sets `ready` after engine
// build. Pops jobs, waits on burst->event inside infer(), sinks features, and
// frees prev_burst only when infer() returns true.
void inference_consumer_worker(const AppConfig& cfg, FeatureSink& sink,
                               InferenceQueue<kInferenceQueueCap>& queue,
                               std::atomic<bool>& producer_done, std::atomic<bool>& ready,
                               std::atomic<bool>& stop);

}  // namespace daqiri::apps::resnet
