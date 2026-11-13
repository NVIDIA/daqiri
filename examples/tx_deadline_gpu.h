/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cuda_runtime_api.h>
#include <daqiri/gpu_tx.cuh>
#include <cstdint>
#include <vector>

namespace tx_deadline {
enum class Notify { MappedFlag, EventPoll, StreamPoll, EventSync, HostCallback };
enum class Backend { Cpu, Gdr, Gpunetio, GpunetioPrebuilt };

// Single writer/reader, one command outstanding. No mapped-host atomics/RMWs.
struct alignas(64) Control {
  alignas(64) uint32_t request = 0;
  uint32_t ticket_index = 0;
  alignas(64) uint32_t ready = 0;
  alignas(64) uint32_t done = 0;
  uint32_t error = 0;
  uint64_t gpu_start_ns = 0, gpu_ready_ns = 0, gpu_submit_ns = 0;
  uint64_t tx_raw_timestamp = 0, deadline_ns = 0, sequence = 0;
  uint8_t* packet = nullptr;
  uint64_t first_doorbell_ns = 0;
};

struct PacketSpec {
  uint8_t src_mac[6]{}, dst_mac[6]{};
  uint8_t src_ip[4]{192, 0, 2, 1}, dst_ip[4]{192, 0, 2, 2};
  uint16_t src_port = 4096, dst_port = 4096;
  uint32_t bytes = 256;
};
constexpr uint32_t kMagic = 0x44515458;  // DQTX
constexpr uint32_t kMarkerOffset = 42;
constexpr uint32_t kMinBytes = 66;

struct PreparedTx {
  uint8_t* packet = nullptr;
  daqiri::GpuTxSubmission submission{};
};
// GPU-readable CQ for the benchmark's native GPU SQ. DBREC and CQ are host mapped.
struct GdrQueueView {
  uint64_t cq, cq_record, ci;
  uint32_t cq_size;
};

cudaError_t launch(Control*, PacketSpec, bool persistent, void* queue, uint32_t mkey_be,
                   cudaStream_t, Backend, daqiri::GpuTxQueue = {},
                   const daqiri::GpuTxTicket* = nullptr);
cudaError_t prebuild_sdk_window(void* queue, uint8_t* packets, uint32_t slots, uint32_t stride,
                                uint32_t first_slot, uint32_t n, uint32_t bytes, uint32_t mkey_be,
                                uint64_t epoch, uint64_t interval, uint64_t lead,
                                daqiri::GpuTxTicket* tickets);

class Gpunetio {
 public:
  Gpunetio(const char* nic_bdf, int gpu, uint32_t slots, uint32_t stride);
  ~Gpunetio();
  Gpunetio(const Gpunetio&) = delete;
  Gpunetio& operator=(const Gpunetio&) = delete;
  void* queue() const;
  uint8_t* packet() const;
  uint32_t mkey_be() const;
  std::vector<PreparedTx> prepare_gpu_window(uint32_t n, uint32_t bytes, uint64_t epoch,
                                             uint64_t interval, uint64_t lead,
                                             daqiri::GpuTxTicket* tickets);

 private:
  struct Impl;
  Impl* impl_;
};
}  // namespace tx_deadline
