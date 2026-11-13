// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

namespace daqiri {

// Cache this process-local handle on the GPU once for a queue's lifetime.
struct GpuTxQueue {
  uint64_t doorbell;
  uint64_t record;
};

// Copy a bounded window of these tickets into GPU memory before producing its
// packets. Tickets must be submitted once, in preparation order.
struct GpuTxTicket {
  uint64_t doorbell_value;
  uint32_t record_value;
  uint32_t doorbell_offset;
};

// Opaque, single-use descriptor returned by prepare_tx_burst_gpu(). Copy it to
// the producing GPU without changing its fields. It owns no packet storage.
struct GpuTxSubmission {
  uint64_t doorbell;
  uint64_t record;
  uint64_t doorbell_value;
  uint32_t record_value;
  uint32_t doorbell_offset;
};

#ifdef __CUDACC__
#define DAQIRI_GPU_TX_HD __host__ __device__
#else
#define DAQIRI_GPU_TX_HD
#endif

DAQIRI_GPU_TX_HD inline GpuTxQueue gpu_tx_queue(const GpuTxSubmission& submission) {
  return {submission.doorbell - submission.doorbell_offset, submission.record};
}

DAQIRI_GPU_TX_HD inline GpuTxTicket gpu_tx_ticket(const GpuTxSubmission& submission) {
  return {submission.doorbell_value, submission.record_value, submission.doorbell_offset};
}

#undef DAQIRI_GPU_TX_HD

#ifdef __CUDACC__
namespace detail {
__device__ __forceinline__ void gpu_tx_release() {
#if __CUDA_ARCH__ >= 900 && \
    (__CUDACC_VER_MAJOR__ > 12 || (__CUDACC_VER_MAJOR__ == 12 && __CUDACC_VER_MINOR__ >= 8))
  asm volatile("fence.release.sys;" ::: "memory");
#else
  // GPUNetIO's pre-SM90 release-fence emulation. The addressed scratch store
  // supplies release ordering without a full system acquire/release fence.
  uint32_t scratch;
  asm volatile("st.release.sys.u32 [%0], %1;" ::"l"(&scratch), "r"(0) : "memory");
#endif
}
}  // namespace detail

/**
 * Submit exactly once, from one GPU thread, after all packet-writing threads
 * have performed a system fence and synchronized. The CPU must publish the
 * descriptor with a system release/acquire handshake (or a CUDA dependency).
 * SM 70+ and CUDA 12.2+ are required for the non-coalescing MMIO store.
 * Optionally return the first doorbell's GPU globaltimer value. This is not an
 * epoch timestamp. The timestamp is stored only after the recovery doorbell.
 */
__device__ __forceinline__ void submit_tx_gpu(const GpuTxQueue& queue, const GpuTxTicket& ticket,
                                              uint64_t* first_doorbell_ns = nullptr) {
  const uint64_t doorbell = queue.doorbell + ticket.doorbell_offset;
  // Match mlx5's GPUNetIO submit protocol: notify early after publishing the
  // packet/WQEs, then update DBREC and ring again for the recovery path. The
  // first notification need not wait for the GPU-to-host DBREC store.
  detail::gpu_tx_release();
  asm volatile("st.mmio.relaxed.sys.global.b64 [%0], %1;" ::"l"(doorbell),
               "l"(ticket.doorbell_value)
               : "memory");
  uint64_t first = 0;
  if (first_doorbell_ns) asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(first));
  asm volatile("st.relaxed.sys.global.u32 [%0], %1;" ::"l"(queue.record), "r"(ticket.record_value)
               : "memory");
  detail::gpu_tx_release();
  asm volatile("st.mmio.relaxed.sys.global.b64 [%0], %1;" ::"l"(doorbell),
               "l"(ticket.doorbell_value)
               : "memory");
  if (first_doorbell_ns) *first_doorbell_ns = first;
}

__device__ __forceinline__ void submit_tx_gpu(const GpuTxSubmission& submission,
                                              uint64_t* first_doorbell_ns = nullptr) {
  submit_tx_gpu(gpu_tx_queue(submission), gpu_tx_ticket(submission), first_doorbell_ns);
}
#endif

}  // namespace daqiri
