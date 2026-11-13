/* SPDX-License-Identifier: Apache-2.0 */
#include "tx_deadline_gpu.h"

#include <arpa/inet.h>
#include <unistd.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_eth_txq.h>
#include <doca_gpunetio.h>
#include <doca_mmap.h>
#include <doca_gpunetio_eth_def.h>
#include <cstring>
#include <vector>
#include <array>
#include <cstdlib>

#include <memory>
#include <stdexcept>
#include <string>

namespace tx_deadline {
namespace {
void check(doca_error_t error, const char* operation) {
  if (error != DOCA_SUCCESS)
    throw std::runtime_error(std::string(operation) + ": " + doca_error_get_descr(error));
}
}  // namespace

struct Gpunetio::Impl {
  doca_dev* dev = nullptr;
  doca_gpu* gpu = nullptr;
  doca_eth_txq* txq = nullptr;
  doca_gpu_eth_txq* gpu_txq = nullptr;
  doca_mmap* mmap = nullptr;
  void* packet = nullptr;
  uint32_t key = 0;
  uint32_t slots = 0, stride = 0, next_slot = 0;
  int dmabuf = -1;
  bool started = false;
  doca_gpu_eth_txq cached{};
  bool cached_valid = false;
  uint64_t cpu_pi = 0;

  ~Impl() {
    if (started) doca_ctx_stop(doca_eth_txq_as_doca_ctx(txq));
    if (txq) doca_eth_txq_destroy(txq);
    if (mmap) doca_mmap_destroy(mmap);
    if (dmabuf >= 0) close(dmabuf);
    if (packet) doca_gpu_mem_free(gpu, packet);
    if (gpu) doca_gpu_destroy(gpu);
    if (dev) doca_dev_close(dev);
  }
};

Gpunetio::Gpunetio(const char* bdf, int ordinal, uint32_t slots, uint32_t stride) : impl_(nullptr) {
  auto p = std::make_unique<Impl>();
  p->slots = slots;
  p->stride = stride;
  char gpu_bdf[32]{};
  if (cudaDeviceGetPCIBusId(gpu_bdf, sizeof(gpu_bdf), ordinal) != cudaSuccess)
    throw std::runtime_error("cannot resolve GPU PCI address");
  doca_devinfo** list = nullptr;
  uint32_t count = 0;
  check(doca_devinfo_create_list(&list, &count), "device list");
  doca_error_t open_result = DOCA_ERROR_NOT_FOUND;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t equal = 0;
    if (doca_devinfo_is_equal_pci_addr(list[i], bdf, &equal) != DOCA_SUCCESS || !equal) continue;
    open_result = doca_dev_open(list[i], &p->dev);
    break;
  }
  doca_devinfo_destroy_list(list);
  check(open_result, "open DOCA NIC");
  check(doca_eth_txq_cap_is_type_supported(doca_dev_as_devinfo(p->dev), DOCA_ETH_TXQ_TYPE_REGULAR,
                                           DOCA_ETH_TXQ_DATA_PATH_TYPE_GPU),
        "GPU TX capability");
  doca_eth_wait_on_time_type mode;
  check(doca_eth_txq_cap_get_wait_on_time_offload_supported(doca_dev_as_devinfo(p->dev), &mode),
        "wait-on-time capability");
  if (mode != DOCA_ETH_WAIT_ON_TIME_TYPE_NATIVE)
    throw std::runtime_error("GPUNetIO comparison requires native hardware wait-on-time");
  check(doca_gpu_create(gpu_bdf, &p->gpu), "GPU context");
  check(doca_eth_txq_create(p->dev, 1024, &p->txq), "GPU TX queue");
  check(doca_eth_txq_set_wait_on_time_offload(p->txq), "accurate send");
  {
    check(doca_eth_txq_set_l3_chksum_offload(p->txq, 1), "matched L3 offload");
    check(doca_eth_txq_set_l4_chksum_offload(p->txq, 1), "matched L4 offload");
  }
  check(doca_ctx_set_datapath_on_gpu(doca_eth_txq_as_doca_ctx(p->txq), p->gpu), "GPU data path");
  // No PE is attached: only the GPU consumes the TX CQ.
  check(doca_ctx_start(doca_eth_txq_as_doca_ctx(p->txq)), "start GPU TX");
  p->started = true;
  check(doca_eth_txq_get_gpu_handle(p->txq, &p->gpu_txq), "GPU TX handle");
  if (!slots || !stride || size_t(stride) > (SIZE_MAX - 65535) / slots)
    throw std::runtime_error("invalid GPUNetIO packet pool size");
  const size_t allocation = (size_t(slots) * stride + 65535) & ~size_t(65535);
  void* unused = nullptr;
  check(doca_gpu_mem_alloc(p->gpu, allocation, 65536, DOCA_GPU_MEM_TYPE_GPU, &p->packet, &unused),
        "GPU packet allocation");
  check(doca_mmap_create(&p->mmap), "packet map");
  check(doca_mmap_add_dev(p->mmap, p->dev), "packet map NIC");
  check(doca_mmap_set_permissions(p->mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE), "packet access");
  const auto export_result = doca_gpu_dmabuf_fd(p->gpu, p->packet, allocation, &p->dmabuf);
  if (export_result == DOCA_SUCCESS) {
    check(doca_mmap_set_dmabuf_memrange(p->mmap, p->dmabuf, p->packet, 0, allocation),
          "DMA-BUF map");
  } else if (export_result == DOCA_ERROR_NOT_SUPPORTED) {
    check(doca_mmap_set_memrange(p->mmap, p->packet, allocation), "peermem packet map");
  } else {
    check(export_result, "DMA-BUF export");
  }
  check(doca_mmap_start(p->mmap), "register GPU packet");
  check(doca_mmap_get_mkey(p->mmap, p->dev, &p->key), "GPU packet key");
  p->key = htonl(p->key);
  impl_ = p.release();
}

Gpunetio::~Gpunetio() {
  delete impl_;
}
void* Gpunetio::queue() const {
  return impl_->gpu_txq;
}
uint8_t* Gpunetio::packet() const {
  const auto slot = impl_->next_slot++ % impl_->slots;
  return static_cast<uint8_t*>(impl_->packet) + size_t(slot) * impl_->stride;
}
uint32_t Gpunetio::mkey_be() const {
  return impl_->key;
}
std::vector<PreparedTx> Gpunetio::prepare_gpu_window(uint32_t n, uint32_t bytes, uint64_t epoch,
                                                     uint64_t interval, uint64_t lead,
                                                     daqiri::GpuTxTicket* tickets) {
  auto& p = *impl_;
  if (!p.cached_valid) {
    if (cudaMemcpy(&p.cached, p.gpu_txq, sizeof(p.cached), cudaMemcpyDeviceToHost) != cudaSuccess)
      throw std::runtime_error("read SDK prebuild queue metadata");
    p.cached_valid = true;
  }
  if (!n || n > 32 || 2 * n > uint32_t(p.cached.wqe_mask) + 1 ||
      p.cpu_pi + uint64_t(2) * n > UINT32_MAX)
    throw std::runtime_error("invalid SDK GPU prebuild window/producer index");
  const auto first_slot = p.next_slot;
  if (prebuild_sdk_window(p.gpu_txq, static_cast<uint8_t*>(p.packet), p.slots, p.stride, first_slot,
                          n, bytes, p.key, epoch, interval, lead, tickets) != cudaSuccess ||
      cudaStreamSynchronize(nullptr) != cudaSuccess)
    throw std::runtime_error("SDK GPU WQE prebuild");
  uint64_t reserved = 0;
  if (cudaMemcpy(&reserved,
                 reinterpret_cast<uint8_t*>(p.gpu_txq) + offsetof(doca_gpu_eth_txq, sq_rsvd_index),
                 sizeof(reserved), cudaMemcpyDeviceToHost) != cudaSuccess ||
      reserved != p.cpu_pi + uint64_t(2) * n)
    throw std::runtime_error("SDK GPU reserved index disagrees with prebuilt tickets");
  if (std::getenv("DAQIRI_PREBUILT_AUDIT")) {
    using Entry = std::array<uint32_t, 16>;
    std::vector<Entry> actual(n * 2);
    const uint32_t first = p.cpu_pi & p.cached.wqe_mask;
    const uint32_t count = std::min<uint32_t>(n * 2, uint32_t(p.cached.wqe_mask) + 1 - first);
    if (cudaMemcpy(actual.data(), reinterpret_cast<void*>(p.cached.wqe_addr + first * 64),
                   count * 64, cudaMemcpyDeviceToHost) != cudaSuccess ||
        (count < n * 2 &&
         cudaMemcpy(actual.data() + count, reinterpret_cast<void*>(p.cached.wqe_addr),
                    (n * 2 - count) * 64, cudaMemcpyDeviceToHost) != cudaSuccess))
      throw std::runtime_error("read SDK prebuilt audit");
    for (uint32_t j = 0; j < n; ++j) {
      Entry wait{}, send{};
      const auto pi = p.cpu_pi + 2 * j;
      wait[0] = htobe32((uint32_t(uint16_t(pi)) << 8) | DOCA_GPUNETIO_ETH_MLX5_OPCODE_WAIT |
                        (DOCA_GPUNETIO_ETH_MLX5_OPC_MOD_WAIT_TIME << 24));
      wait[1] = p.cached.const_wait_be;
      wait[2] = p.cached.const_cflags_first_err_be;
      wait[4] = p.cached.const_wait_wqe_op_be;
      uint64_t timestamp = 0;
      check(doca_eth_txq_calculate_timestamp(p.txq, epoch + uint64_t(j) * interval + lead,
                                             &timestamp),
            "audit host timestamp conversion");
      std::memcpy(&wait[8], &timestamp, sizeof(timestamp));
      wait[10] = wait[11] = UINT32_MAX;
      send[0] = htobe32((uint32_t(uint16_t(pi + 1)) << 8) | DOCA_GPUNETIO_ETH_MLX5_OPCODE_SEND);
      send[1] = p.cached.sq_num_shift8_be_1ds;
      send[2] = p.cached.const_cflags_always_be;
      auto* cs = reinterpret_cast<uint8_t*>(send.data()) + 20;
      if (p.cached.enable_l3_cs_offload) *cs |= DOCA_GPUNETIO_ETH_WQE_L3_CSUM;
      if (p.cached.enable_l4_cs_offload) *cs |= DOCA_GPUNETIO_ETH_WQE_L4_CSUM;
      send[8] = htobe32(bytes);
      send[9] = p.key;
      uint64_t address = htobe64(reinterpret_cast<uint64_t>(p.packet) +
                                 size_t((first_slot + j) % p.slots) * p.stride);
      std::memcpy(&send[10], &address, sizeof(address));
      if (actual[2 * j] != wait || actual[2 * j + 1] != send) {
        for (int k = 0; k < 16; ++k)
          fprintf(stderr,
                  "WQE AUDIT pair=%u word=%d wait=%08x expected=%08x send=%08x expected=%08x\n", j,
                  k, be32toh(actual[2 * j][k]), be32toh(wait[k]), be32toh(actual[2 * j + 1][k]),
                  be32toh(send[k]));
        throw std::runtime_error("SDK GPU WQE audit disagrees with CPU oracle");
      }
    }
    fprintf(stderr, "SDK WQE AUDIT passed n=%u first_pi=%lu including reserved fields\n", n,
            p.cpu_pi);
  }
  std::vector<PreparedTx> result;
  result.reserve(n);
  for (uint32_t j = 0; j < n; ++j) {
    PreparedTx packet{};
    packet.packet = this->packet();
    p.cpu_pi += 2;
    packet.submission.doorbell = p.cached.wqe_db;
    packet.submission.record = p.cached.wqe_db_rec;
    // Private benchmark metadata, consumed only by DocaPrebuilt. This field
    // carries the full PI, not a NIC-endian DBR value or a DAQIRI submission.
    packet.submission.record_value = uint32_t(p.cpu_pi);
    result.push_back(packet);
  }
  return result;
}

}  // namespace tx_deadline
