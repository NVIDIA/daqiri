// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <infiniband/mlx5dv.h>
#include "src/rdma/cm_api.h"
#include "src/engines/ibverbs/mlx5_prm_min.h"

namespace {
ibv_qp_ex extended{};
mlx5dv_qp_ex dv{};
std::vector<int> steps;
std::array<uint8_t, 48> wait_bytes{};
uint64_t wait_id = 0, send_id = 0;
unsigned send_flags = 0;
int post_result = 0;
ibv_qp created_qp{};
uint32_t remote_vendor = 0, negotiated_options = 0, advertised_vendor = 0;
int creations = 0, destructions = 0, init_result = 0;
bool wait_supported = true, realtime = true;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void start(ibv_qp_ex*) {
  steps.push_back(1);
}
void raw(mlx5dv_qp_ex*, const void* wqe) {
  steps.push_back(2);
  require(extended.wr_flags == 0, "WAIT requested successful completions");
  wait_id = extended.wr_id;
  std::memcpy(wait_bytes.data(), wqe, wait_bytes.size());
}
void send(ibv_qp_ex* qp) {
  steps.push_back(3);
  send_id = qp->wr_id;
  send_flags = qp->wr_flags;
}
void sge(ibv_qp_ex*, uint32_t key, uint64_t addr, uint32_t length) {
  require(key == 123 && addr == 456 && length == 64, "DMA descriptor changed");
  steps.push_back(4);
}
void inline_data(ibv_qp_ex*, void* addr, size_t length) {
  require(addr == reinterpret_cast<void*>(456) && length == 64, "inline data changed");
  steps.push_back(5);
}
int complete(ibv_qp_ex*) {
  steps.push_back(6);
  return post_result;
}
}  // namespace

// Test the production glue's builder transaction without requiring a device.
extern "C" ibv_qp_ex* __wrap_ibv_qp_to_qp_ex(ibv_qp*) {
  return &extended;
}
extern "C" mlx5dv_qp_ex* __wrap_mlx5dv_qp_ex_from_ibv_qp_ex(ibv_qp_ex*) {
  return &dv;
}
extern "C" bool __wrap_mlx5dv_is_supported(ibv_device*) {
  return true;
}
extern "C" int __wrap_mlx5dv_devx_general_cmd(ibv_context*, const void*, size_t, void* out,
                                              size_t) {
  void* cap = DEVX_ADDR_OF(query_hca_cap_out, out, capability);
  DEVX_SET(cmd_hca_cap_min, cap, wait_on_time, wait_supported);
  DEVX_SET(cmd_hca_cap_min, cap, device_frequency_khz, realtime ? 1000000 : 156250);
  return 0;
}
extern "C" ibv_qp* __wrap_mlx5dv_create_qp(ibv_context*, ibv_qp_init_attr_ex* attr,
                                           mlx5dv_qp_init_attr* dv_attr) {
  require(attr->send_ops_flags == IBV_QP_EX_WITH_SEND && (attr->comp_mask & IBV_QP_INIT_ATTR_PD) &&
              (attr->comp_mask & IBV_QP_INIT_ATTR_SEND_OPS_FLAGS) &&
              dv_attr->send_ops_flags == MLX5DV_QP_EX_WITH_RAW_WQE,
          "timed QP did not enable extended SEND and raw WQEs");
  ++creations;
  return &created_qp;
}
extern "C" int __wrap_ibv_query_ece(ibv_qp*, ibv_ece* ece) {
  ece->vendor_id = 0x2c9;
  ece->options = 7;
  return 0;
}
extern "C" int __wrap_ibv_set_ece(ibv_qp*, ibv_ece* ece) {
  require(ece->vendor_id == 0x2c9, "ECE vendor was not normalized to the local vendor");
  negotiated_options = ece->options;
  return 0;
}
extern "C" int __wrap_rdma_get_remote_ece(rdma_cm_id* id, ibv_ece* ece) {
  require(id->qp == nullptr, "CM ECE queried after QP publication");
  ece->vendor_id = remote_vendor;
  ece->options = 5;
  return 0;
}
extern "C" int __wrap_rdma_set_local_ece(rdma_cm_id* id, ibv_ece* ece) {
  require(id->qp == nullptr, "CM ECE advertised after QP publication");
  advertised_vendor = ece->vendor_id;
  return 0;
}
extern "C" int __wrap_rdma_init_qp_attr(rdma_cm_id*, ibv_qp_attr* attr, int* mask) {
  require(attr->qp_state == IBV_QPS_INIT, "missing CM INIT transition");
  *mask = IBV_QP_STATE | IBV_QP_PORT;
  return 0;
}
extern "C" int __wrap_ibv_modify_qp(ibv_qp*, ibv_qp_attr*, int mask) {
  require(mask == (IBV_QP_STATE | IBV_QP_PORT), "CM INIT attributes were not applied");
  return init_result;
}
extern "C" int __wrap_ibv_destroy_qp(ibv_qp*) {
  ++destructions;
  return 0;
}

int main() {
  try {
    ibv_device device{};
    ibv_context context{};
    context.device = &device;
    ibv_pd pd{};
    rdma_cm_id id{};
    id.verbs = &context;
    ibv_qp_init_attr attr{};
    attr.qp_type = IBV_QPT_RC;
    for (uint32_t vendor : {0u, 0x2c9u, 0x1234u}) {
      remote_vendor = vendor;
      advertised_vendor = 0;
      negotiated_options = 99;
      require(daqiri::detail::create_timed_qp(&id, &pd, &attr) == 0 && id.qp == &created_qp &&
                  id.pd == &pd && advertised_vendor == 0x2c9 &&
                  negotiated_options == (vendor == 0       ? 99u
                                         : vendor == 0x2c9 ? 5u
                                                           : 0u),
              "timed QP lost CM ECE interoperability");
      id.qp = nullptr;
    }
    const int before = creations;
    for (bool no_wait : {false, true}) {
      wait_supported = !no_wait;
      realtime = no_wait;
      require(daqiri::detail::create_timed_qp(&id, &pd, &attr) != 0 && errno == EOPNOTSUPP &&
                  creations == before && id.qp == nullptr,
              "unsupported timing capabilities did not reject QP setup");
    }
    wait_supported = realtime = true;
    init_result = EIO;
    require(daqiri::detail::create_timed_qp(&id, &pd, &attr) != 0 && errno == EIO &&
                destructions == 1 && id.qp == nullptr,
            "failed INIT leaked or published the timed QP");
    init_result = 0;
    extended.wr_start = start;
    extended.wr_send = send;
    extended.wr_set_sge = sge;
    extended.wr_set_inline_data = inline_data;
    extended.wr_complete = complete;
    dv.wr_raw_wqe = raw;
    ibv_qp qp{};
    qp.qp_num = 0x123456;
    ibv_sge data{456, 64, 123};
    ibv_send_wr wr{};
    wr.wr_id = 987;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &data;
    wr.num_sge = 1;
    constexpr uint64_t time = 1793275200123456789ULL;
    for (bool inlined : {false, true}) {
      for (int result : {0, ENOMEM}) {
        steps.clear();
        post_result = result;
        wr.send_flags = IBV_SEND_SIGNALED | (inlined ? IBV_SEND_INLINE : 0);
        require(daqiri::detail::post_timed_send(&qp, &wr, time) == result,
                "posting failure was not propagated");
        require(steps == std::vector<int>{1, 2, 3, inlined ? 5 : 4, 6},
                "WAIT and SEND did not share one builder transaction");
        require(wait_id == 987 && send_id == 987 && send_flags == wr.send_flags,
                "completion ID or signaling policy changed");
        mlx5_wqe_ctrl_seg ctrl{};
        mlx5_wqe_wseg wait{};
        std::memcpy(&ctrl, wait_bytes.data(), sizeof(ctrl));
        std::memcpy(&wait, wait_bytes.data() + sizeof(ctrl), sizeof(wait));
        require(be32toh(ctrl.opmod_idx_opcode) == ((2u << 24) | 0x0f) &&
                    be32toh(ctrl.qpn_ds) == ((qp.qp_num << 8) | 3) && ctrl.fm_ce_se == 4,
                "incorrect WAIT control encoding");
        require(
            be32toh(wait.operation) == 5 && wait.lkey == 0 && wait.va_high == 0 &&
                wait.va_low == 0 &&
                be64toh(wait.value) == (((time / 1000000000ULL) << 32) | (time % 1000000000ULL)) &&
                be64toh(wait.mask) == ((8ULL << 32) - 1),
            "incorrect real-time clock conversion or comparison mask");
      }
    }
    std::cout << "PASS: RDMA timing capabilities, CM ECE, WAIT encoding, posting, and failures\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
