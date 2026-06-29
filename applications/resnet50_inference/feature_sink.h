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

#include <cstdint>
#include <string>
#include <vector>

namespace daqiri::apps::resnet {

// Load a one-class-id-per-line labels sidecar (written by prepare_cifar10_pcap.py).
// Returns an empty vector on failure.
std::vector<int> load_labels(const std::string& path);

// Consumes host-resident ResNet feature vectors. In benchmark mode it just
// counts (throughput). In example mode it accumulates a per-class mean feature
// vector (label per image comes from the dataset labels sidecar, indexed by a
// monotonic image counter; the wire loopback is drop-free and in-order, so
// delivery order matches send order) and prints a few sample vectors. PCA is
// intentionally not implemented; per-class mean-feature stats are the cheap,
// dependency-free readout that shows the latent space separating by class.
class FeatureSink {
 public:
  FeatureSink(bool example_mode, int feature_dim, int top_k, std::vector<int> labels);

  // host_features is a row-major [n, feature_dim] FP32 matrix of one inference
  // batch's outputs (n images).
  void consume(const float* host_features, uint32_t n);

  void log_final_summary(double seconds) const;

  uint64_t images() const {
    return images_;
  }

 private:
  bool example_mode_;
  int feature_dim_;
  int top_k_;
  int num_classes_ = 0;
  std::vector<int> labels_;

  uint64_t images_ = 0;
  uint64_t batches_ = 0;
  uint64_t samples_printed_ = 0;

  std::vector<double> class_sum_;      // num_classes_ * feature_dim_
  std::vector<uint64_t> class_count_;  // num_classes_
};

}  // namespace daqiri::apps::resnet
