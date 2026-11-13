// SPDX-License-Identifier: Apache-2.0
#include "tx_deadline_gdr.h"
#include "tx_deadline_gdr_map.h"
#include "tx_deadline_gdr_prm.h"
#include <infiniband/mlx5dv.h>
#include <cuda.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <ctime>

namespace tx_deadline {
namespace {
void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(std::string(what) + ": " + strerror(errno));
}
void cu(cudaError_t e, const char* what) {
  if (e != cudaSuccess) throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(e));
}
struct Memory {
  void *ptr = nullptr, *gpu = nullptr;
  size_t size = 0;
  bool device = false, host_registered = false;
  mlx5dv_devx_umem* umem = nullptr;
  ~Memory() {
    if (umem) mlx5dv_devx_umem_dereg(umem);
    if (!ptr) return;
    if (device)
      cudaFree(ptr);
    else {
      if (host_registered) cudaHostUnregister(ptr);
      free(ptr);
    }
  }
  void allocate(ibv_context* ctx, size_t bytes, bool dev, int fill = 0) {
    size = (bytes + 65535) & ~size_t(65535);
    device = dev;
    int fd = -1;
    if (dev) {
      cu(cudaMalloc(&ptr, size), "allocate GPU queue memory");
      gpu = ptr;
      cu(cudaMemset(ptr, fill, size), "initialize GPU queue memory");
      const auto r = cuMemGetHandleForAddressRange(&fd, reinterpret_cast<CUdeviceptr>(ptr), size,
                                                   CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
      if (r != CUDA_SUCCESS) {
        const char* error = nullptr;
        cuGetErrorString(r, &error);
        throw std::runtime_error(std::string("export GPU UMEM: ") + error);
      }
      mlx5dv_devx_umem_in in{};
      in.size = size;
      in.access = IBV_ACCESS_LOCAL_WRITE;
      in.pgsz_bitmap = uint64_t(1) << 12;
      in.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
      in.dmabuf_fd = fd;
      umem = mlx5dv_devx_umem_reg_ex(ctx, &in);
      const int dma_error = errno;
      close(fd);
      if (!umem) {
        fprintf(stderr, "GPU UMEM DMA-BUF failed (%s); trying installed peer-memory registration\n",
                strerror(dma_error));
        umem = mlx5dv_devx_umem_reg(ctx, ptr, size, IBV_ACCESS_LOCAL_WRITE);
      }
    } else {
      require(posix_memalign(&ptr, 65536, size) == 0, "allocate host queue memory");
      cu(cudaHostRegister(ptr, size, cudaHostRegisterMapped), "map host queue to GPU");
      host_registered = true;
      memset(ptr, fill, size);
      cu(cudaHostGetDevicePointer(&gpu, ptr, 0), "host queue GPU address");
      mlx5dv_devx_umem_in in{};
      in.addr = ptr;
      in.size = size;
      in.access = IBV_ACCESS_LOCAL_WRITE;
      in.pgsz_bitmap = uint64_t(1) << 12;
      umem = mlx5dv_devx_umem_reg_ex(ctx, &in);
    }
    require(umem, "register independent queue UMEM");
  }
};
template <size_t N, size_t M>
mlx5dv_devx_obj* create(ibv_context* ctx, uint32_t (&in)[N], uint32_t (&out)[M],
                        const char* label) {
  auto* obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
  if (!obj) {
    fprintf(stderr, "%s: status=%u syndrome=%#x\n", label,
            unsigned(reinterpret_cast<uint8_t*>(out)[0]), be32toh(out[1]));
    require(false, label);
  }
  return obj;
}
}  // namespace

struct GdrTx::Impl {
  ibv_context* ctx = nullptr;
  ibv_pd* pd = nullptr;
  mlx5dv_devx_uar* uar = nullptr;
  mlx5dv_devx_obj *td = nullptr, *tis = nullptr, *sq = nullptr, *cq_obj = nullptr;
  Memory sqmem, dbr, cq;
  GdrMap gdr;
  void* packets = nullptr;
  ibv_mr* packet_mr = nullptr;
  GdrQueueView* view = nullptr;
  void *uar_base = nullptr, *uar_gpu = nullptr;
  uint64_t doorbell = 0, pi = 0;
  uint32_t cqn = 0, sqn = 0, slots = 0, stride = 0, next = 0;
  static constexpr uint32_t capacity = 1024;
  ~Impl() {
    if (sq) mlx5dv_devx_obj_destroy(sq);
    if (cq_obj) mlx5dv_devx_obj_destroy(cq_obj);
    if (tis) mlx5dv_devx_obj_destroy(tis);
    if (td) mlx5dv_devx_obj_destroy(td);
    if (view) cudaFree(view);
    if (uar_base) cudaHostUnregister(uar_base);
    if (uar) mlx5dv_devx_free_uar(uar);
    if (packet_mr) ibv_dereg_mr(packet_mr);
    if (packets) cudaFree(packets);
    // UMEM registrations must die before their context.
    for (auto* m : {&sqmem, &dbr, &cq}) {
      if (m->umem) {
        mlx5dv_devx_umem_dereg(m->umem);
        m->umem = nullptr;
      }
    }
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
  }
};

GdrTx::GdrTx(const char* bdf, uint32_t slots, uint32_t stride) {
  if (!slots || !stride || size_t(stride) > (SIZE_MAX - 65535) / slots)
    throw std::runtime_error("invalid GDRCopy packet pool size");
  auto p = std::make_unique<Impl>();
  p->slots = slots;
  p->stride = stride;
  int count = 0;
  auto** devices = ibv_get_device_list(&count);
  require(devices, "enumerate RDMA devices");
  for (int i = 0; i < count; ++i) {
    char path[256], actual[512];
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device",
             ibv_get_device_name(devices[i]));
    const auto n = readlink(path, actual, sizeof(actual) - 1);
    if (n < 0) continue;
    actual[n] = 0;
    const auto* name = strrchr(actual, '/');
    if (strcmp(name ? name + 1 : actual, bdf)) continue;
    mlx5dv_context_attr attr{};
    attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;
    p->ctx = mlx5dv_open_device(devices[i], &attr);
    break;
  }
  ibv_free_device_list(devices);
  require(p->ctx, "open native DevX device");
  p->pd = ibv_alloc_pd(p->ctx);
  require(p->pd, "allocate native PD");
  mlx5dv_pd dvpd{};
  mlx5dv_obj obj{};
  obj.pd.in = p->pd;
  obj.pd.out = &dvpd;
  require(mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD) == 0, "query PD number");
  p->uar = mlx5dv_devx_alloc_uar(p->ctx, MLX5DV_UAR_ALLOC_TYPE_NC);
  require(p->uar, "allocate native NC UAR");
  const size_t page = sysconf(_SC_PAGESIZE);
  const auto address = reinterpret_cast<uintptr_t>(p->uar->reg_addr);
  p->uar_base = reinterpret_cast<void*>(address & ~(page - 1));
  cu(cudaHostRegister(p->uar_base, page, cudaHostRegisterIoMemory | cudaHostRegisterMapped),
     "map native UAR to GPU");
  cu(cudaHostGetDevicePointer(&p->uar_gpu, p->uar_base, 0), "UAR GPU address");
  p->doorbell = reinterpret_cast<uint64_t>(p->uar_gpu) + (address & (page - 1));
  p->sqmem.allocate(p->ctx, Impl::capacity * 64, true);
  p->dbr.allocate(p->ctx, 65536, false);
  p->cq.allocate(p->ctx, Impl::capacity * 64 + 64, false, 0xff);
  memset(static_cast<uint8_t*>(p->cq.ptr) + Impl::capacity * 64, 0, 64);
  uint32_t eqn = 0;
  require(mlx5dv_devx_query_eqn(p->ctx, 0, &eqn) == 0, "query event queue");
  uint32_t ci[DEVX_ST_SZ_DW(create_cq_in)]{}, co[DEVX_ST_SZ_DW(create_cq_out)]{};
  DEVX_SET(create_cq_in, ci, opcode, MLX5_CMD_OP_CREATE_CQ);
  auto* cqc = DEVX_ADDR_OF(create_cq_in, ci, cq_context);
  DEVX_SET(cqc, cqc, log_cq_size, 10);
  DEVX_SET(cqc, cqc, uar_page, p->uar->page_id);
  DEVX_SET(cqc, cqc, c_eqn, eqn);
  DEVX_SET(cqc, cqc, dbr_umem_valid, 1);
  DEVX_SET(cqc, cqc, dbr_umem_id, p->cq.umem->umem_id);
  DEVX_SET64(cqc, cqc, dbr_addr, Impl::capacity * 64);
  DEVX_SET(create_cq_in, ci, cq_umem_valid, 1);
  DEVX_SET(create_cq_in, ci, cq_umem_id, p->cq.umem->umem_id);
  p->cq_obj = create(p->ctx, ci, co, "CREATE_CQ");
  p->cqn = DEVX_GET(create_cq_out, co, cqn);
  uint32_t ti[DEVX_ST_SZ_DW(alloc_transport_domain_in)]{},
      to[DEVX_ST_SZ_DW(alloc_transport_domain_out)]{};
  DEVX_SET(alloc_transport_domain_in, ti, opcode, MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
  p->td = create(p->ctx, ti, to, "ALLOC_TD");
  uint32_t tsi[DEVX_ST_SZ_DW(create_tis_in)]{}, tso[DEVX_ST_SZ_DW(create_tis_out)]{};
  DEVX_SET(create_tis_in, tsi, opcode, 0x912);
  auto* tisc = DEVX_ADDR_OF(create_tis_in, tsi, ctx);
  DEVX_SET(tisc, tisc, transport_domain,
           DEVX_GET(alloc_transport_domain_out, to, transport_domain));
  p->tis = create(p->ctx, tsi, tso, "CREATE_TIS");
  uint32_t si[DEVX_ST_SZ_DW(create_sq_in)]{}, so[DEVX_ST_SZ_DW(create_sq_out)]{};
  DEVX_SET(create_sq_in, si, opcode, 0x904);
  auto* sqc = DEVX_ADDR_OF(create_sq_in, si, ctx);
  DEVX_SET(sqc, sqc, flush_in_error_en, 1);
  DEVX_SET(sqc, sqc, cqn, DEVX_GET(create_cq_out, co, cqn));
  DEVX_SET(sqc, sqc, tis_lst_sz, 1);
  DEVX_SET(sqc, sqc, tis_num_0, DEVX_GET(create_tis_out, tso, tisn));
  DEVX_SET(sqc, sqc, ts_format, 2);
  auto* wq = DEVX_ADDR_OF(sqc, sqc, wq);
  DEVX_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
  DEVX_SET(wq, wq, pd, dvpd.pdn);
  DEVX_SET(wq, wq, uar_page, p->uar->page_id);
  DEVX_SET(wq, wq, log_wq_stride, 6);
  DEVX_SET(wq, wq, log_wq_sz, 10);
  DEVX_SET(wq, wq, wq_umem_valid, 1);
  DEVX_SET(wq, wq, wq_umem_id, p->sqmem.umem->umem_id);
  DEVX_SET(wq, wq, dbr_umem_valid, 1);
  DEVX_SET(wq, wq, dbr_umem_id, p->dbr.umem->umem_id);
  // Send doorbell record occupies word 1; word 0 is the receive record.
  DEVX_SET64(wq, wq, dbr_addr, 0);
  p->sq = create(p->ctx, si, so, "CREATE_SQ");
  p->sqn = DEVX_GET(create_sq_out, so, sqn);
  uint32_t mi[DEVX_ST_SZ_DW(modify_sq_in)]{}, mo[DEVX_ST_SZ_DW(modify_sq_out)]{};
  DEVX_SET(modify_sq_in, mi, opcode, 0x905);
  DEVX_SET(modify_sq_in, mi, sqn, p->sqn);
  DEVX_SET(sqc, DEVX_ADDR_OF(modify_sq_in, mi, ctx), state, 1);
  require(mlx5dv_devx_obj_modify(p->sq, mi, sizeof(mi), mo, sizeof(mo)) == 0, "SQ RESET to READY");
  const size_t packet_bytes = (size_t(slots) * stride + 65535) & ~size_t(65535);
  cu(cudaMalloc(&p->packets, packet_bytes), "allocate native GPU packets");
  int fd = -1;
  require(
      cuMemGetHandleForAddressRange(&fd, reinterpret_cast<CUdeviceptr>(p->packets), packet_bytes,
                                    CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0) == CUDA_SUCCESS,
      "export native GPU packets");
  p->packet_mr = ibv_reg_dmabuf_mr(p->pd, 0, packet_bytes, reinterpret_cast<uint64_t>(p->packets),
                                   fd, IBV_ACCESS_LOCAL_WRITE);
  close(fd);
  require(p->packet_mr, "register native GPU packets");
  GdrQueueView view{reinterpret_cast<uint64_t>(p->cq.gpu),
                    reinterpret_cast<uint64_t>(p->cq.gpu) + Impl::capacity * 64, 0, Impl::capacity};
  cu(cudaMalloc(reinterpret_cast<void**>(&p->view), sizeof(view)), "allocate native queue view");
  cu(cudaMemcpy(p->view, &view, sizeof(view), cudaMemcpyHostToDevice), "cache native queue view");
  p->gdr.map(p->sqmem.ptr, p->sqmem.size);
  fprintf(stderr, "GDRCopy GPU SQ=%u host DBREC CQ=%u capacity=%u\n", p->sqn, p->cqn,
          Impl::capacity);
  impl_ = std::move(p);
}
GdrTx::~GdrTx() = default;
void* GdrTx::queue() const {
  return impl_->view;
}
daqiri::GpuTxQueue GdrTx::handle() const {
  return {impl_->doorbell, reinterpret_cast<uint64_t>(impl_->dbr.gpu) + 4};
}
std::vector<PreparedTx> GdrTx::prepare_window(uint32_t n, uint32_t bytes, uint64_t epoch,
                                              uint64_t interval, uint64_t lead) {
  auto& p = *impl_;
  // Caller completes the previous window before reuse. One packet is published at a time.
  if (!n || n > 32 || n > p.slots || !bytes || bytes > p.stride)
    throw std::runtime_error("invalid GDRCopy window");
  std::vector<std::array<uint32_t, 16>> wqes(n * 2);
  std::vector<PreparedTx> result;
  result.reserve(n);
  const uint32_t first = p.pi & (Impl::capacity - 1);
  for (uint32_t j = 0; j < n; ++j) {
    PreparedTx packet{};
    packet.packet = static_cast<uint8_t*>(p.packets) + size_t(p.next++ % p.slots) * p.stride;
    auto* wait = wqes[j * 2].data();
    auto* send = wqes[j * 2 + 1].data();
    wait[0] =
        htobe32((uint32_t(MLX5_OPC_MOD_WAIT_TIME) << 24) | (uint32_t(uint16_t(p.pi)) << 8) | 0x0f);
    wait[1] = htobe32((p.sqn << 8) | 3);
    wait[2] = htobe32(uint32_t(MLX5_COMP_ONLY_FIRST_ERR) << MLX5_COMP_MODE_OFFSET);
    wait[4] = htobe32(3);  // Same cyclic wait condition and mask as DOCA 3.2.
    const uint64_t ns = epoch + uint64_t(j) * interval + lead;
    const uint64_t timestamp = htobe64(((ns / 1000000000ULL) << 32) | (ns % 1000000000ULL));
    std::memcpy(&wait[8], &timestamp, sizeof(timestamp));
    wait[10] = wait[11] = UINT32_MAX;
    send[0] = htobe32((uint32_t(uint16_t(p.pi + 1)) << 8) | 0x0a);
    send[1] = htobe32((p.sqn << 8) | 3);
    send[2] = htobe32(8u);           // One timestamped completion per SEND.
    send[5] = htobe32(0xc0000000u);  // L3 + L4 checksum offloads.
    send[8] = htobe32(bytes);
    send[9] = htobe32(p.packet_mr->lkey);
    const uint64_t va = htobe64(reinterpret_cast<uint64_t>(packet.packet));
    std::memcpy(&send[10], &va, sizeof(va));
    p.pi += 2;
    const uint32_t bell[2] = {htobe32(uint32_t(p.pi << 8)), htobe32(p.sqn << 8)};
    packet.submission.doorbell = p.doorbell;
    packet.submission.record = reinterpret_cast<uint64_t>(p.dbr.gpu) + 4;
    std::memcpy(&packet.submission.doorbell_value, bell, sizeof(bell));
    packet.submission.record_value = htobe32(uint32_t(p.pi) & 0xffff);
    result.push_back(packet);
  }
  const uint32_t first_count = std::min(n * 2, Impl::capacity - first);
  p.gdr.copy(size_t(first) * 64, wqes.data(), size_t(first_count) * 64);
  if (first_count < n * 2)
    p.gdr.copy(0, wqes.data() + first_count, size_t(n * 2 - first_count) * 64);
  GdrMap::store_barrier();
  // Ticket publication subsequently performs the CPU CUDA ordering API before commands.
  return result;
}
}  // namespace tx_deadline
