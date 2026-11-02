// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

namespace daqiri::detail {

// Private glue ABI. Bump the version when the function table changes.
struct RdmaCmApi {
  static constexpr uint32_t kAbiVersion = 2;
  uint32_t abi_version;
  size_t struct_size;
#define DAQIRI_RDMA_FUNCTION(symbol, member) decltype(&::symbol) member;
#include "cm_functions.inc"
#undef DAQIRI_RDMA_FUNCTION

  bool valid() const {
    if (abi_version != kAbiVersion || struct_size != sizeof(RdmaCmApi)) {
      return false;
    }
#define DAQIRI_RDMA_FUNCTION(symbol, member) \
  if (member == nullptr) {                   \
    return false;                            \
  }
#include "cm_functions.inc"
#undef DAQIRI_RDMA_FUNCTION
    return true;
  }
};

// Returns nullptr with a diagnostic on failure. Only successful loads are cached.
const RdmaCmApi* load_cm_api(std::string& error);

}  // namespace daqiri::detail

extern "C" const daqiri::detail::RdmaCmApi* daqiri_get_cm_verbs_api_v1(uint32_t abi_version,
                                                                       size_t struct_size);
