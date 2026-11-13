/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "tx_deadline_gpu.h"
#include <memory>

namespace tx_deadline {
// Benchmark prototype: CPU encodes WQEs, GDRCopy prepublishes to a GPU SQ,
// and the persistent GPU packet producer rings the doorbell. No CUDA-copy path.
class GdrTx {
 public:
  GdrTx(const char* bdf, uint32_t slots, uint32_t stride);
  ~GdrTx();
  GdrTx(const GdrTx&) = delete;
  GdrTx& operator=(const GdrTx&) = delete;
  void* queue() const;
  daqiri::GpuTxQueue handle() const;
  std::vector<PreparedTx> prepare_window(uint32_t n, uint32_t bytes, uint64_t epoch,
                                         uint64_t interval, uint64_t lead);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace tx_deadline
