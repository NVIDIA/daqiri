/* SPDX-License-Identifier: Apache-2.0 */
#include "tx_deadline_gpu.h"

#ifdef DAQIRI_BENCH_GPUNETIO
#include <doca_gpunetio_dev_eth_txq.cuh>
#endif

namespace tx_deadline {
namespace {
__device__ uint64_t timer_ns() {
  uint64_t value;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(value));
  return value;
}

__device__ uint32_t load_acquire(uint32_t* ptr) {
  uint32_t value;
  asm volatile("ld.acquire.sys.global.u32 %0, [%1];" : "=r"(value) : "l"(ptr) : "memory");
  return value;
}

__device__ void store_release(uint32_t* ptr, uint32_t value) {
  asm volatile("st.release.sys.global.u32 [%0], %1;" ::"l"(ptr), "r"(value) : "memory");
}

__device__ daqiri::GpuTxTicket load_ticket(const daqiri::GpuTxTicket* p) {
  uint64_t value, indices;
  // Strong loads bypass stale per-SM data after the next window's publication.
  // This allocation is GPU memory, so no per-packet host descriptor fetch occurs.
  asm volatile("ld.relaxed.sys.global.v2.u64 {%0, %1}, [%2];"
               : "=l"(value), "=l"(indices)
               : "l"(p)
               : "memory");
  return {value, uint32_t(indices), uint32_t(indices >> 32)};
}

__device__ void put16(uint8_t* p, uint16_t value) {
  p[0] = value >> 8;
  p[1] = value;
}

__device__ void put64(uint8_t* p, uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    p[i] = value;
    value >>= 8;
  }
}

#ifdef DAQIRI_BENCH_GPUNETIO
// Descriptor construction only: packet bytes are produced later, after command.
// Each lane builds one WAIT/SEND pair using the unmodified SDK builders.
__global__ void build_sdk_window(doca_gpu_eth_txq* q, uint8_t* packets, uint32_t slots,
                                 uint32_t stride, uint32_t first_slot, uint32_t n, uint32_t bytes,
                                 uint32_t mkey, uint64_t epoch, uint64_t interval, uint64_t lead,
                                 daqiri::GpuTxTicket* tickets) {
  __shared__ uint64_t first;
  constexpr auto sharing = DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_EXCLUSIVE;
  if (threadIdx.x == 0) first = doca_gpu_dev_eth_txq_reserve_wq_slots<sharing>(q, 2 * n);
  __syncthreads();
  const auto j = threadIdx.x;
  if (j < n) {
    const uint64_t base = first + 2 * j;
    uint64_t timestamp;
    doca_gpu_dev_eth_txq_calculate_timestamp(q, epoch + uint64_t(j) * interval + lead, &timestamp);
    doca_gpu_dev_eth_txq_wqe_prepare_wait_time(q, doca_gpu_dev_eth_txq_get_wqe_ptr(q, base), base,
                                               timestamp, DOCA_GPUNETIO_ETH_SEND_FLAG_NONE);
    // DOCA 3.2's WAIT encoder leaves cseg.imm and wseg.{lkey,va_high,va_low}
    // uninitialized in its local structs. The standalone 32-lane caller exposes
    // nonzero register garbage in those reserved fields (validated by readback).
    // Normalize only these reserved words after the SDK encoder. This happens
    // entirely outside the measured deadline and does not change SDK submit.
    auto* wait = doca_gpu_dev_eth_txq_get_wqe_ptr(q, base);
    asm volatile("" ::: "memory");
    wait->cseg.imm = 0;
    wait->wseg.lkey = 0;
    wait->wseg.va_high = 0;
    wait->wseg.va_low = 0;
    const auto addr =
        reinterpret_cast<uint64_t>(packets + size_t((first_slot + j) % slots) * stride);
    doca_gpu_dev_eth_txq_wqe_prepare_send(q, doca_gpu_dev_eth_txq_get_wqe_ptr(q, base + 1),
                                          base + 1, addr, mkey, bytes,
                                          DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY);
    // The SDK prebuild also caches its own full producer index on the GPU.
    tickets[j] = {0, uint32_t(base + 2), 0};
  }
  // Every writer releases its WQEs; no doorbell or packet publication here.
  __threadfence_system();
  __syncthreads();
  if (threadIdx.x == 0) doca_gpu_dev_eth_txq_mark_wqes_ready<sharing>(q, first, first + 2 * n - 1);
}
#endif

// Every byte, including Ethernet/IP/UDP headers and the per-packet marker, is
// produced on the GPU. No host packet template or host header fill is used.
__device__ void construct_packet(uint8_t* p, PacketSpec s, uint64_t seq, uint64_t deadline) {
  for (uint32_t i = threadIdx.x; i < s.bytes; i += blockDim.x) p[i] = uint8_t(seq + i);
  __syncthreads();
  if (threadIdx.x == 0) {
    for (int i = 0; i < 6; ++i) {
      p[i] = s.dst_mac[i];
      p[i + 6] = s.src_mac[i];
    }
    put16(p + 12, 0x0800);
    p[14] = 0x45;
    p[15] = 0;
    put16(p + 16, s.bytes - 14);
    put16(p + 18, uint16_t(seq));
    put16(p + 20, 0x4000);
    p[22] = 64;
    p[23] = 17;
    put16(p + 24, 0);
    for (int i = 0; i < 4; ++i) {
      p[26 + i] = s.src_ip[i];
      p[30 + i] = s.dst_ip[i];
    }
    uint32_t sum = 0;
    for (int i = 14; i < 34; i += 2) sum += (uint32_t(p[i]) << 8) | p[i + 1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    put16(p + 24, ~sum);
    put16(p + 34, s.src_port);
    put16(p + 36, s.dst_port);
    put16(p + 38, s.bytes - 34);
    put16(p + 40, 0);  // IPv4 UDP checksum disabled, identically on both paths.
    put16(p + 42, kMagic >> 16);
    put16(p + 44, kMagic & 0xffff);
    put64(p + 46, seq);
    put64(p + 54, deadline);
    put16(p + 62, 0x1234);
    put16(p + 64, 0x5678);
  }
  __syncthreads();
  // Flush all producing threads' stores before signaling the CPU or NIC.
  __threadfence_system();
  __syncthreads();
}

__global__ void produce(Control* c, PacketSpec spec, bool persistent, void* queue, uint32_t mkey_be,
                        Backend backend, daqiri::GpuTxQueue cached_queue,
                        const daqiri::GpuTxTicket* tickets) {
  __shared__ uint32_t token;
  __shared__ uint8_t* packet;
  __shared__ uint64_t sequence, deadline;
  daqiri::GpuTxTicket ticket;
  uint32_t last = 0;
  uint64_t start_time = 0, ready_time = 0;
  do {
    if (threadIdx.x == 0) {
      do {
        token = load_acquire(&c->request);
      } while (token == last);
      if (token != UINT32_MAX) {
        start_time = timer_ns();
        packet = c->packet;
        sequence = c->sequence;
        deadline = c->deadline_ns;
        if (tickets) ticket = load_ticket(tickets + c->ticket_index);
      }
    }
    __syncthreads();
    if (token == UINT32_MAX) return;
    construct_packet(packet, spec, sequence, deadline);
    if (threadIdx.x == 0) {
      ready_time = timer_ns();
      if (backend == Backend::Cpu) store_release(&c->ready, token);
      if (backend == Backend::Gdr) {
        auto* q = static_cast<GdrQueueView*>(queue);
        uint64_t first;
        daqiri::submit_tx_gpu(cached_queue, ticket, &first);
        const auto end = timer_ns();
        c->first_doorbell_ns = first;
        c->gpu_submit_ns = end;
        const uint64_t ci = q->ci;
        auto* entry = reinterpret_cast<volatile uint8_t*>(q->cq + (ci & (q->cq_size - 1)) * 64);
        const uint64_t until = timer_ns() + 1000000000ULL;
        uint16_t opown;
        do {
          asm volatile("ld.relaxed.sys.global.u8 %0, [%1];"
                       : "=h"(opown)
                       : "l"(entry + 63)
                       : "memory");
          if (timer_ns() > until) {
            c->error = 3;
            break;
          }
        } while ((opown >> 4) == 15 || (opown & 1) != !!(ci & q->cq_size));
        if (!c->error) {
          if ((opown >> 4) != 0) {
            c->error = 0x100 + opown;
          } else {
            __threadfence_system();
            uint64_t raw;
            asm volatile("ld.relaxed.sys.global.u64 %0, [%1];"
                         : "=l"(raw)
                         : "l"(entry + 48)
                         : "memory");
            c->tx_raw_timestamp = __byte_perm(uint32_t(raw >> 32), 0, 0x0123) |
                                  (uint64_t(__byte_perm(uint32_t(raw), 0, 0x0123)) << 32);
            q->ci = ci + 1;
            daqiri::detail::gpu_tx_release();
            const uint32_t record = __byte_perm(uint32_t(ci + 1) & 0xffffff, 0, 0x0123);
            asm volatile("st.relaxed.sys.global.u32 [%0], %1;" ::"l"(q->cq_record), "r"(record)
                         : "memory");
          }
        }
      }
#ifdef DAQIRI_BENCH_GPUNETIO
      if (backend == Backend::Gpunetio || backend == Backend::GpunetioPrebuilt) {
        auto* q = static_cast<doca_gpu_eth_txq*>(queue);
        if (backend == Backend::GpunetioPrebuilt) {
          // Unmodified SDK submit, using WQEs prepared before the command window.
          doca_gpu_dev_eth_txq_submit<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_EXCLUSIVE,
                                      DOCA_GPUNETIO_ETH_SYNC_SCOPE_SYS,
                                      DOCA_GPUNETIO_ETH_NIC_HANDLER_GPU_SM_DB>(
              q, uint64_t(ticket.record_value));
        } else {
          uint64_t timestamp;
          doca_gpu_dev_eth_txq_calculate_timestamp(q, deadline, &timestamp);
          doca_gpu_dev_eth_ticket_t out_ticket;
          doca_gpu_dev_eth_txq_wait_send_thread<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_EXCLUSIVE,
                                                DOCA_GPUNETIO_ETH_SYNC_SCOPE_SYS,
                                                DOCA_GPUNETIO_ETH_NIC_HANDLER_GPU_SM_DB>(
              q, timestamp, reinterpret_cast<uint64_t>(packet), mkey_be, spec.bytes,
              DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY, &out_ticket);
        }
        c->gpu_submit_ns = timer_ns();
        // One SEND CQE per packet; WAIT has no notification. Read its timestamp
        // after owner/phase validation, before acknowledging/reusing the CQ slot.
        // DOCA 3.2 exposes no timestamp getter, so use its published mlx5 CQ view.
        const uint64_t ci = q->cqe_ci;
        auto* cq = reinterpret_cast<mlx5_cqe64*>(q->cqe_addr);
        volatile mlx5_cqe64* entry = &cq[ci & q->cqe_mask];
        const uint64_t until = timer_ns() + 1000000000ULL;
        uint8_t opown;
        do {
          opown = doca_gpu_dev_eth_load_relaxed_sys_global((uint8_t*)&entry->op_own);
          if (timer_ns() > until) {
            c->error = 1;
            break;
          }
        } while ((opown >> 4) == MLX5_CQE_INVALID || (opown & 1) != !!(ci & q->cqe_num));
        if (!c->error) {
          if ((opown >> 4) != MLX5_CQE_REQ) {
            doca_gpu_dev_eth_print_cqe_err(entry);
            c->error = 2;
          } else {
            __threadfence_system();
            const uint64_t raw = entry->timestamp;
            c->tx_raw_timestamp = __byte_perm(uint32_t(raw >> 32), 0, 0x0123) |
                                  (uint64_t(__byte_perm(uint32_t(raw), 0, 0x0123)) << 32);
            doca_gpu_dev_eth_txq_submit_cq_dbr<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_EXCLUSIVE,
                                               DOCA_GPUNETIO_ETH_SYNC_SCOPE_SYS>(q, ci + 1);
          }
        }
      }
#endif
      // Keep mapped-host diagnostic stores off every transmitter's submit path.
      c->gpu_start_ns = start_time;
      c->gpu_ready_ns = ready_time;
      store_release(&c->done, token);
    }
    __syncthreads();
    if (token == UINT32_MAX) return;
    last = token;
  } while (persistent);
}
}  // namespace

cudaError_t prebuild_sdk_window(void* queue, uint8_t* packets, uint32_t slots, uint32_t stride,
                                uint32_t first_slot, uint32_t n, uint32_t bytes, uint32_t mkey_be,
                                uint64_t epoch, uint64_t interval, uint64_t lead,
                                daqiri::GpuTxTicket* tickets) {
#ifdef DAQIRI_BENCH_GPUNETIO
  if (!n || n > 32) return cudaErrorInvalidValue;
  build_sdk_window<<<1, 32>>>(static_cast<doca_gpu_eth_txq*>(queue), packets, slots, stride,
                              first_slot, n, bytes, mkey_be, epoch, interval, lead, tickets);
  return cudaGetLastError();
#else
  return cudaErrorNotSupported;
#endif
}

cudaError_t launch(Control* c, PacketSpec spec, bool persistent, void* q, uint32_t mkey,
                   cudaStream_t stream, Backend backend, daqiri::GpuTxQueue cached_queue,
                   const daqiri::GpuTxTicket* tickets) {
  produce<<<1, 128, 0, stream>>>(c, spec, persistent, q, mkey, backend, cached_queue, tickets);
  return cudaGetLastError();
}
}  // namespace tx_deadline
