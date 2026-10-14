// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "src/rdma/raw_api.h"
#include "src/rdma/cm_api.h"
#include "src/rdma/glue_loader.h"

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "load";
  const bool change_directory = mode == "adjacent-chdir";
  const bool adjacent = mode == "adjacent" || change_directory;
  const auto expected_directory = std::filesystem::current_path();
  if (change_directory) {
    if (argc != 3) {
      std::cerr << "adjacent-chdir requires a working directory\n";
      return 1;
    }
    std::filesystem::current_path(argv[2]);
  }
  if (adjacent && (std::getenv("DAQIRI_RDMA_GLUE_DIR") != nullptr ||
                   std::getenv("LD_LIBRARY_PATH") != nullptr)) {
    std::cerr << "Adjacent search requires unset environment overrides\n";
    return 1;
  }
  std::string error;
  const auto* raw = daqiri::detail::load_raw_api(error);
  if (mode != "load" && !adjacent) {
    if (raw != nullptr || error.find(mode) == std::string::npos) {
      std::cerr << "Unexpected raw loader result: " << error << '\n';
      return 1;
    }
    const auto* cm = daqiri::detail::load_cm_api(error);
    if (cm != nullptr || error.find(mode) == std::string::npos) {
      std::cerr << "Unexpected CM loader result: " << error << '\n';
      return 1;
    }
    return 0;
  }
  if (raw == nullptr || !error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto* cm = daqiri::detail::load_cm_api(error);
  if (cm == nullptr || !error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }

  // Verify that both tables came from the copied DSOs, rather than a globally
  // installed library that could hide a broken search beside the executable.
  if (adjacent) {
    for (const void* table : {static_cast<const void*>(raw), static_cast<const void*>(cm)}) {
      Dl_info info{};
      if (dladdr(table, &info) == 0 || info.dli_fname == nullptr ||
          std::filesystem::canonical(info.dli_fname).parent_path() != expected_directory) {
        std::cerr << "Glue library was not loaded from beside the executable\n";
        return 1;
      }
    }
  }

  // Exercise real library calls without requiring a NIC or creating resources.
  int count = 0;
  auto** devices = raw->get_device_list(&count);
  if (devices == nullptr) {
    return 1;
  }
  raw->free_device_list(devices);
  if (cm->wc_status_str(IBV_WC_SUCCESS) == nullptr) {
    return 1;
  }

  // Verify that successful loads remain cached even if the search path changes,
  // and that concurrent engine initialization sees the same immutable tables.
  setenv("DAQIRI_RDMA_GLUE_DIR", "/nonexistent-daqiri-glue", 1);
  std::vector<std::thread> threads;
  std::vector<int> passed(8, 0);
  for (size_t i = 0; i < passed.size(); ++i) {
    threads.emplace_back([&, i]() {
      std::string thread_error;
      passed[i] = daqiri::detail::load_raw_api(thread_error) == raw && thread_error.empty() &&
                  daqiri::detail::load_cm_api(thread_error) == cm && thread_error.empty();
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (int result : passed) {
    if (!result) {
      return 1;
    }
  }
  return 0;
}
