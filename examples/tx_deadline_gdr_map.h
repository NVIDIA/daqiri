// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gdrapi.h>
#include <cuda.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

// Pin/map once, outside the command/deadline window.
class GdrMap {
 public:
  GdrMap() = default;
  GdrMap(const GdrMap&) = delete;
  GdrMap& operator=(const GdrMap&) = delete;
  ~GdrMap() {
    if (base_) gdr_unmap(g_, mh_, base_, size_);
    if (pinned_) gdr_unpin_buffer(g_, mh_);
    if (g_) gdr_close(g_);
  }
  static void require(int error, const char* operation) {
    if (error)
      throw std::runtime_error(std::string(operation) + ": error " + std::to_string(error));
  }
  void map(void* gpu, size_t size) {
    unsigned sync = 1;
    require(cuPointerSetAttribute(&sync, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                  reinterpret_cast<CUdeviceptr>(gpu)),
            "SYNC_MEMOPS");
    g_ = gdr_open();
    if (!g_) throw std::runtime_error("gdr_open failed (gdrdrv required)");
    size_ = size;
    require(gdr_pin_buffer(g_, reinterpret_cast<unsigned long>(gpu), size_, 0, 0, &mh_),
            "gdr_pin_buffer");
    pinned_ = true;
    require(gdr_map(g_, mh_, &base_, size_), "gdr_map");
    gdr_info_t info{};
    require(gdr_get_info(g_, mh_, &info), "gdr_get_info");
    if (reinterpret_cast<uint64_t>(gpu) < info.va)
      throw std::runtime_error("invalid GDR mapping base");
    const auto offset = reinterpret_cast<uint64_t>(gpu) - info.va;
    if (offset > info.mapped_size || size > info.mapped_size - offset)
      throw std::runtime_error("GDR mapping too small");
    ptr_ = static_cast<uint8_t*>(base_) + offset;
    fprintf(stderr, "GDR map gpu=%p cpu=%p size=%zu mapping_type=%u wc=%u\n", gpu, ptr_, size,
            unsigned(info.mapping_type), unsigned(info.wc_mapping));
  }
  void copy(size_t offset, const void* input, size_t bytes) {
    if (offset > size_ || bytes > size_ - offset)
      throw std::runtime_error("GDR copy out of bounds");
    require(gdr_copy_to_mapping(mh_, ptr_ + offset, input, bytes), "gdr_copy_to_mapping");
  }
  static void store_barrier() {
#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("sfence" ::: "memory");
#else
#error "GDRCopy publication requires an architecture-specific device store barrier"
#endif
  }

 private:
  gdr_t g_ = nullptr;
  gdr_mh_t mh_{};
  bool pinned_ = false;
  void* base_ = nullptr;
  uint8_t* ptr_ = nullptr;
  size_t size_ = 0;
};
