// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

namespace daqiri::detail {

// Private glue ABI. Bump the version when the function table changes.
struct RawVerbsApi {
  uint32_t abi_version;
  size_t struct_size;
#define DAQIRI_RDMA_FUNCTION(symbol, member) decltype(&::symbol) member;
#include "raw_functions.inc"
#undef DAQIRI_RDMA_FUNCTION

  bool valid() const {
    if (abi_version != 1 || struct_size != sizeof(RawVerbsApi)) {
      return false;
    }
#define DAQIRI_RDMA_FUNCTION(symbol, member) \
  if (member == nullptr) {                   \
    return false;                            \
  }
#include "raw_functions.inc"
#undef DAQIRI_RDMA_FUNCTION
    return true;
  }
};

// Returns nullptr with a diagnostic on failure. Only successful loads are cached.
const RawVerbsApi* load_raw_api(std::string& error);

}  // namespace daqiri::detail

extern "C" const daqiri::detail::RawVerbsApi* daqiri_get_raw_verbs_api_v1(uint32_t abi_version,
                                                                          size_t struct_size);
