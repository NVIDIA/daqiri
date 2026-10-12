// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "src/rdma/raw_api.h"
#include "src/rdma/cm_api.h"

#if DAQIRI_TEST_GLUE_MISSING_SYMBOL
extern "C" void unrelated_symbol() {}
#else
extern "C" const daqiri::detail::RawVerbsApi* daqiri_get_raw_verbs_api_v1(uint32_t, size_t) {
  // Correct header, but no function pointers: the loader must reject it.
  static const daqiri::detail::RawVerbsApi api{1, sizeof(daqiri::detail::RawVerbsApi)};
  return &api;
}
extern "C" const daqiri::detail::RdmaCmApi* daqiri_get_cm_verbs_api_v1(uint32_t, size_t) {
  return nullptr;  // Models a glue DSO that rejects the requested ABI.
}
#endif
