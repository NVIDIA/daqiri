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

// Microbenchmark comparing the libdpdk-free daqiri::Ring / daqiri::ObjectPool
// primitives against DPDK's rte_ring / rte_mempool. The daqiri arm always
// builds (the primitives are header-only); the rte arm is compiled only when
// BENCH_HAVE_DPDK is defined (a DPDK-enabled build). Run it on a DPDK box to
// confirm parity:
//
//   ./pool_ring_bench
//
// Methodology: each cell pins producer/consumer threads to distinct cores, runs
// a warmup pass, then a timed pass of a fixed number of operations, and reports
// throughput (Mops/s) and per-op latency (ns/op). Each cell is repeated and the
// median is reported. Bulk cells move N pointers per call.

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "src/daqiri_ring.h"
#include "src/daqiri_pool.h"

#if BENCH_HAVE_DPDK
#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#endif

namespace {

constexpr unsigned kRingSize = 1024;       // power of two
constexpr long kOpsPerThread = 2'000'000;  // items enqueued per producer
constexpr int kReps = 3;

void pin_thread(std::thread& t, int core) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

struct Result {
  double mops;   // million ops / sec (total enqueue+dequeue counted as 2 ops? no: 1 item moved)
  double ns_op;  // ns per item moved end-to-end
};

// ---------------------------------------------------------------------------
// daqiri::Ring benchmark
// ---------------------------------------------------------------------------
Result bench_daqiri_ring(int producers, int consumers, unsigned bulk, daqiri::RingMode mode) {
  std::vector<double> mops_runs;
  const long total = kOpsPerThread * producers;
  for (int rep = 0; rep < kReps; rep++) {
    daqiri::Ring* r = daqiri::Ring::create("bench", kRingSize, mode);
    // Counting is local per thread (no per-op shared atomic, which would itself
    // be the bottleneck). Consumers drain until every producer has finished and
    // the ring is empty.
    std::atomic<int> producers_done{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    for (int p = 0; p < producers; p++) {
      threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        void* buf[64];
        for (unsigned i = 0; i < bulk; i++) {
          buf[i] = reinterpret_cast<void*>(uintptr_t(i + 1));
        }
        long sent = 0;
        while (sent < kOpsPerThread) {
          if (bulk == 1) {
            if (r->enqueue(buf[0])) {
              sent++;
            }
          } else {
            const unsigned n = static_cast<unsigned>(std::min<long>(bulk, kOpsPerThread - sent));
            if (r->enqueue_bulk(buf, n) == n) {
              sent += n;
            }
          }
        }
        producers_done.fetch_add(1, std::memory_order_release);
      });
    }
    for (int c = 0; c < consumers; c++) {
      threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        void* buf[64];
        for (;;) {
          bool got_any = false;
          if (bulk == 1) {
            void* o;
            if (r->dequeue(&o)) {
              got_any = true;
            }
          } else {
            if (r->dequeue_bulk(buf, bulk)) {
              got_any = true;
            }
          }
          if (!got_any && producers_done.load(std::memory_order_acquire) == producers &&
              r->count() == 0) {
            break;
          }
        }
      });
    }

    int core = 0;
    for (auto& t : threads) {
      pin_thread(t, core++ % std::max(1u, std::thread::hardware_concurrency()));
    }

    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) {
      t.join();
    }
    auto end = std::chrono::steady_clock::now();

    const double secs = std::chrono::duration<double>(end - start).count();
    mops_runs.push_back(total / secs / 1e6);
    daqiri::Ring::free(r);
  }
  const double mops = median(mops_runs);
  return {mops, 1e3 / mops};
}

Result bench_daqiri_pool(int threads_n) {
  std::vector<double> mops_runs;
  const long total = kOpsPerThread * threads_n;
  for (int rep = 0; rep < kReps; rep++) {
    daqiri::ObjectPool* pool = daqiri::ObjectPool::create("bench", 4096, 64);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < threads_n; t++) {
      threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        long done = 0;
        void* slots[16];
        while (done < kOpsPerThread) {
          int held = 0;
          for (int i = 0; i < 16 && pool->get(&slots[i]); i++) {
            held++;
          }
          for (int i = 0; i < held; i++) {
            pool->put(slots[i]);
          }
          done += held;
        }
      });
    }
    int core = 0;
    for (auto& t : threads) {
      pin_thread(t, core++ % std::max(1u, std::thread::hardware_concurrency()));
    }
    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) {
      t.join();
    }
    auto end = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(end - start).count();
    mops_runs.push_back(total / secs / 1e6);
    daqiri::ObjectPool::free(pool);
  }
  const double mops = median(mops_runs);
  return {mops, 1e3 / mops};
}

#if BENCH_HAVE_DPDK
// ---------------------------------------------------------------------------
// rte_ring / rte_mempool benchmark (same harness)
// ---------------------------------------------------------------------------
Result bench_rte_ring(int producers, int consumers, unsigned bulk, unsigned flags) {
  std::vector<double> mops_runs;
  const long total = kOpsPerThread * producers;
  for (int rep = 0; rep < kReps; rep++) {
    rte_ring* r = rte_ring_create("bench_rte", kRingSize, rte_socket_id(), flags);
    std::atomic<int> producers_done{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; p++) {
      threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        void* buf[64];
        for (unsigned i = 0; i < bulk; i++) {
          buf[i] = reinterpret_cast<void*>(uintptr_t(i + 1));
        }
        long sent = 0;
        while (sent < kOpsPerThread) {
          if (bulk == 1) {
            if (rte_ring_enqueue(r, buf[0]) == 0) {
              sent++;
            }
          } else {
            const unsigned n = static_cast<unsigned>(std::min<long>(bulk, kOpsPerThread - sent));
            if (rte_ring_enqueue_bulk(r, buf, n, nullptr) == n) {
              sent += n;
            }
          }
        }
        producers_done.fetch_add(1, std::memory_order_release);
      });
    }
    for (int c = 0; c < consumers; c++) {
      threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        void* buf[64];
        for (;;) {
          bool got_any = false;
          if (bulk == 1) {
            void* o;
            if (rte_ring_dequeue(r, &o) == 0) {
              got_any = true;
            }
          } else {
            if (rte_ring_dequeue_bulk(r, buf, bulk, nullptr)) {
              got_any = true;
            }
          }
          if (!got_any && producers_done.load(std::memory_order_acquire) == producers &&
              rte_ring_count(r) == 0) {
            break;
          }
        }
      });
    }
    int core = 0;
    for (auto& t : threads) {
      pin_thread(t, core++ % std::max(1u, std::thread::hardware_concurrency()));
    }
    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) {
      t.join();
    }
    auto end = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(end - start).count();
    mops_runs.push_back(total / secs / 1e6);
    rte_ring_free(r);
  }
  const double mops = median(mops_runs);
  return {mops, 1e3 / mops};
}

Result bench_rte_pool(int threads_n) {
  std::vector<double> mops_runs;
  const long total = kOpsPerThread * threads_n;
  for (int rep = 0; rep < kReps; rep++) {
    rte_mempool* pool = rte_mempool_create("bench_rte_pool", 4096, 64, 0, 0, nullptr, nullptr,
                                           nullptr, nullptr, rte_socket_id(), 0);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < threads_n; t++) {
      threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        long done = 0;
        void* slots[16];
        while (done < kOpsPerThread) {
          int held = 0;
          for (int i = 0; i < 16 && rte_mempool_get(pool, &slots[i]) == 0; i++) {
            held++;
          }
          for (int i = 0; i < held; i++) {
            rte_mempool_put(pool, slots[i]);
          }
          done += held;
        }
      });
    }
    int core = 0;
    for (auto& t : threads) {
      pin_thread(t, core++ % std::max(1u, std::thread::hardware_concurrency()));
    }
    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) {
      t.join();
    }
    auto end = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(end - start).count();
    mops_runs.push_back(total / secs / 1e6);
    rte_mempool_free(pool);
  }
  const double mops = median(mops_runs);
  return {mops, 1e3 / mops};
}
#endif  // BENCH_HAVE_DPDK

void print_row(const char* name, Result d
#if BENCH_HAVE_DPDK
               ,
               Result r
#endif
) {
#if BENCH_HAVE_DPDK
  const double ratio = d.mops / r.mops * 100.0;
  printf("%-26s | %9.1f %8.1f | %9.1f %8.1f | %6.0f%%\n", name, d.mops, d.ns_op, r.mops, r.ns_op,
         ratio);
#else
  printf("%-26s | %9.1f %8.1f\n", name, d.mops, d.ns_op);
#endif
}

}  // namespace

int main(int argc, char** argv) {
#if BENCH_HAVE_DPDK
  std::vector<char*> eal = {argv[0], const_cast<char*>("--no-huge"), const_cast<char*>("--no-pci"),
                            const_cast<char*>("-l"), const_cast<char*>("0-3")};
  if (rte_eal_init(static_cast<int>(eal.size()), eal.data()) < 0) {
    fprintf(stderr, "rte_eal_init failed; running daqiri-only\n");
  }
  printf("Comparing daqiri::Ring/ObjectPool vs DPDK rte_ring/rte_mempool\n");
  printf("%-26s | %9s %8s | %9s %8s | %6s\n", "case", "d-Mops", "d-ns", "rte-Mops", "rte-ns",
         "d/rte");
#else
  printf("daqiri::Ring / ObjectPool throughput (build with DPDK for rte comparison)\n");
  printf("%-26s | %9s %8s\n", "case", "Mops", "ns/op");
#endif
  (void)argc;
  printf("------------------------------------------------------------------------------------\n");

  struct RingCase {
    const char* name;
    int p, c;
    unsigned bulk;
    daqiri::RingMode mode;
    unsigned rte_flags;
  };
  const RingCase ring_cases[] = {
      {"SPSC single 1p1c", 1, 1, 1, daqiri::RingMode::SPSC, RING_F_SP_ENQ | RING_F_SC_DEQ},
      {"SPSC bulk8 1p1c", 1, 1, 8, daqiri::RingMode::SPSC, RING_F_SP_ENQ | RING_F_SC_DEQ},
      {"SPSC bulk32 1p1c", 1, 1, 32, daqiri::RingMode::SPSC, RING_F_SP_ENQ | RING_F_SC_DEQ},
      {"MPMC single 2p2c", 2, 2, 1, daqiri::RingMode::MPMC, 0},
      {"MPMC bulk8 2p2c", 2, 2, 8, daqiri::RingMode::MPMC, 0},
      {"MPMC single 4p4c", 4, 4, 1, daqiri::RingMode::MPMC, 0},
      {"MPMC bulk32 4p4c", 4, 4, 32, daqiri::RingMode::MPMC, 0},
  };
  for (const auto& rc : ring_cases) {
    Result d = bench_daqiri_ring(rc.p, rc.c, rc.bulk, rc.mode);
#if BENCH_HAVE_DPDK
    Result r = bench_rte_ring(rc.p, rc.c, rc.bulk, rc.rte_flags);
    print_row(rc.name, d, r);
#else
    (void)rc.rte_flags;
    print_row(rc.name, d);
#endif
  }

  const int pool_threads[] = {1, 4, 8};
  for (int n : pool_threads) {
    char name[32];
    snprintf(name, sizeof(name), "ObjectPool get/put %dt", n);
    Result d = bench_daqiri_pool(n);
#if BENCH_HAVE_DPDK
    Result r = bench_rte_pool(n);
    print_row(name, d, r);
#else
    print_row(name, d);
#endif
  }
  return 0;
}
