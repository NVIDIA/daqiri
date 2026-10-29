// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "cm_api.h"
#if DAQIRI_IBVERBS_DLOPEN
#include "glue_loader.h"
#endif

namespace daqiri::detail {

const RdmaCmApi* load_cm_api(std::string& error) {
#if DAQIRI_IBVERBS_DLOPEN
  // Intentionally retained across engine shutdown and static destruction.
  static auto* library = new GlueLibrary<RdmaCmApi>;
  return library->load("libdaqiri_cm_verbs_glue.so.2", "daqiri_get_cm_verbs_api_v2", error);
#else
  error.clear();
  return daqiri_get_cm_verbs_api_v2(RdmaCmApi::kAbiVersion, sizeof(RdmaCmApi));
#endif
}

}  // namespace daqiri::detail
