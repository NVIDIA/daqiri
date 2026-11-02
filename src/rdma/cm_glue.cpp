// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "cm_api.h"

extern "C" const daqiri::detail::RdmaCmApi* daqiri_get_cm_verbs_api_v1(uint32_t abi_version,
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
