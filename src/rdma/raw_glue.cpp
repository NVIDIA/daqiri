// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw_api.h"

extern "C" const daqiri::detail::RawVerbsApi* daqiri_get_raw_verbs_api_v1(uint32_t abi_version,
                                                                          size_t struct_size) {
  if (abi_version != 1 || struct_size != sizeof(daqiri::detail::RawVerbsApi)) {
    return nullptr;
  }
  static const daqiri::detail::RawVerbsApi api{
      1,
      sizeof(daqiri::detail::RawVerbsApi),
#define DAQIRI_RDMA_FUNCTION(symbol, member) &::symbol,
#include "raw_functions.inc"
#undef DAQIRI_RDMA_FUNCTION
  };
  return &api;
}
