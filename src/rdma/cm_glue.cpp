// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "cm_api.h"
#include <cerrno>
#include <infiniband/mlx5dv.h>
#include "../engines/ibverbs/mlx5_prm_min.h"

namespace daqiri::detail {

int create_timed_qp(rdma_cm_id* id, ibv_pd* pd, ibv_qp_init_attr* attr) {
  // Keep ordinary RDMA independent of mlx5 hardware; this path is opt-in and
  // must never silently fall back to immediate transmission.
  if (id->qp || attr->qp_type != IBV_QPT_RC || !mlx5dv_is_supported(id->verbs->device)) {
    errno = EOPNOTSUPP;
    return -1;
  }
  uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)]{};
  uint32_t out[DEVX_ST_SZ_DW(query_hca_cap_out)]{};
  DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
  DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_HCA_CAP_OPMOD_GENERAL_CUR);
  if (mlx5dv_devx_general_cmd(id->verbs, in, sizeof(in), out, sizeof(out))) return -1;
  const void* cap = DEVX_ADDR_OF(query_hca_cap_out, out, capability);
  if (!DEVX_GET(cmd_hca_cap_min, cap, wait_on_time) ||
      DEVX_GET(cmd_hca_cap_min, cap, device_frequency_khz) != 1000000u) {
    errno = EOPNOTSUPP;
    return -1;
  }

  ibv_qp_init_attr_ex extended{};
  extended.qp_context = attr->qp_context;
  extended.send_cq = attr->send_cq;
  extended.recv_cq = attr->recv_cq;
  extended.cap = attr->cap;
  extended.qp_type = attr->qp_type;
  extended.sq_sig_all = attr->sq_sig_all;
  extended.pd = pd;
  extended.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
  extended.send_ops_flags = IBV_QP_EX_WITH_SEND;
  mlx5dv_qp_init_attr dv{};
  dv.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS;
  dv.send_ops_flags = MLX5DV_QP_EX_WITH_RAW_WQE;
  auto* qp = mlx5dv_create_qp(id->verbs, &extended, &dv);
  if (!qp) return -1;

  // Preserve CM's ECE negotiation as well as its INIT transition. CM rejects
  // a response whose vendor differs from the local ECE vendor cached on the
  // ID. Its public ECE helpers must run before publishing id->qp.
  ibv_ece local{}, remote{};
  int ret = ibv_query_ece(qp, &local);
  if (ret == EOPNOTSUPP) ret = 0;
  if (!ret) ret = rdma_get_remote_ece(id, &remote);
  if (!ret && remote.vendor_id) {
    ibv_ece negotiated{};
    negotiated.vendor_id = local.vendor_id;
    negotiated.options = remote.vendor_id == local.vendor_id ? remote.options : 0;
    ret = ibv_set_ece(qp, &negotiated);
    if (ret == EOPNOTSUPP) ret = 0;
  }
  ibv_qp_attr initial{};
  initial.qp_state = IBV_QPS_INIT;
  int mask = 0;
  if (!ret) ret = rdma_init_qp_attr(id, &initial, &mask);
  if (!ret) ret = ibv_modify_qp(qp, &initial, mask);
  if (!ret && remote.vendor_id) {
    ret = ibv_query_ece(qp, &local);
    if (ret == EOPNOTSUPP) ret = 0;
  }
  if (!ret && local.vendor_id) ret = rdma_set_local_ece(id, &local);
  if (ret) {
    const int error = ret > 0 ? ret : errno;
    ibv_destroy_qp(qp);
    errno = error;
    return -1;
  }
  id->pd = pd;
  id->qp = qp;
  attr->cap = extended.cap;
  return 0;
}

int post_timed_send(ibv_qp* qp, ibv_send_wr* wr, uint64_t timestamp_ns) {
  auto* extended = ibv_qp_to_qp_ex(qp);
  if (!extended) return EOPNOTSUPP;
  auto* dv = mlx5dv_qp_ex_from_ibv_qp_ex(extended);
  // Only SEND is supported by the RDMA engine's public submission path.
  if (!dv || wr->opcode != IBV_WR_SEND || wr->num_sge != 1 || wr->next) return EINVAL;
  struct {
    mlx5_wqe_ctrl_seg ctrl;
    mlx5_wqe_wseg wait;
  } wqe{};
  static_assert(sizeof(wqe) == 48);
  wqe.ctrl.opmod_idx_opcode = htobe32((MLX5_OPC_MOD_WAIT_TIME << 24) | MLX5_OPCODE_WAIT);
  wqe.ctrl.qpn_ds = htobe32((qp->qp_num << 8) | 3u);
  wqe.ctrl.fm_ce_se = MLX5_COMP_ONLY_FIRST_ERR << MLX5_COMP_MODE_OFFSET;
  wqe.wait.operation = htobe32(MLX5_WAIT_COND_CYCLIC_SMALLER);
  wqe.wait.value = htobe64(((timestamp_ns / 1000000000ULL) << 32) | (timestamp_ns % 1000000000ULL));
  wqe.wait.mask = htobe64((8ULL << 32) - 1);

  // A single builder transaction either posts both requests or rolls back
  // both. Only the SEND requests successful completions; its CQE reclaims the
  // preceding unsignaled WAIT as well. The provider maintains WQE indices.
  ibv_wr_start(extended);
  extended->wr_id = wr->wr_id;
  extended->wr_flags = 0;
  mlx5dv_wr_raw_wqe(dv, &wqe);
  extended->wr_id = wr->wr_id;
  extended->wr_flags = wr->send_flags;
  ibv_wr_send(extended);
  if (wr->send_flags & IBV_SEND_INLINE) {
    ibv_wr_set_inline_data(extended, reinterpret_cast<void*>(wr->sg_list->addr),
                           wr->sg_list->length);
  } else {
    ibv_wr_set_sge(extended, wr->sg_list->lkey, wr->sg_list->addr, wr->sg_list->length);
  }
  return ibv_wr_complete(extended);
}
}  // namespace daqiri::detail

extern "C" const daqiri::detail::RdmaCmApi* daqiri_get_cm_verbs_api_v2(uint32_t abi_version,
                                                                       size_t struct_size) {
  if (abi_version != daqiri::detail::RdmaCmApi::kAbiVersion ||
      struct_size != sizeof(daqiri::detail::RdmaCmApi)) {
    return nullptr;
  }
  static const daqiri::detail::RdmaCmApi api{
      daqiri::detail::RdmaCmApi::kAbiVersion,
      sizeof(daqiri::detail::RdmaCmApi),
#define DAQIRI_RDMA_FUNCTION(symbol, member) &::symbol,
#include "cm_functions.inc"
#undef DAQIRI_RDMA_FUNCTION
  };
  return &api;
}
