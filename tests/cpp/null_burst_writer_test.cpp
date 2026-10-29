// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include "src/engines/ibverbs/daqiri_ibverbs_engine.h"

namespace daqiri {
void initialize_engine(Engine* engine);
}
namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
std::vector<uint8_t> read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  require(stream.good(), "writer output missing");
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
struct EngineScope {
  daqiri::IbverbsEngine engine;
  EngineScope() {
    daqiri::initialize_engine(&engine);
  }
  ~EngineScope() {
    daqiri::initialize_engine(nullptr);
  }
};
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    char name[] = "/tmp/daqiri-null-writer-XXXXXX";
    const char* result = mkdtemp(name);
    require(result != nullptr, "mkdtemp failed");
    path = result;
  }
  ~TempDir() {
    std::filesystem::remove_all(path);
  }
};
void run(const char* endpoint) {
  using namespace daqiri;
  EngineScope scope;
  TempDir dir;
  BurstParams burst{};
  burst.hdr.hdr.num_pkts = 1;
  burst.hdr.hdr.num_segs = 4;
  std::array<uint8_t, 2> first{1, 2};
  std::array<uint8_t, 3> last{3, 4, 5};
  std::array<void*, 4> pointers{nullptr, first.data(), nullptr, last.data()};
  std::array<uint32_t, 4> lengths{3, 2, 5, 3};
  for (size_t seg = 0; seg < 4; ++seg) {
    burst.pkts[seg] = &pointers[seg];
    burst.pkt_lens[seg] = &lengths[seg];
  }
  S3Writer* s3 = nullptr;
  if (endpoint != nullptr) {
    S3WriterConfig config{};
    config.bucket = "null-writer-test";
    config.region = "us-east-1";
    config.endpoint_override = endpoint;
    config.path_style = true;
    require(daqiri_s3_writer_create(config, &s3) == Status::SUCCESS, "S3 writer creation failed");
  }
  try {
    for (bool all_null : {false, true}) {
      pointers = {nullptr, all_null ? nullptr : first.data(), nullptr,
                  all_null ? nullptr : last.data()};
      burst.hdr.hdr.burst_flags = (all_null ? 15U : 5U) << DAQIRI_BURST_FLAG_NULL_SEGMENT_SHIFT;
      std::vector<uint8_t> expected{0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 3, 4, 5};
      if (all_null) std::fill(expected.begin(), expected.end(), 0);
      for (uint64_t offset : {0, 2, 3, 4, 6, 10, 13, 14}) {
        const auto prefix = std::to_string(all_null) + "-" + std::to_string(offset);
        FileWriteHandle* handle = nullptr;
        require(daqiri_write_raw_to_file_async(&burst, dir.path.string(), prefix, offset,
                                               &handle) == Status::SUCCESS,
                "raw writer rejected null spans");
        FileWriteStatus result{};
        const size_t start = std::min<size_t>(offset, expected.size());
        require(daqiri_file_write_wait(handle, &result) == Status::SUCCESS &&
                    result.completed_packets == 1 &&
                    result.bytes_written == expected.size() - start,
                "raw writer completion accounting differs");
        require(daqiri_file_write_destroy(handle) == Status::SUCCESS, "raw writer cleanup failed");
        require(read(dir.path / (prefix + "_0")) ==
                    std::vector<uint8_t>(expected.begin() + start, expected.end()),
                "raw writer did not preserve discarded bytes and logical offsets");
        if (s3 != nullptr && (offset == 0 || offset == 2 || offset == 6 || offset == 13)) {
          S3WriteHandle* upload = nullptr;
          require(daqiri_write_raw_to_s3_objects_async(s3, &burst, prefix, offset, &upload) ==
                      Status::SUCCESS,
                  "S3 writer rejected null spans");
          S3WriteStatus uploaded{};
          require(daqiri_s3_write_wait(upload, &uploaded) == Status::SUCCESS, "S3 upload failed");
          require(daqiri_s3_write_destroy(upload) == Status::SUCCESS, "S3 upload cleanup failed");
        }
      }
      const auto prefix = all_null ? "all-null" : "mixed";
      require(daqiri_write_pcap_to_file(&burst, dir.path.string(), prefix) == Status::SUCCESS,
              "pcap writer rejected null spans");
      const auto pcap = read(dir.path / (std::string(prefix) + ".pcap"));
      require(pcap.size() == 24 + 16 + expected.size(), "pcap size differs");
      uint32_t included = 0, original = 0;
      std::memcpy(&included, pcap.data() + 24 + 8, 4);
      std::memcpy(&original, pcap.data() + 24 + 12, 4);
      require(included == expected.size() && original == expected.size(),
              "pcap lengths lost discarded spans");
      require(std::equal(expected.begin(), expected.end(), pcap.begin() + 40), "pcap bytes differ");
    }
    burst.hdr.hdr.burst_flags = 0;
    require(daqiri_write_raw_to_file(&burst, dir.path.string(), "bad", 0) == Status::NULL_PTR,
            "raw writer silently accepted an unmarked null pointer");
    require(daqiri_write_pcap_to_file(&burst, dir.path.string(), "bad") == Status::NULL_PTR,
            "pcap writer silently accepted an unmarked null pointer");
    if (s3 != nullptr) {
      S3WriteHandle* upload = nullptr;
      require(
          daqiri_write_raw_to_s3_objects_async(s3, &burst, "bad", 0, &upload) == Status::NULL_PTR &&
              upload == nullptr,
          "S3 writer silently accepted an unmarked null pointer");
    }
  } catch (...) {
    if (s3 != nullptr) daqiri_s3_writer_destroy(s3);
    throw;
  }
  if (s3 != nullptr) require(daqiri_s3_writer_destroy(s3) == Status::SUCCESS, "S3 cleanup failed");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    run(argc > 1 ? argv[1] : nullptr);
    std::cout << "PASS: raw/pcap/S3 null-span bytes, offsets, and completion accounting\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
