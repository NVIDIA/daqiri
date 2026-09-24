/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <daqiri/daqiri.h>

#include "src/engine.h"

#include <iostream>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: daqiri_config_validate <config.yaml> [...]\n";
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
