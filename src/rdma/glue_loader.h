// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <dlfcn.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

namespace daqiri::detail {

// Gives dladdr an address in the engine DSO (or the static consumer executable).
inline void rdma_glue_anchor() {}

template <typename Api>
class GlueLibrary {
 public:
  const Api* load(const char* filename, const char* symbol, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (api_ != nullptr) {
      return api_;
    }

    void* handle = nullptr;
    std::string path;
    const char* directory = std::getenv("DAQIRI_RDMA_GLUE_DIR");
    if (directory != nullptr && directory[0] != '\0') {
      // An explicit directory is authoritative; do not silently load another copy.
      path = std::string(directory) + "/" + filename;
      handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    } else {
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(&rdma_glue_anchor), &info) != 0 &&
          info.dli_fname != nullptr) {
        const std::string owner(info.dli_fname);
        const auto slash = owner.find_last_of('/');
        if (slash != std::string::npos) {
          path = owner.substr(0, slash + 1) + filename;
          handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        }
      }
      if (handle == nullptr) {
        path = filename;
        handle = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
      }
    }
    if (handle == nullptr) {
      error = "dlopen(" + path + "): " + dlerror();
      return nullptr;
    }

    dlerror();
    using Getter = const Api* (*)(uint32_t, size_t);
    auto getter = reinterpret_cast<Getter>(dlsym(handle, symbol));
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr || getter == nullptr) {
      error = "dlsym(" + path + ", " + symbol +
              "): " + (symbol_error == nullptr ? "null entry point" : symbol_error);
      dlclose(handle);
      return nullptr;
    }
    const Api* candidate = getter(1, sizeof(Api));
    if (candidate == nullptr || !candidate->valid()) {
      error = "Incompatible or incomplete RDMA glue API in " + path;
      dlclose(handle);
      return nullptr;
    }

    // Provider callbacks and cleanup can outlive an engine instance. Successful
    // handles intentionally remain open until process exit, including at shutdown.
    api_ = candidate;
    return api_;
  }

 private:
  std::mutex mutex_;
  const Api* api_ = nullptr;
};

}  // namespace daqiri::detail
