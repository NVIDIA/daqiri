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

#include "feature_sink.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace daqiri::apps::resnet {

std::vector<int> load_labels(const std::string& path) {
  std::vector<int> labels;
  std::ifstream f(path);
  if (!f) {
    std::cerr << "load_labels: cannot open " << path << "\n";
    return labels;
  }
  int v;
  while (f >> v) labels.push_back(v);
  std::cerr << "load_labels: " << labels.size() << " labels from " << path << "\n";
  return labels;
}

FeatureSink::FeatureSink(bool example_mode, int feature_dim, int top_k, std::vector<int> labels)
    : example_mode_(example_mode),
      feature_dim_(feature_dim),
      top_k_(std::min(top_k, feature_dim)),
      labels_(std::move(labels)) {
  if (example_mode_ && !labels_.empty()) {
    num_classes_ = *std::max_element(labels_.begin(), labels_.end()) + 1;
    class_sum_.assign(static_cast<size_t>(num_classes_) * feature_dim_, 0.0);
    class_count_.assign(num_classes_, 0);
  }
}

void FeatureSink::consume(const float* host_features, uint32_t n) {
  ++batches_;
  if (!example_mode_ || labels_.empty()) {
    images_ += n;
    return;
  }

  for (uint32_t i = 0; i < n; ++i) {
    const float* row = host_features + static_cast<size_t>(i) * feature_dim_;
    const int label = labels_[static_cast<size_t>(images_ % labels_.size())];
    if (label >= 0 && label < num_classes_) {
      double* acc = class_sum_.data() + static_cast<size_t>(label) * feature_dim_;
      for (int d = 0; d < feature_dim_; ++d) acc[d] += row[d];
      ++class_count_[label];
    }
    ++images_;

    // Print a couple of raw feature vectors so the run shows concrete output.
    if (samples_printed_ < 2) {
      std::ostringstream os;
      os << "  feature[image " << (images_ - 1) << ", class " << label << "] = [";
      const int show = std::min(top_k_, feature_dim_);
      os << std::fixed << std::setprecision(4);
      for (int d = 0; d < show; ++d) os << (d ? ", " : "") << row[d];
      os << ", ...] (dim=" << feature_dim_ << ")";
      std::cerr << os.str() << "\n";
      ++samples_printed_;
    }
  }
}

void FeatureSink::log_final_summary(double seconds) const {
  const double imgs_per_s = seconds > 0 ? static_cast<double>(images_) / seconds : 0.0;
  std::cerr << "\n=== ResNet inference summary ===\n";
  std::cerr << "images=" << images_ << " batches=" << batches_ << " seconds=" << std::fixed
            << std::setprecision(2) << seconds << " => " << imgs_per_s << " img/s\n";

  if (!example_mode_ || labels_.empty() || num_classes_ == 0) return;

  std::cerr << "\nPer-class mean-feature stats (first " << top_k_
            << " dims + L2 norm of the mean vector):\n";
  for (int c = 0; c < num_classes_; ++c) {
    const uint64_t cnt = class_count_[c];
    if (cnt == 0) continue;
    const double* acc = class_sum_.data() + static_cast<size_t>(c) * feature_dim_;
    double norm = 0.0;
    for (int d = 0; d < feature_dim_; ++d) {
      const double mean = acc[d] / static_cast<double>(cnt);
      norm += mean * mean;
    }
    std::ostringstream os;
    os << "  class " << std::setw(2) << c << " (n=" << std::setw(6) << cnt << "): mean=["
       << std::fixed << std::setprecision(4);
    for (int d = 0; d < top_k_; ++d) {
      os << (d ? ", " : "") << acc[d] / static_cast<double>(cnt);
    }
    os << ", ...]  |mean|=" << std::sqrt(norm);
    std::cerr << os.str() << "\n";
  }
  std::cerr << "(Distinct per-class mean vectors indicate ResNet separates the "
               "classes in latent space.)\n";
}

}  // namespace daqiri::apps::resnet
