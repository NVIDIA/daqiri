/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <daqiri/daqiri.h>

#include "src/engine.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--list-engines") {
    std::cout << "socket";
#if DAQIRI_ENGINE_DPDK
    std::cout << " dpdk";
#endif
#if DAQIRI_ENGINE_IBVERBS || DAQIRI_ENGINE_RDMA
    std::cout << " ibverbs";
#endif
    std::cout << '\n';
    return 0;
  }

  if (argc < 2) {
    std::cerr << "Usage: daqiri_config_validate <config.yaml> [...] | --list-engines\n";
    return 2;
  }

  bool valid = true;
  for (int index = 1; index < argc; ++index) {
    daqiri::NetworkConfig config;
    const auto status = daqiri::parse_network_config_from_yaml_file(argv[index], config);
    if (status != daqiri::Status::SUCCESS || !daqiri::validate_network_config(config)) {
      std::cerr << argv[index] << ": invalid\n";
      valid = false;
      continue;
    }
    std::cout << argv[index] << ": valid\n";
  }
  return valid ? 0 : 1;
}
