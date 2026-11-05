// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <dlfcn.h>
#include <link.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace daqiri::detail {

// Gives dladdr an address in the engine DSO (or the static consumer executable).
inline void rdma_glue_anchor() {}

template <typename Api>
class GlueLibrary {
 public:
  const Api* load(const char* filename, const char* symbol, std::string& error,
                  uint32_t abi_version = 1) {
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
      void* link_map_info = nullptr;
      if (dladdr1(reinterpret_cast<void*>(&rdma_glue_anchor), &info, &link_map_info,
                  RTLD_DL_LINKMAP) != 0 &&
          info.dli_fname != nullptr) {
        std::string owner(info.dli_fname);
        const auto* object = static_cast<const link_map*>(link_map_info);
        if (object != nullptr && object->l_name != nullptr && object->l_name[0] == '\0') {
          // The main executable's dli_fname can be a relative argv[0]. Resolving
          // it against cwd fails after chdir(), so ask the kernel for its path.
          std::error_code path_error;
          const auto executable = std::filesystem::read_symlink("/proc/self/exe", path_error);
          owner = path_error ? std::string{} : executable.string();
        }
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
    const Api* candidate = getter(abi_version, sizeof(Api));
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
