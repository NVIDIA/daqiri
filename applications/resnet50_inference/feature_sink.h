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
// When pca_every_n_batches > 0, keeps a ring of recent vectors and prints
// headless PC1/PC2 projections to stdout every N batches.
// Predicted-class / softmax head is intentionally deferred.
class FeatureSink {
 public:
  FeatureSink(bool example_mode, int feature_dim, int top_k, std::vector<int> labels,
              int pca_every_n_batches = 8);

  void consume(const float* host_features, uint32_t n);

  void log_final_summary(double seconds);

  uint64_t images() const { return images_; }

 private:
  void push_pca_sample(const float* row);
  void run_pca_and_print();

  bool example_mode_;
  int feature_dim_;
  int top_k_;
  int pca_every_n_batches_;
  int num_classes_ = 0;
  std::vector<int> labels_;

  uint64_t images_ = 0;
  uint64_t batches_ = 0;
  uint64_t samples_printed_ = 0;
  uint64_t last_pca_batch_ = 0;

  std::vector<double> class_sum_;
  std::vector<uint64_t> class_count_;

  // Ring of recent feature vectors for incremental 2-component PCA (capacity 256).
  static constexpr size_t kPcaRingCap = 256;
  std::vector<float> pca_ring_;  // row-major [cap * feature_dim]
  size_t pca_ring_count_ = 0;
  size_t pca_ring_next_ = 0;
};

}  // namespace daqiri::apps::resnet
