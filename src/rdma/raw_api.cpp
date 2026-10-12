// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw_api.h"
#if DAQIRI_IBVERBS_DLOPEN
#include "glue_loader.h"
#endif

namespace daqiri::detail {

const RawVerbsApi* load_raw_api(std::string& error) {
#if DAQIRI_IBVERBS_DLOPEN
  // Intentionally retained across engine shutdown and static destruction.
  static auto* library = new GlueLibrary<RawVerbsApi>;
  return library->load("libdaqiri_raw_verbs_glue.so.1", "daqiri_get_raw_verbs_api_v1", error);
#else
  error.clear();
  return daqiri_get_raw_verbs_api_v1(1, sizeof(RawVerbsApi));
#endif
}

}  // namespace daqiri::detail
