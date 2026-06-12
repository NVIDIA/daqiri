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

// A bounded, lock-free ring buffer of void* pointers. This is a direct,
// dependency-free reimplementation of the classic DPDK rte_ring algorithm
// (four monotonic head/tail cursors: prod_head/prod_tail/cons_head/cons_tail)
// so that daqiri_common and the non-DPDK engines (rdma, ibverbs, socket) no
// longer need libdpdk just for a fast pointer ring.
//
// Semantics mirror rte_ring exactly:
//   * capacity is a power of two; the usable count is capacity-1 (one slot is
//     reserved to disambiguate full from empty), so a ring sized
//     round_up_pow2(n+1) holds n items -- the same sizing rule callers used
//     with rte_align32pow2(n+1).
//   * bulk enqueue/dequeue are all-or-nothing: they move exactly n items or 0.
//   * MPMC (multi-producer/multi-consumer) and SPSC (single on both ends)
//     modes are selected at creation, matching rte_ring's
//     RING_F_SP_ENQ | RING_F_SC_DEQ flags.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace daqiri {

enum class RingMode { MPMC, SPSC };

namespace detail {

// Spin-wait hint while another producer/consumer finishes publishing its
// reserved slots. Matches rte_pause().
inline void ring_cpu_pause() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_acquire);
#endif
}

inline uint32_t round_up_pow2(uint32_t v) {
  if (v < 2) {
    return 2;
  }
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
}

}  // namespace detail

class Ring {
 public:
  // count is rounded up to a power of two; usable capacity is (rounded-1).
  // mode selects MPMC vs SPSC. Returns nullptr on allocation failure.
  static Ring* create(const char* /*name*/, unsigned count, RingMode mode) {
    const uint32_t size = detail::round_up_pow2(static_cast<uint32_t>(count));
    void* mem = nullptr;
    const size_t bytes = sizeof(Ring) + static_cast<size_t>(size) * sizeof(void*);
    if (posix_memalign(&mem, kCacheline, bytes) != 0) {
      return nullptr;
    }
    Ring* r = new (mem) Ring();
    r->size_ = size;
    r->mask_ = size - 1;
    r->single_ = (mode == RingMode::SPSC);
    r->ring_ = reinterpret_cast<void**>(reinterpret_cast<char*>(mem) + sizeof(Ring));
    return r;
  }

  static void free(Ring* r) {
    if (r == nullptr) {
      return;
    }
    r->~Ring();
    std::free(r);
  }

  bool enqueue(void* obj) {
    return enqueue_bulk(&obj, 1) == 1;
  }
  bool dequeue(void** obj) {
    return dequeue_bulk(obj, 1) == 1;
  }

  // All-or-nothing: returns n if all enqueued, else 0.
  unsigned enqueue_bulk(void* const* objs, unsigned n) {
    if (n == 0) {
      return 0;
    }
    uint32_t prod_head;
    const uint32_t prod_next = move_prod_head(n, &prod_head);
    if (prod_next == prod_head) {
      return 0;
    }  // not enough free space

    for (unsigned i = 0; i < n; i++) {
      ring_[(prod_head + i) & mask_] = objs[i];
    }

    if (!single_) {
      // Wait until preceding producers have committed, preserving FIFO publish
      // order. Relaxed is sufficient here: the release store below, paired with
      // the acquire fence + acquire tail-load in move_cons_head/move_prod_head,
      // is what establishes the cross-thread happens-before for the slot writes.
      while (prod_tail_.load(std::memory_order_relaxed) != prod_head) {
        detail::ring_cpu_pause();
      }
    }
    prod_tail_.store(prod_next, std::memory_order_release);
    return n;
  }

  // All-or-nothing: returns n if all dequeued, else 0.
  unsigned dequeue_bulk(void** objs, unsigned n) {
    if (n == 0) {
      return 0;
    }
    uint32_t cons_head;
    const uint32_t cons_next = move_cons_head(n, &cons_head);
    if (cons_next == cons_head) {
      return 0;
    }  // not enough entries

    for (unsigned i = 0; i < n; i++) {
      objs[i] = ring_[(cons_head + i) & mask_];
    }

    if (!single_) {
      // Relaxed for the same reason as the producer side.
      while (cons_tail_.load(std::memory_order_relaxed) != cons_head) {
        detail::ring_cpu_pause();
      }
    }
    cons_tail_.store(cons_next, std::memory_order_release);
    return n;
  }

  unsigned count() const {
    const uint32_t prod = prod_tail_.load(std::memory_order_relaxed);
    const uint32_t cons = cons_tail_.load(std::memory_order_relaxed);
    return (prod - cons) & mask_;
  }

  unsigned capacity() const {
    return mask_;
  }  // usable slots

 private:
  static constexpr size_t kCacheline = 64;

  Ring() = default;
  ~Ring() = default;
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  // Reserve n slots for production. On success returns the new head and writes
  // the old head to *old_head. On failure (insufficient space) returns
  // *old_head unchanged (caller detects next==head).
  uint32_t move_prod_head(unsigned n, uint32_t* old_head) {
    uint32_t head = prod_head_.load(std::memory_order_relaxed);
    uint32_t next;
    do {
      // Acquire fence + acquire load (the DPDK rte_ring C11 recipe). This is
      // load-bearing on weakly-ordered CPUs (aarch64): it makes the slot reads
      // that a consumer performs before releasing cons_tail visible to this
      // producer before it reuses those slots, and conversely chains slot-write
      // visibility through to the consumer. Dropping the fence corrupts data at
      // -O2 on ARM (verified) even though it "works" on x86.
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint32_t cons = cons_tail_.load(std::memory_order_acquire);
      const uint32_t free_entries = mask_ + cons - head;  // unsigned wraparound
      if (n > free_entries) {
        *old_head = head;
        return head;  // signal failure
      }
      next = head + n;
      if (single_) {
        prod_head_.store(next, std::memory_order_relaxed);
        break;
      }
    } while (!prod_head_.compare_exchange_weak(head, next, std::memory_order_relaxed,
                                               std::memory_order_relaxed));
    *old_head = head;
    return next;
  }

  uint32_t move_cons_head(unsigned n, uint32_t* old_head) {
    uint32_t head = cons_head_.load(std::memory_order_relaxed);
    uint32_t next;
    do {
      // Acquire fence + acquire load -- see the note in move_prod_head.
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint32_t prod = prod_tail_.load(std::memory_order_acquire);
      const uint32_t entries = prod - head;  // unsigned wraparound
      if (n > entries) {
        *old_head = head;
        return head;  // signal failure
      }
      next = head + n;
      if (single_) {
        cons_head_.store(next, std::memory_order_relaxed);
        break;
      }
    } while (!cons_head_.compare_exchange_weak(head, next, std::memory_order_relaxed,
                                               std::memory_order_relaxed));
    *old_head = head;
    return next;
  }

  // Producer and consumer cursors live on separate cache lines to avoid
  // false sharing between the two sides.
  alignas(kCacheline) std::atomic<uint32_t> prod_head_{0};
  std::atomic<uint32_t> prod_tail_{0};
  alignas(kCacheline) std::atomic<uint32_t> cons_head_{0};
  std::atomic<uint32_t> cons_tail_{0};
  alignas(kCacheline) uint32_t mask_ = 0;
  uint32_t size_ = 0;
  bool single_ = false;
  void** ring_ = nullptr;  // points just past this object in the same allocation
};

}  // namespace daqiri
