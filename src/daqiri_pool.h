/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

// A fixed-capacity object pool: a single contiguous slab of n equal-sized
// elements plus a lock-free free-list (daqiri::Ring) holding the pointers to
// the currently-available elements. This is a dependency-free replacement for
// the generic rte_mempool usage in daqiri_common and the non-DPDK engines
// (the metadata / burst-descriptor pools), which never relied on rte_mbuf
// semantics -- they only needed get()/put()/avail/in-use on fixed-size slabs.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "src/daqiri_ring.h"

namespace daqiri {

class ObjectPool {
 public:
  // Allocates n elements of at least elt_size bytes each (stride rounded up to
  // a cache line to avoid false sharing between neighbouring elements) and
  // pre-populates the free-list. numa_node (>=0) pins both the slab and the
  // free-list ring to that NUMA node when libnuma is available (mirroring
  // rte_mempool_create's socket argument); -1 leaves placement to first-touch.
  // Returns nullptr on failure.
  static ObjectPool* create(const char* /*name*/, unsigned n, size_t elt_size, int numa_node = -1) {
    auto* p = new (std::nothrow) ObjectPool();
    if (p == nullptr) {
      return nullptr;
    }

    const size_t stride = (elt_size + kCacheline - 1) & ~(kCacheline - 1);
    if (posix_memalign(&p->base_, kCacheline, static_cast<size_t>(n) * stride) != 0) {
      delete p;
      return nullptr;
    }
    detail::numa_bind(p->base_, static_cast<size_t>(n) * stride, numa_node);
    // Free-list capacity must hold all n elements; round_up_pow2(n+1)-1 >= n.
    p->free_ = Ring::create("objpool", n + 1, RingMode::MPMC, numa_node);
    if (p->free_ == nullptr) {
      std::free(p->base_);
      delete p;
      return nullptr;
    }

    p->n_ = n;
    p->stride_ = stride;
    auto* base = static_cast<char*>(p->base_);
    for (unsigned i = 0; i < n; i++) {
      p->free_->enqueue(base + static_cast<size_t>(i) * stride);
    }
    return p;
  }

  static void free(ObjectPool* p) {
    if (p == nullptr) {
      return;
    }
    if (p->free_ != nullptr) {
      Ring::free(p->free_);
    }
    if (p->base_ != nullptr) {
      std::free(p->base_);
    }
    delete p;
  }

  // Returns true and sets *obj on success; false if the pool is exhausted.
  bool get(void** obj) {
    return free_->dequeue(obj);
  }
  void put(void* obj) {
    free_->enqueue(obj);
  }

  unsigned avail_count() const {
    return free_->count();
  }
  unsigned in_use_count() const {
    return n_ - free_->count();
  }
  unsigned size() const {
    return n_;
  }

 private:
  static constexpr size_t kCacheline = 64;

  ObjectPool() = default;
  ~ObjectPool() = default;
  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  void* base_ = nullptr;
  Ring* free_ = nullptr;
  unsigned n_ = 0;
  size_t stride_ = 0;
};

}  // namespace daqiri
