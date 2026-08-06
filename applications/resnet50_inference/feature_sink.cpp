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
#include <vector>

namespace daqiri::apps::resnet {

namespace {

constexpr const char* kCifar10Names[] = {
    "airplane", "automobile", "bird",  "cat",  "deer",
    "dog",      "frog",       "horse", "ship", "truck",
};

double dot(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return s;
}

void normalize(std::vector<double>& v) {
  const double n = std::sqrt(dot(v, v));
  if (n <= 0.0) return;
  for (double& x : v) x /= n;
}

// Cov matvec without forming the d×d matrix: (X^T X) v / (n-1) on mean-centered rows.
void cov_matvec(const std::vector<double>& centered, size_t n, int dim,
                const std::vector<double>& v, std::vector<double>& out) {
  out.assign(static_cast<size_t>(dim), 0.0);
  if (n < 2) return;
  std::vector<double> xv(n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const double* row = centered.data() + i * static_cast<size_t>(dim);
    double s = 0.0;
    for (int d = 0; d < dim; ++d) s += row[d] * v[static_cast<size_t>(d)];
    xv[i] = s;
  }
  const double scale = 1.0 / static_cast<double>(n - 1);
  for (size_t i = 0; i < n; ++i) {
    const double* row = centered.data() + i * static_cast<size_t>(dim);
    const double c = xv[i] * scale;
    for (int d = 0; d < dim; ++d) out[static_cast<size_t>(d)] += row[d] * c;
  }
}

void power_iteration(const std::vector<double>& centered, size_t n, int dim,
                     const std::vector<double>* deflate, std::vector<double>& out_pc,
                     int iters = 32) {
  out_pc.assign(static_cast<size_t>(dim), 0.0);
  if (n < 2 || dim <= 0) return;
  // Deterministic start vector.
  for (int d = 0; d < dim; ++d) out_pc[static_cast<size_t>(d)] = 1.0 / std::sqrt(static_cast<double>(dim));
  std::vector<double> tmp(static_cast<size_t>(dim));
  for (int it = 0; it < iters; ++it) {
    cov_matvec(centered, n, dim, out_pc, tmp);
    if (deflate != nullptr) {
      const double proj = dot(tmp, *deflate);
      for (int d = 0; d < dim; ++d) tmp[static_cast<size_t>(d)] -= proj * (*deflate)[static_cast<size_t>(d)];
    }
    normalize(tmp);
    out_pc.swap(tmp);
  }
}

}  // namespace

const char* cifar10_class_name(int class_id) {
  if (class_id >= 0 && class_id < 10) return kCifar10Names[class_id];
  return "unknown";
}

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

FeatureSink::FeatureSink(bool example_mode, int feature_dim, int top_k, std::vector<int> labels,
                         int pca_every_n_batches)
    : example_mode_(example_mode),
      feature_dim_(feature_dim),
      top_k_(std::min(top_k, feature_dim)),
      pca_every_n_batches_(pca_every_n_batches),
      labels_(std::move(labels)) {
  if (example_mode_ && !labels_.empty()) {
    num_classes_ = *std::max_element(labels_.begin(), labels_.end()) + 1;
    class_sum_.assign(static_cast<size_t>(num_classes_) * feature_dim_, 0.0);
    class_count_.assign(num_classes_, 0);
  }
  if (pca_every_n_batches_ > 0 && feature_dim_ > 0) {
    pca_ring_.assign(kPcaRingCap * static_cast<size_t>(feature_dim_), 0.0f);
  }
}

void FeatureSink::push_pca_sample(const float* row) {
  if (pca_every_n_batches_ <= 0 || feature_dim_ <= 0) return;
  float* dest = pca_ring_.data() + pca_ring_next_ * static_cast<size_t>(feature_dim_);
  std::copy(row, row + feature_dim_, dest);
  pca_ring_next_ = (pca_ring_next_ + 1) % kPcaRingCap;
  if (pca_ring_count_ < kPcaRingCap) ++pca_ring_count_;
}

void FeatureSink::run_pca_and_print() {
  const size_t n = pca_ring_count_;
  if (n < 2 || feature_dim_ <= 0) return;

  std::vector<double> mean(static_cast<size_t>(feature_dim_), 0.0);
  // Oldest→newest order for stable indexing in print.
  const size_t start =
      (pca_ring_count_ < kPcaRingCap) ? 0 : pca_ring_next_;
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (start + i) % kPcaRingCap;
    const float* row = pca_ring_.data() + idx * static_cast<size_t>(feature_dim_);
    for (int d = 0; d < feature_dim_; ++d) mean[static_cast<size_t>(d)] += row[d];
  }
  for (double& m : mean) m /= static_cast<double>(n);

  std::vector<double> centered(n * static_cast<size_t>(feature_dim_));
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (start + i) % kPcaRingCap;
    const float* row = pca_ring_.data() + idx * static_cast<size_t>(feature_dim_);
    double* dest = centered.data() + i * static_cast<size_t>(feature_dim_);
    for (int d = 0; d < feature_dim_; ++d) {
      dest[d] = static_cast<double>(row[d]) - mean[static_cast<size_t>(d)];
    }
  }

  std::vector<double> pc1, pc2;
  power_iteration(centered, n, feature_dim_, nullptr, pc1);
  power_iteration(centered, n, feature_dim_, &pc1, pc2);

  std::cout << std::fixed << std::setprecision(6);
  for (size_t i = 0; i < n; ++i) {
    const double* row = centered.data() + i * static_cast<size_t>(feature_dim_);
    double p1 = 0.0, p2 = 0.0;
    for (int d = 0; d < feature_dim_; ++d) {
      p1 += row[d] * pc1[static_cast<size_t>(d)];
      p2 += row[d] * pc2[static_cast<size_t>(d)];
    }
    std::cout << "pc1=" << p1 << " pc2=" << p2 << "\n";
  }
  std::cout.flush();
  last_pca_batch_ = batches_;
}

void FeatureSink::consume(const float* host_features, uint32_t n) {
  ++batches_;

  for (uint32_t i = 0; i < n; ++i) {
    const float* row = host_features + static_cast<size_t>(i) * feature_dim_;
    push_pca_sample(row);

    if (example_mode_ && !labels_.empty()) {
      const int label = labels_[static_cast<size_t>(images_ % labels_.size())];
      if (label >= 0 && label < num_classes_) {
        double* acc = class_sum_.data() + static_cast<size_t>(label) * feature_dim_;
        for (int d = 0; d < feature_dim_; ++d) acc[d] += row[d];
        ++class_count_[label];
      }

      if (samples_printed_ < 2) {
        std::ostringstream os;
        os << "  feature[image " << images_ << ", class " << label << " ("
           << cifar10_class_name(label) << ")] = [";
        const int show = std::min(top_k_, feature_dim_);
        os << std::fixed << std::setprecision(4);
        for (int d = 0; d < show; ++d) os << (d ? ", " : "") << row[d];
        os << ", ...] (dim=" << feature_dim_ << ")";
        std::cerr << os.str() << "\n";
        ++samples_printed_;
      }
    }
    ++images_;
  }

  if (pca_every_n_batches_ > 0 &&
      (batches_ % static_cast<uint64_t>(pca_every_n_batches_) == 0)) {
    run_pca_and_print();
  }
}

void FeatureSink::log_final_summary(double seconds) {
  // Flush a final PCA pass if we buffered samples since the last print.
  if (pca_every_n_batches_ > 0 && batches_ > last_pca_batch_ && pca_ring_count_ >= 2) {
    run_pca_and_print();
  }

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
    os << "  class " << std::setw(2) << c << " " << std::setw(10) << cifar10_class_name(c)
       << " (n=" << std::setw(6) << cnt << "): mean=[" << std::fixed << std::setprecision(4);
    for (int d = 0; d < top_k_; ++d) {
      os << (d ? ", " : "") << acc[d] / static_cast<double>(cnt);
    }
    os << ", ...]  |mean|=" << std::sqrt(norm);
    std::cerr << os.str() << "\n";
  }
  std::cerr << "(Distinct per-class mean vectors indicate ResNet separates the "
               "classes in latent space. Class names are ground-truth from the "
               "dataset sidecar; predicted-class softmax is a follow-up.)\n";
}

}  // namespace daqiri::apps::resnet
