// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <infiniband/verbs.h>
#include <daqiri/types.h>

namespace daqiri::detail {
inline bool rdma_operation_supported(ibv_qp_type type, RDMAOpCode opcode) {
  switch (opcode) {
    case RDMAOpCode::SEND:
    case RDMAOpCode::RECEIVE:
      return type == IBV_QPT_RC || type == IBV_QPT_UC || type == IBV_QPT_UD;
    case RDMAOpCode::RDMA_WRITE:
    case RDMAOpCode::RDMA_WRITE_IMM:
      return type == IBV_QPT_RC || type == IBV_QPT_UC;
    case RDMAOpCode::RDMA_READ:
      return type == IBV_QPT_RC;
    default:
      return false;
  }
}
}  // namespace daqiri::detail
