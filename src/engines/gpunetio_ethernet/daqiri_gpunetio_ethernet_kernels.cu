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

#include <doca_gpunetio_dev_eth_rxq.cuh>
#include <doca_gpunetio_dev_eth_txq.cuh>

#include <cuda/atomic>

#include "src/engines/gpunetio_ethernet/daqiri_gpunetio_ethernet_kernels.h"

namespace daqiri::gpunetio {

namespace {

constexpr uint32_t RX_THREADS = 256;
// The receive loop returns at least this often, to apply releases and check for exit
constexpr uint64_t RX_POLL_NS = 5000;

constexpr uint32_t TX_THREADS = 256;
// Chunks in flight on a TX queue, each one waits for a completion
constexpr uint32_t TX_CHUNKS_IN_FLIGHT = 64;

// Flags and counters in pinned host memory are shared with the CPU
template <typename T>
__device__ __forceinline__ T load_acquire_sys(T* ptr) {
  cuda::atomic_ref<T, cuda::thread_scope_system> ref(*ptr);
  return ref.load(cuda::std::memory_order_acquire);
}

template <typename T>
__device__ __forceinline__ void store_release_sys(T* ptr, T value) {
  cuda::atomic_ref<T, cuda::thread_scope_system> ref(*ptr);
  ref.store(value, cuda::std::memory_order_release);
}

/*
 * Receive path. The DOCA receive functions give the buffers of the packets they return back to
 * the NIC right away, so the NIC overwrites a packet once it wraps around the ring. The engine
 * keeps the buffers until the application frees the packets: these functions poll the completions
 * through the GPU queue handle, and rx_release() gives the buffers back.
 */

__device__ volatile struct mlx5_cqe64* rx_cqe(const doca_gpu_eth_rxq* rxq, uint64_t idx) {
  auto* cqe = reinterpret_cast<struct mlx5_cqe64*>(rxq->cqe_addr);
  return &cqe[idx & rxq->cqe_mask];
}

// Ring slot of the packet a completion reports
__device__ uint64_t rx_cqe_slot(const doca_gpu_eth_rxq* rxq, volatile struct mlx5_cqe64* cqe64) {
  if (rxq->striding_rq) {
    return static_cast<uint64_t>(DOCA_GPUNETIO_ETH_BSWAP16(cqe64->wqe_id) & rxq->wqe_mask) *
               rxq->wqe_strides_num +
           DOCA_GPUNETIO_ETH_BSWAP16(cqe64->wqe_counter);
  }
  return DOCA_GPUNETIO_ETH_BSWAP16(cqe64->wqe_counter) & rxq->wqe_mask;
}

/*
 * All threads: waits up to timeout_ns for the completions of up to max_pkts packets, and returns
 * how many arrived, without consuming them. Thread i polls the completions i, i + blockDim.x, ...
 * As the NIC writes the completions in order, the count always covers the oldest ones, even when
 * a thread sees a completion that another thread missed. failed reports a completion with error.
 */
__device__ uint32_t rx_poll(doca_gpu_eth_rxq* rxq, uint32_t max_pkts, uint64_t timeout_ns,
                            bool* failed) {
  __shared__ uint32_t total;
  const uint64_t cqe_ci = DOCA_GPUNETIO_ETH_VOLATILE(rxq->cqe_ci);
  const uint32_t cqe_num = rxq->cqe_num;
  const uint32_t per_thread = max(max_pkts / blockDim.x, 1u);
  const uint64_t t0 = threadIdx.x == 0 ? doca_gpu_dev_eth_query_globaltimer() : 0;
  uint32_t got = 0;
  bool error = false;

  if (threadIdx.x == 0) {
    total = 0;
  }
  __syncthreads();

  while (true) {
    bool stop = false;
    const uint64_t idx = cqe_ci + threadIdx.x + static_cast<uint64_t>(got) * blockDim.x;
    volatile struct mlx5_cqe64* cqe64 = rx_cqe(rxq, idx);
    const uint8_t opown = doca_gpu_dev_eth_load_relaxed_sys_global((uint8_t*)&cqe64->op_own);
    const uint8_t opcode = opown >> DOCA_GPUNETIO_ETH_MLX5_CQE_OPCODE_SHIFT;
    if (opcode != MLX5_CQE_INVALID && !((opown & MLX5_CQE_OWNER_MASK) ^ !!(idx & cqe_num))) {
      if (opcode == MLX5_CQE_RESP_ERR || opcode == MLX5_CQE_REQ_ERR) {
        error = true;
        stop = true;
      } else if (++got >= per_thread) {
        stop = true;
      }
    }
    if (threadIdx.x == 0 && doca_gpu_dev_eth_query_globaltimer() - t0 > timeout_ns) {
      stop = true;
    }
    if (__syncthreads_or(stop)) {
      break;
    }
  }

  *failed = __syncthreads_or(error);
  const uint32_t warp_got = __reduce_add_sync(0xffffffff, got);
  if ((threadIdx.x & 31) == 0 && warp_got > 0) {
    atomicAdd_block(&total, warp_got);
  }
  __syncthreads();
  return total;
}

// All threads: the byte counts of the count packets just polled, for the CPU, at their ring slots
__device__ void rx_store_lengths(const RxKernelArgs& args, uint64_t seq, uint32_t count) {
  const uint64_t cqe_ci = DOCA_GPUNETIO_ETH_VOLATILE(args.rxq->cqe_ci);
  const bool striding = args.rxq->striding_rq != 0;
  for (uint32_t i = threadIdx.x; i < count; i += blockDim.x) {
    uint32_t bytes = doca_gpu_dev_eth_bswap32(rx_cqe(args.rxq, cqe_ci + i)->byte_cnt);
    if (striding) {
      bytes &= 0xFFFF;  // The upper bits count the strides
    }
    args.pkt_len[(seq + i) & args.ring_mask] = bytes;
  }
  __threadfence_system();
}

// Thread 0: consumes the completions of count packets. Their receive buffers stay out.
__device__ bool rx_consume(doca_gpu_eth_rxq* rxq, uint32_t count) {
  // Pre-Hopper GPUs need a memory consistency operation before the packets are read
  if (rxq->need_mcst && doca_gpu_dev_eth_rxq_mcst(&rxq->mcst_qp) != DOCA_SUCCESS) {
    return false;
  }
  doca_gpu_dev_eth_rxq_submit_cq_dbr(rxq, DOCA_GPUNETIO_ETH_VOLATILE(rxq->cqe_ci) + count);
  return true;
}

/*
 * Thread 0: gives the receive buffers of the next num_pkts packets, oldest first, back to the NIC.
 * A striding receive WQE holds wqe_strides_num packets and goes back once all of them are
 * released: partial counts the released packets of the oldest WQE still out.
 */
__device__ void rx_release(doca_gpu_eth_rxq* rxq, uint64_t num_pkts, uint64_t* partial) {
  uint64_t wqes = num_pkts;
  if (rxq->striding_rq) {
    const uint64_t strides = rxq->wqe_strides_num;
    const uint64_t released = *partial + num_pkts;
    wqes = released / strides;
    *partial = released - wqes * strides;
  }
  if (wqes > 0) {
    doca_gpu_dev_eth_rxq_submit_dbr(rxq, DOCA_GPUNETIO_ETH_VOLATILE(rxq->wqe_pi) + wqes);
  }
}

/*
 * One block per RX queue of the GPU: block i receives from queues[i] for the lifetime of the
 * engine. A packet stays valid until the application frees its burst: the CPU reports the freed
 * packets through ctrl->released_pkts, and only then does the kernel give their buffers back to
 * the NIC.
 */
__global__ void __launch_bounds__(RX_THREADS) rx_kernel(const RxKernelArgs* queues) {
  const RxKernelArgs args = queues[blockIdx.x];
  __shared__ uint64_t first_slot;
  __shared__ uint64_t received;  // Sequence number of the next packet; ring slot = sequence % size
  __shared__ uint32_t started;   // The first packet set the sequence
  __shared__ uint32_t stop;

  // Used by thread 0 only
  uint64_t released = 0;
  uint64_t partial = 0;
  uint64_t burst_start = 0;  // First packet of the burst being collected
  uint64_t burst_pkts = 0;
  uint64_t burst_t0 = 0;
  uint64_t next_desc = 0;
  uint64_t total_pkts = 0;

  if (threadIdx.x == 0) {
    received = 0;
    started = 0;
    stop = 0;
  }
  __syncthreads();

  while (true) {
    // Thread 0 updates the shared state only once every thread is done with it
    __syncthreads();
    if (threadIdx.x == 0) {
      const uint64_t to_release = load_acquire_sys(&args.ctrl->released_pkts);
      if (to_release != released) {
        rx_release(args.rxq, to_release - released, &partial);
        released = to_release;
      }
      if (load_acquire_sys(&args.ctrl->exit) != 0) {
        stop = 1;
      }
    }
    __syncthreads();
    if (stop != 0) {
      break;
    }

    bool failed = false;
    const uint32_t count = rx_poll(args.rxq, args.max_pkts, RX_POLL_NS, &failed);
    if (failed) {
      if (threadIdx.x == 0) {
        store_release_sys(&args.ctrl->error, static_cast<uint32_t>(KERNEL_RX_ERROR_CQE));
      }
      break;
    }

    if (count > 0) {
      if (threadIdx.x == 0) {
        first_slot =
            rx_cqe_slot(args.rxq, rx_cqe(args.rxq, DOCA_GPUNETIO_ETH_VOLATILE(args.rxq->cqe_ci)));
      }
      __syncthreads();
      // The ring slot of the first packet ever received starts the sequence
      const uint64_t base = started != 0 ? received : first_slot;
      rx_store_lengths(args, base, count);
      __syncthreads();

      if (threadIdx.x == 0) {
        if (first_slot != (base & args.ring_mask)) {
          // Packets must follow the ring order, for example a packet bigger than the stride breaks
          // it
          store_release_sys(&args.ctrl->error, static_cast<uint32_t>(KERNEL_RX_OUT_OF_ORDER));
          stop = 1;
        } else if (!rx_consume(args.rxq, count)) {
          store_release_sys(&args.ctrl->error, static_cast<uint32_t>(KERNEL_RX_ERROR_CQE));
          stop = 1;
        } else {
          if (burst_pkts == 0) {
            burst_start = base;
            burst_t0 = doca_gpu_dev_eth_query_globaltimer();
          }
          burst_pkts += count;
          received = base + count;
          started = 1;
          total_pkts += count;
        }
      }
    }

    if (threadIdx.x == 0 && stop == 0) {
      // Publish full bursts. A burst also ends at the end of the ring, so that its packets are
      // contiguous in memory, and when the timeout elapses.
      const uint64_t now =
          (burst_pkts > 0 && args.timeout_ns > 0) ? doca_gpu_dev_eth_query_globaltimer() : 0;
      const uint64_t published = next_desc;
      while (burst_pkts > 0) {
        const uint64_t room = args.ring_mask + 1 - (burst_start & args.ring_mask);
        uint64_t publish;
        if (burst_pkts >= args.batch_size && args.batch_size <= room) {
          publish = args.batch_size;
        } else if (burst_pkts >= room) {
          publish = room;
        } else if (args.timeout_ns > 0 && now - burst_t0 >= args.timeout_ns) {
          publish = burst_pkts;
        } else {
          break;
        }
        RxBurstDesc* desc = &args.desc[next_desc & args.desc_mask];
        desc->pkt_seq = burst_start;
        desc->num_pkts = static_cast<uint32_t>(publish);
        store_release_sys(&desc->ready, static_cast<uint32_t>(next_desc + 1));
        next_desc++;
        burst_start += publish;
        burst_pkts -= publish;
        burst_t0 = now;
      }
      if (count > 0 || next_desc != published) {
        store_release_sys(&args.ctrl->pkts, total_pkts);
        store_release_sys(&args.ctrl->bursts, next_desc);
      }
    }
  }
}

/*
 * The TX kernels post each burst as chunks of WQEs. Only the last WQE of a chunk asks the NIC for
 * a completion, and a chunk is never bigger than half the send queue, so the queue always takes a
 * new chunk once the oldest chunks complete.
 */
struct TxRun {
  uint64_t next_wqe;
  uint64_t done_wqe;
  uint64_t chunks;
  uint64_t cqes;
  uint64_t bursts_done;
  uint64_t chunk_end[TX_CHUNKS_IN_FLIGHT];  // WQE index after the chunk
  uint8_t chunk_last[TX_CHUNKS_IN_FLIGHT];  // The chunk ends a burst
};

// Thread 0: consumes the completion of the oldest chunk in flight. Without wait, returns with
// progress == false if the completion has not arrived yet. Returns false on a completion error.
__device__ bool tx_complete_one(const TxKernelArgs& args, TxRun& run, bool wait, bool* progress) {
  *progress = false;
  if (run.cqes == run.chunks) {
    return true;
  }
  if (doca_gpu_dev_eth_txq_poll_completion_at(
          args.txq, run.cqes,
          wait ? DOCA_GPUNETIO_ETH_WAIT_FLAG_B : DOCA_GPUNETIO_ETH_WAIT_FLAG_NB) != DOCA_SUCCESS) {
    return false;
  }
  if (DOCA_GPUNETIO_ETH_VOLATILE(args.txq->cqe_ci) <= run.cqes) {
    return true;
  }
  const uint32_t slot = run.cqes % TX_CHUNKS_IN_FLIGHT;
  run.done_wqe = run.chunk_end[slot];
  if (run.chunk_last[slot] != 0) {
    run.bursts_done++;
    store_release_sys(&args.ctrl->completed_bursts, run.bursts_done);
  }
  run.cqes++;
  *progress = true;
  return true;
}

// All threads: one WQE per packet, then thread 0 rings the doorbell
__device__ void tx_post_chunk(const TxKernelArgs& args, uint64_t wqe_first, uint64_t slot_first,
                              uint32_t count) {
  for (uint32_t i = threadIdx.x; i < count; i += blockDim.x) {
    const TxPacket pkt = args.pkts[(slot_first + i) % args.num_slots];
    const uint64_t wqe_idx = wqe_first + i;
    struct doca_gpu_dev_eth_txq_wqe* wqe =
        doca_gpu_dev_eth_txq_get_wqe_ptr(args.txq, static_cast<uint16_t>(wqe_idx));
    doca_gpu_dev_eth_txq_wqe_prepare_send(
        args.txq, wqe, static_cast<uint16_t>(wqe_idx), pkt.addr, args.mkey, pkt.len,
        i == count - 1 ? DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY : DOCA_GPUNETIO_ETH_SEND_FLAG_NONE);
  }
  __syncthreads();
  if (threadIdx.x == 0) {
    doca_gpu_dev_eth_txq_submit(args.txq, wqe_first + count);
  }
}

// All threads: posts a burst, waiting for send queue room when needed. Returns false on a
// completion error.
__device__ bool tx_post_burst(const TxKernelArgs& args, TxRun& run, uint64_t slot_start,
                              uint32_t num_pkts) {
  __shared__ uint32_t ok;

  for (uint32_t done = 0; done < num_pkts;) {
    const uint32_t count = min(num_pkts - done, args.max_chunk);
    if (threadIdx.x == 0) {
      const uint64_t sq_size = static_cast<uint64_t>(args.txq->wqe_mask) + 1;
      bool progress = false;
      bool room_ok = true;
      while (room_ok && (run.next_wqe + count - run.done_wqe > sq_size ||
                         run.chunks - run.cqes >= TX_CHUNKS_IN_FLIGHT)) {
        room_ok = tx_complete_one(args, run, true, &progress);
      }
      ok = room_ok ? 1 : 0;
    }
    __syncthreads();
    if (ok == 0) {
      return false;
    }

    tx_post_chunk(args, run.next_wqe, slot_start + done, count);
    if (threadIdx.x == 0) {
      const uint32_t slot = run.chunks % TX_CHUNKS_IN_FLIGHT;
      run.chunk_end[slot] = run.next_wqe + count;
      run.chunk_last[slot] = (done + count == num_pkts) ? 1 : 0;
      run.next_wqe += count;
      run.chunks++;
    }
    __syncthreads();
    done += count;
  }
  return true;
}

__device__ void tx_load_state(const TxKernelArgs& args, TxRun& run) {
  run.next_wqe = args.state->next_wqe;
  run.done_wqe = args.state->done_wqe;
  run.chunks = args.state->chunks;
  run.cqes = args.state->chunks;  // Every launch leaves no completion pending
  run.bursts_done = args.state->bursts_done;
}

__device__ void tx_save_state(const TxKernelArgs& args, const TxRun& run) {
  args.state->next_wqe = run.next_wqe;
  args.state->done_wqe = run.done_wqe;
  args.state->chunks = run.chunks;
  args.state->bursts_done = run.bursts_done;
}

// One block per persistent TX queue of the GPU: block i serves queues[i] for the lifetime of the
// engine, several bursts in flight
__global__ void __launch_bounds__(TX_THREADS) tx_persistent_kernel(const TxKernelArgs* queues) {
  const TxKernelArgs args = queues[blockIdx.x];
  __shared__ TxRun run;
  __shared__ uint64_t slot_start;
  __shared__ uint32_t num_pkts;
  __shared__ uint32_t stop;

  uint64_t next_desc = 0;  // Thread 0 only

  if (threadIdx.x == 0) {
    tx_load_state(args, run);
    stop = 0;
  }
  __syncthreads();

  while (true) {
    // Thread 0 updates the shared state only once every thread is done with it
    __syncthreads();
    if (threadIdx.x == 0) {
      bool progress = true;
      bool ok = true;
      while (ok && progress) {
        ok = tx_complete_one(args, run, false, &progress);
      }
      num_pkts = 0;
      if (!ok) {
        store_release_sys(&args.ctrl->error, static_cast<uint32_t>(KERNEL_TX_ERROR_CQE));
        stop = 1;
      } else if (load_acquire_sys(&args.ctrl->exit) != 0) {
        stop = 1;
      } else {
        TxBurstDesc* desc = &args.desc[next_desc & args.desc_mask];
        if (load_acquire_sys(&desc->ready) == static_cast<uint32_t>(next_desc + 1)) {
          slot_start = desc->slot_start;
          num_pkts = desc->num_pkts;
          next_desc++;
        }
      }
    }
    __syncthreads();
    if (stop != 0) {
      break;
    }
    if (num_pkts > 0 && !tx_post_burst(args, run, slot_start, num_pkts)) {
      if (threadIdx.x == 0) {
        store_release_sys(&args.ctrl->error, static_cast<uint32_t>(KERNEL_TX_ERROR_CQE));
      }
      break;
    }
  }
}

// One launch per burst: posts it and returns once the NIC has sent it
__global__ void __launch_bounds__(TX_THREADS)
    tx_burst_kernel(TxKernelArgs args, uint64_t slot_start, uint32_t num_pkts) {
  __shared__ TxRun run;

  if (threadIdx.x == 0) {
    tx_load_state(args, run);
  }
  __syncthreads();

  bool ok = tx_post_burst(args, run, slot_start, num_pkts);
  if (threadIdx.x == 0) {
    bool progress = false;
    while (ok && run.cqes < run.chunks) {
      ok = tx_complete_one(args, run, true, &progress);
    }
    if (!ok) {
      store_release_sys(&args.ctrl->error, static_cast<uint32_t>(KERNEL_TX_ERROR_CQE));
    }
    tx_save_state(args, run);
  }
}

// The blocks of a resident kernel never return, so the GPU must run all of them at once: a block
// waiting for room would leave its queue unserved
template <typename Kernel>
cudaError_t check_resident(Kernel kernel, uint32_t threads, uint32_t blocks) {
  int device = 0;
  int sms = 0;
  int per_sm = 0;
  cudaError_t err = cudaGetDevice(&device);
  if (err == cudaSuccess) {
    err = cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, device);
  }
  if (err == cudaSuccess) {
    err = cudaOccupancyMaxActiveBlocksPerMultiprocessor(&per_sm, kernel, threads, 0);
  }
  if (err != cudaSuccess) {
    return err;
  }
  return static_cast<uint64_t>(per_sm) * sms >= blocks ? cudaSuccess
                                                       : cudaErrorLaunchOutOfResources;
}

// Blocks of the other kernels that share the GPU take part of every SM: a kernel that fits one SM
// k times uses 1/k of it per block, so the blocks of all the kernels fit if these fractions add up
// to at most the SM count
template <typename Kernel>
cudaError_t add_sm_share(Kernel kernel, uint32_t threads, uint32_t blocks, double* sms_needed) {
  if (blocks == 0) {
    return cudaSuccess;
  }
  int per_sm = 0;
  const cudaError_t err =
      cudaOccupancyMaxActiveBlocksPerMultiprocessor(&per_sm, kernel, threads, 0);
  if (err != cudaSuccess) {
    return err;
  }
  if (per_sm <= 0) {
    return cudaErrorLaunchOutOfResources;
  }
  *sms_needed += static_cast<double>(blocks) / per_sm;
  return cudaSuccess;
}

}  // namespace

uint32_t rx_kernel_threads() {
  return RX_THREADS;
}

cudaError_t load_kernels() {
  cudaFuncAttributes attr;
  cudaError_t err = cudaFuncGetAttributes(&attr, rx_kernel);
  if (err == cudaSuccess) {
    err = cudaFuncGetAttributes(&attr, tx_persistent_kernel);
  }
  if (err == cudaSuccess) {
    err = cudaFuncGetAttributes(&attr, tx_burst_kernel);
  }
  return err;
}

cudaError_t check_gpu_capacity(uint32_t rx_blocks, uint32_t tx_persistent_blocks,
                               uint32_t tx_burst_blocks) {
  int device = 0;
  int sms = 0;
  cudaError_t err = cudaGetDevice(&device);
  if (err == cudaSuccess) {
    err = cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, device);
  }
  double needed = 0;
  if (err == cudaSuccess) {
    err = add_sm_share(rx_kernel, RX_THREADS, rx_blocks, &needed);
  }
  if (err == cudaSuccess) {
    err = add_sm_share(tx_persistent_kernel, TX_THREADS, tx_persistent_blocks, &needed);
  }
  if (err == cudaSuccess) {
    err = add_sm_share(tx_burst_kernel, TX_THREADS, tx_burst_blocks, &needed);
  }
  if (err != cudaSuccess) {
    return err;
  }
  return needed <= static_cast<double>(sms) ? cudaSuccess : cudaErrorLaunchOutOfResources;
}

cudaError_t launch_rx_kernel(const RxKernelArgs* queues, uint32_t num_queues, cudaStream_t stream) {
  const cudaError_t err = check_resident(rx_kernel, RX_THREADS, num_queues);
  if (err != cudaSuccess) {
    return err;
  }
  rx_kernel<<<num_queues, RX_THREADS, 0, stream>>>(queues);
  return cudaGetLastError();
}

cudaError_t launch_tx_persistent_kernel(const TxKernelArgs* queues, uint32_t num_queues,
                                        cudaStream_t stream) {
  const cudaError_t err = check_resident(tx_persistent_kernel, TX_THREADS, num_queues);
  if (err != cudaSuccess) {
    return err;
  }
  tx_persistent_kernel<<<num_queues, TX_THREADS, 0, stream>>>(queues);
  return cudaGetLastError();
}

cudaError_t launch_tx_burst_kernel(const TxKernelArgs& args, uint64_t slot_start, uint32_t num_pkts,
                                   cudaStream_t stream) {
  tx_burst_kernel<<<1, TX_THREADS, 0, stream>>>(args, slot_start, num_pkts);
  return cudaGetLastError();
}

}  // namespace daqiri::gpunetio
