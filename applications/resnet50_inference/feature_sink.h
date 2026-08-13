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

// CIFAR-10 class id → name (ground-truth labels from the dataset sidecar).
const char* cifar10_class_name(int class_id);

// Load a one-class-id-per-line labels sidecar (written by prepare_cifar10_pcap.py).
std::vector<int> load_labels(const std::string& path);

// Consumes host-resident ResNet feature vectors. Example mode accumulates a
// per-class mean feature vector (label from the sidecar) and prints class names.
// PCA is intentionally not implemented; per-class mean-feature stats are the
// cheap, dependency-free readout that shows the latent space separating by
// class. A projection would have to run off the inference consumer thread to
// avoid stalling it.
// Predicted-class / softmax head is intentionally deferred.
class FeatureSink {
 public:
  FeatureSink(bool example_mode, int feature_dim, int top_k, std::vector<int> labels);

  void consume(const float* host_features, uint32_t n);

  void log_final_summary(double seconds);

  uint64_t images() const { return images_; }

 private:
  bool example_mode_;
  int feature_dim_;
  int top_k_;
  int num_classes_ = 0;
  std::vector<int> labels_;

  uint64_t images_ = 0;
  uint64_t batches_ = 0;
  uint64_t samples_printed_ = 0;

  std::vector<double> class_sum_;
  std::vector<uint64_t> class_count_;
};

}  // namespace daqiri::apps::resnet
