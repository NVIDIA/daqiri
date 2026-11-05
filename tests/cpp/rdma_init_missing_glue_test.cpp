// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "src/engines/ibverbs/daqiri_ibverbs_engine.h"
#include "src/engines/rdma/daqiri_rdma_engine.h"

int main() {
  // Missing glue must stop bring-up before allocations or device discovery,
  // including when initialize() is invoked through the internal Engine API.
  daqiri::IbverbsEngine raw;
  raw.initialize();
  raw.shutdown();
  daqiri::RdmaEngine cm;
  cm.initialize();
  cm.shutdown();
  return 0;
}
