/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <daqiri/daqiri.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_DIRECT
#define O_DIRECT 00040000
#endif

#if DAQIRI_ENABLE_GDS
#include <cufile.h>
#endif

namespace daqiri {

namespace {

#if DAQIRI_ENABLE_GDS
static constexpr size_t kMaxCuFileBatchEntries = 128;
#endif

static constexpr uint32_t kPcapMagicUsec = 0xa1b2c3d4;
static constexpr uint16_t kPcapVersionMajor = 2;
static constexpr uint16_t kPcapVersionMinor = 4;
static constexpr uint32_t kPcapLinktypeEthernet = 1;

enum class SegmentPointerKind {
  HOST,
  DEVICE,
};

enum class BurstFileLayout {
  RAW,
  PCAP,
};

struct PcapGlobalHeader {
  uint32_t magic_number;
  uint16_t version_major;
  uint16_t version_minor;
  int32_t thiszone;
  uint32_t sigfigs;
  uint32_t snaplen;
  uint32_t network;
};

struct PcapPacketHeader {
  uint32_t ts_sec;
  uint32_t ts_usec;
  uint32_t incl_len;
  uint32_t orig_len;
};

static_assert(sizeof(PcapGlobalHeader) == 24,
              "classic pcap global header must be 24 bytes");
static_assert(sizeof(PcapPacketHeader) == 16,
              "classic pcap packet header must be 16 bytes");

struct FileResource {
  std::filesystem::path path;
  int fd = -1;
  bool remove_on_error = false;
  bool raw_truncated = false;
#if DAQIRI_ENABLE_GDS
  int gds_fd = -1;
  CUfileHandle_t handle = nullptr;
  bool registered = false;
#endif
};

#if DAQIRI_ENABLE_GDS
struct IoCookie {
  uint32_t packet_index = 0;
  size_t expected_bytes = 0;
  bool seen = false;
};

struct BatchState {
  CUfileBatchHandle_t batch = nullptr;
  size_t begin = 0;
  size_t count = 0;
  std::vector<CUfileIOEvents_t> events;
  bool submitted = false;
  bool done = false;
};
#endif

} // namespace

struct FileWriteHandle {
  std::vector<FileResource> files;
#if DAQIRI_ENABLE_GDS
  std::vector<CUfileIOParams_t> params;
  std::vector<IoCookie> cookies;
  std::vector<BatchState> batches;
#endif
  std::vector<uint32_t> packet_pending;
  std::vector<bool> packet_failed;
  std::vector<bool> packet_done;
  FileWriteStatus status;
  size_t total_entries = 0;
  size_t completed_entries = 0;
  bool done = false;
  bool resources_released = false;
  Status final_status = Status::SUCCESS;
};

namespace {

#if DAQIRI_ENABLE_GDS
Status g_driver_status = Status::SUCCESS;
std::once_flag g_driver_once;

Status open_cufile_driver() {
  std::call_once(g_driver_once, []() {
    const CUfileError_t status = cuFileDriverOpen();
    if (status.err != CU_FILE_SUCCESS) {
      DAQIRI_LOG_WARN("cuFileDriverOpen failed with cuFile error {}; "
                      "device-memory file writes "
                      "require GPUDirect Storage support",
                      static_cast<int>(status.err));
      g_driver_status = Status::NOT_SUPPORTED;
    }
  });
  return g_driver_status;
}

Status cufile_error_to_status(CUfileError_t status, const char *op_name) {
  if (status.err == CU_FILE_SUCCESS) {
    return Status::SUCCESS;
  }
  DAQIRI_LOG_ERROR("{} failed with cuFile error {}", op_name,
                   static_cast<int>(status.err));
  return Status::GENERIC_FAILURE;
}
#endif

Status validate_write_request(BurstParams *burst,
                              const std::string &absolute_path,
                              const std::string &file_prefix,
                              FileWriteHandle **handle) {
  if (burst == nullptr || handle == nullptr) {
    return Status::NULL_PTR;
  }
  *handle = nullptr;

  const int64_t num_packets = get_num_packets(burst);
  if (num_packets < 0 || static_cast<uint64_t>(num_packets) >
                             std::numeric_limits<uint32_t>::max()) {
    DAQIRI_LOG_ERROR("Invalid packet count for file write: {}", num_packets);
    return Status::INVALID_PARAMETER;
  }

  const int num_segs = burst->hdr.hdr.num_segs;
  if (num_segs <= 0 || num_segs > MAX_NUM_SEGS) {
    DAQIRI_LOG_ERROR("Invalid segment count for file write: {}", num_segs);
    return Status::INVALID_PARAMETER;
  }

  if (file_prefix.empty() || file_prefix.find('/') != std::string::npos ||
      file_prefix.find('\\') != std::string::npos) {
    DAQIRI_LOG_ERROR("Invalid file prefix '{}'", file_prefix);
    return Status::INVALID_PARAMETER;
  }

  std::error_code ec;
  const std::filesystem::path dir(absolute_path);
  if (!dir.is_absolute() || !std::filesystem::exists(dir, ec) ||
      !std::filesystem::is_directory(dir, ec)) {
    DAQIRI_LOG_ERROR("Output path '{}' must be an existing absolute directory",
                     absolute_path);
    return Status::INVALID_PARAMETER;
  }

  return Status::SUCCESS;
}

SegmentPointerKind classify_segment_pointer(const void *ptr) {
  cudaPointerAttributes attrs{};
  const cudaError_t err = cudaPointerGetAttributes(&attrs, ptr);
  if (err != cudaSuccess) {
    cudaGetLastError();
    return SegmentPointerKind::HOST;
  }

#if defined(CUDART_VERSION) && CUDART_VERSION >= 10000
  if (attrs.type == cudaMemoryTypeDevice ||
      attrs.type == cudaMemoryTypeManaged) {
    return SegmentPointerKind::DEVICE;
  }
#else
  if (attrs.memoryType == cudaMemoryTypeDevice) {
    return SegmentPointerKind::DEVICE;
  }
#endif

  return SegmentPointerKind::HOST;
}

uint64_t get_packet_logical_length(BurstParams *burst, uint32_t packet_index) {
  uint64_t packet_len = 0;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
    packet_len += static_cast<uint64_t>(
        get_segment_packet_length(burst, seg, packet_index));
  }
  return packet_len;
}

Status ensure_device_write_available() {
#if !DAQIRI_ENABLE_GDS
  DAQIRI_LOG_WARN(
      "DAQIRI file write encountered CUDA device memory, but "
      "DAQIRI_ENABLE_GDS=OFF; rebuild with GPUDirect Storage support to write "
      "device-backed bursts directly");
  return Status::NOT_SUPPORTED;
#else
  return open_cufile_driver();
#endif
}

Status preflight_device_segments(BurstParams *burst,
                                 uint64_t packet_data_offset,
                                 BurstFileLayout layout,
                                 bool *has_device_segment_out) {
  const auto num_packets = static_cast<uint32_t>(get_num_packets(burst));
  const auto num_segs = burst->hdr.hdr.num_segs;
  bool has_device_segment = false;

  for (uint32_t pkt = 0; pkt < num_packets; ++pkt) {
    uint64_t bytes_to_skip =
        layout == BurstFileLayout::RAW ? packet_data_offset : 0;
    for (int seg = 0; seg < num_segs; ++seg) {
      const auto seg_len =
          static_cast<uint64_t>(get_segment_packet_length(burst, seg, pkt));
      if (seg_len == 0) {
        continue;
      }

      void *seg_ptr = get_segment_packet_ptr(burst, seg, pkt);
      if (seg_ptr == nullptr) {
        DAQIRI_LOG_ERROR("Null packet segment pointer for packet {} segment {}",
                         pkt, seg);
        return Status::NULL_PTR;
      }

      if (bytes_to_skip >= seg_len) {
        bytes_to_skip -= seg_len;
        continue;
      }

      if (classify_segment_pointer(seg_ptr) == SegmentPointerKind::DEVICE) {
        has_device_segment = true;
      }
      bytes_to_skip = 0;
    }
  }

  if (has_device_segment_out != nullptr) {
    *has_device_segment_out = has_device_segment;
  }
  if (has_device_segment) {
    return ensure_device_write_available();
  }
  return Status::SUCCESS;
}

Status validate_pcap_packet_lengths(BurstParams *burst,
                                    uint32_t *max_packet_len) {
  *max_packet_len = 0;
  const auto num_packets = static_cast<uint32_t>(get_num_packets(burst));

  for (uint32_t pkt = 0; pkt < num_packets; ++pkt) {
    const uint64_t packet_len = get_packet_logical_length(burst, pkt);
    if (packet_len > std::numeric_limits<uint32_t>::max()) {
      DAQIRI_LOG_ERROR("Packet {} is too large for classic pcap: {} bytes", pkt,
                       packet_len);
      return Status::INVALID_PARAMETER;
    }
    *max_packet_len =
        std::max(*max_packet_len, static_cast<uint32_t>(packet_len));
  }

  return Status::SUCCESS;
}

Status open_output_file(FileResource &file, int flags) {
  std::error_code ec;
  const bool existed_before_open = std::filesystem::exists(file.path, ec);
  file.fd = ::open(file.path.c_str(), flags, 0644);
  if (file.fd < 0) {
    DAQIRI_LOG_ERROR("Failed to open '{}': {}", file.path.string(),
                     std::strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  file.remove_on_error = (flags & O_CREAT) != 0 && !existed_before_open;
  return Status::SUCCESS;
}

Status truncate_output_file(FileResource &file) {
  if (file.raw_truncated) {
    return Status::SUCCESS;
  }
  if (::ftruncate(file.fd, 0) != 0) {
    DAQIRI_LOG_ERROR("Failed to truncate '{}': {}", file.path.string(),
                     std::strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  file.raw_truncated = true;
  return Status::SUCCESS;
}

Status pwrite_all(const FileResource &file, const void *data, size_t nbytes,
                  off_t file_offset) {
  const auto *src = static_cast<const uint8_t *>(data);
  size_t written = 0;
  while (written < nbytes) {
    const ssize_t ret = ::pwrite(file.fd, src + written, nbytes - written,
                                 file_offset + static_cast<off_t>(written));
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      DAQIRI_LOG_ERROR("Failed to write '{}': {}", file.path.string(),
                       std::strerror(errno));
      return Status::GENERIC_FAILURE;
    }
    if (ret == 0) {
      DAQIRI_LOG_ERROR("Short write to '{}'", file.path.string());
      return Status::GENERIC_FAILURE;
    }
    written += static_cast<size_t>(ret);
  }
  return Status::SUCCESS;
}

Status pread_all(const FileResource &file, void *data, size_t nbytes,
                 off_t file_offset) {
  auto *dst = static_cast<uint8_t *>(data);
  size_t read_bytes = 0;
  while (read_bytes < nbytes) {
    const ssize_t ret = ::pread(file.fd, dst + read_bytes, nbytes - read_bytes,
                                file_offset + static_cast<off_t>(read_bytes));
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      DAQIRI_LOG_ERROR("Failed to read '{}': {}", file.path.string(),
                       std::strerror(errno));
      return Status::GENERIC_FAILURE;
    }
    if (ret == 0) {
      return Status::INVALID_PARAMETER;
    }
    read_bytes += static_cast<size_t>(ret);
  }
  return Status::SUCCESS;
}

#if DAQIRI_ENABLE_GDS
Status register_output_file(FileResource &file) {
  if (file.registered) {
    return Status::SUCCESS;
  }

  if (file.gds_fd < 0) {
    file.gds_fd = ::open(file.path.c_str(), O_RDWR | O_DIRECT);
    if (file.gds_fd < 0) {
      DAQIRI_LOG_ERROR("Failed to open '{}' with O_DIRECT for cuFile: {}",
                       file.path.string(), std::strerror(errno));
      return Status::GENERIC_FAILURE;
    }
  }

  CUfileDescr_t desc{};
  desc.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;
  desc.handle.fd = file.gds_fd;
  const CUfileError_t status = cuFileHandleRegister(&file.handle, &desc);
  const Status daqiri_status =
      cufile_error_to_status(status, "cuFileHandleRegister");
  if (daqiri_status != Status::SUCCESS) {
    DAQIRI_LOG_ERROR("Failed to register cuFile handle for '{}'",
                     file.path.string());
    return daqiri_status;
  }
  file.registered = true;
  return Status::SUCCESS;
}
#endif

void close_output_file(FileResource &file) {
#if DAQIRI_ENABLE_GDS
  if (file.registered) {
    cuFileHandleDeregister(file.handle);
    file.registered = false;
    file.handle = nullptr;
  }
  if (file.gds_fd >= 0) {
    if (::close(file.gds_fd) != 0) {
      DAQIRI_LOG_ERROR("Failed to close cuFile fd for '{}': {}",
                       file.path.string(), std::strerror(errno));
    }
    file.gds_fd = -1;
  }
#endif
  if (file.fd >= 0) {
    if (::close(file.fd) != 0) {
      DAQIRI_LOG_ERROR("Failed to close '{}': {}", file.path.string(),
                       std::strerror(errno));
    }
    file.fd = -1;
  }
}

void populate_status(const FileWriteHandle &handle, FileWriteStatus *status) {
  if (status != nullptr) {
    *status = handle.status;
  }
}

#if DAQIRI_ENABLE_GDS
bool is_terminal_status(CUfileStatus_t status) {
  return status == CUFILE_COMPLETE || status == CUFILE_FAILED ||
         status == CUFILE_INVALID || status == CUFILE_CANCELED ||
         status == CUFILE_TIMEOUT;
}

bool is_failure_status(CUfileStatus_t status) {
  return status == CUFILE_FAILED || status == CUFILE_INVALID ||
         status == CUFILE_CANCELED || status == CUFILE_TIMEOUT;
}
#endif

void complete_packet_if_ready(FileWriteHandle &handle, uint32_t packet_index) {
  if (packet_index >= handle.packet_pending.size() ||
      handle.packet_done[packet_index]) {
    return;
  }
  if (handle.packet_pending[packet_index] != 0) {
    return;
  }

  handle.packet_done[packet_index] = true;
  if (handle.packet_failed[packet_index]) {
    ++handle.status.failed_packets;
  } else {
    ++handle.status.completed_packets;
  }
}

void release_handle_resources(FileWriteHandle &handle,
                              bool remove_created_files = false) {
  if (handle.resources_released) {
    return;
  }

#if DAQIRI_ENABLE_GDS
  for (auto &batch : handle.batches) {
    if (batch.batch != nullptr) {
      cuFileBatchIODestroy(batch.batch);
      batch.batch = nullptr;
    }
  }
#endif

  std::vector<std::filesystem::path> files_to_remove;
  for (auto &file : handle.files) {
    if (remove_created_files && file.remove_on_error) {
      files_to_remove.push_back(file.path);
    }
    close_output_file(file);
  }
  for (const auto &path : files_to_remove) {
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) && ec) {
      DAQIRI_LOG_ERROR("Failed to remove incomplete output '{}': {}",
                       path.string(), ec.message());
    }
  }

  handle.resources_released = true;
}

Status poll_handle(FileWriteHandle &handle, FileWriteStatus *status) {
  if (handle.done) {
    populate_status(handle, status);
    return handle.final_status;
  }

#if !DAQIRI_ENABLE_GDS
  handle.done = true;
  handle.final_status = handle.status.failed_packets == 0
                            ? Status::SUCCESS
                            : Status::GENERIC_FAILURE;
  populate_status(handle, status);
  return handle.final_status;
#else
  for (auto &batch : handle.batches) {
    if (!batch.submitted || batch.done) {
      continue;
    }

    unsigned nr = static_cast<unsigned>(batch.events.size());
    timespec timeout{};
    const CUfileError_t get_status = cuFileBatchIOGetStatus(
        batch.batch, 0, &nr, batch.events.data(), &timeout);
    const Status daqiri_status =
        cufile_error_to_status(get_status, "cuFileBatchIOGetStatus");
    if (daqiri_status != Status::SUCCESS) {
      handle.final_status = daqiri_status;
      handle.done = true;
      populate_status(handle, status);
      return daqiri_status;
    }

    for (unsigned i = 0; i < nr; ++i) {
      auto *cookie = static_cast<IoCookie *>(batch.events[i].cookie);
      if (cookie == nullptr || cookie->seen) {
        continue;
      }
      if (!is_terminal_status(batch.events[i].status)) {
        continue;
      }

      cookie->seen = true;
      ++handle.completed_entries;

      if (is_failure_status(batch.events[i].status) ||
          (batch.events[i].status == CUFILE_COMPLETE &&
           batch.events[i].ret != cookie->expected_bytes)) {
        handle.packet_failed[cookie->packet_index] = true;
        DAQIRI_LOG_ERROR("cuFile batch write failed for packet {} with status "
                         "{} and {} bytes",
                         cookie->packet_index,
                         static_cast<int>(batch.events[i].status),
                         static_cast<uint64_t>(batch.events[i].ret));
      } else {
        handle.status.bytes_written +=
            static_cast<uint64_t>(batch.events[i].ret);
      }

      if (cookie->packet_index < handle.packet_pending.size() &&
          handle.packet_pending[cookie->packet_index] > 0) {
        --handle.packet_pending[cookie->packet_index];
      }
      complete_packet_if_ready(handle, cookie->packet_index);
    }

    batch.done = std::all_of(
        handle.cookies.begin() + static_cast<std::ptrdiff_t>(batch.begin),
        handle.cookies.begin() +
            static_cast<std::ptrdiff_t>(batch.begin + batch.count),
        [](const IoCookie &cookie) { return cookie.seen; });
  }

  if (handle.completed_entries == handle.total_entries) {
    handle.done = true;
    handle.final_status = handle.status.failed_packets == 0
                              ? Status::SUCCESS
                              : Status::GENERIC_FAILURE;
    populate_status(handle, status);
    return handle.final_status;
  }

  populate_status(handle, status);
  return Status::NOT_READY;
#endif
}

#if DAQIRI_ENABLE_GDS
Status submit_batches(FileWriteHandle &handle) {
  for (size_t begin = 0; begin < handle.params.size();
       begin += kMaxCuFileBatchEntries) {
    BatchState batch;
    batch.begin = begin;
    batch.count =
        std::min(kMaxCuFileBatchEntries, handle.params.size() - begin);
    batch.events.resize(batch.count);

    CUfileError_t setup_status =
        cuFileBatchIOSetUp(&batch.batch, static_cast<int>(batch.count));
    Status daqiri_status =
        cufile_error_to_status(setup_status, "cuFileBatchIOSetUp");
    if (daqiri_status != Status::SUCCESS) {
      handle.total_entries = begin;
      return daqiri_status;
    }

    CUfileError_t submit_status =
        cuFileBatchIOSubmit(batch.batch, static_cast<unsigned>(batch.count),
                            handle.params.data() + batch.begin, 0);
    daqiri_status =
        cufile_error_to_status(submit_status, "cuFileBatchIOSubmit");
    if (daqiri_status != Status::SUCCESS) {
      cuFileBatchIODestroy(batch.batch);
      handle.total_entries = begin;
      return daqiri_status;
    }

    batch.submitted = true;
    handle.batches.push_back(std::move(batch));
  }

  return Status::SUCCESS;
}

Status queue_device_write(FileWriteHandle &handle, FileResource &file,
                          uint32_t packet_index, void *seg_ptr,
                          off_t seg_offset, size_t write_len,
                          off_t file_offset) {
  Status status = ensure_device_write_available();
  if (status != Status::SUCCESS) {
    return status;
  }

  status = register_output_file(file);
  if (status != Status::SUCCESS) {
    return status;
  }

  IoCookie cookie;
  cookie.packet_index = packet_index;
  cookie.expected_bytes = write_len;
  handle.cookies.push_back(cookie);

  CUfileIOParams_t params{};
  params.mode = CUFILE_BATCH;
  params.u.batch.devPtr_base = seg_ptr;
  params.u.batch.file_offset = file_offset;
  params.u.batch.devPtr_offset = seg_offset;
  params.u.batch.size = write_len;
  params.fh = file.handle;
  params.opcode = CUFILE_WRITE;
  params.cookie = &handle.cookies.back();
  handle.params.push_back(params);
  ++handle.packet_pending[packet_index];

  return Status::SUCCESS;
}
#endif

Status queue_or_write_segment(FileWriteHandle &handle, FileResource &file,
                              uint32_t packet_index, void *seg_ptr,
                              off_t seg_offset, size_t write_len,
                              off_t file_offset) {
  if (classify_segment_pointer(seg_ptr) == SegmentPointerKind::HOST) {
    const auto *host_ptr = static_cast<const uint8_t *>(seg_ptr) + seg_offset;
    const Status status = pwrite_all(file, host_ptr, write_len, file_offset);
    if (status == Status::SUCCESS) {
      handle.status.bytes_written += static_cast<uint64_t>(write_len);
    }
    return status;
  }

#if !DAQIRI_ENABLE_GDS
  return ensure_device_write_available();
#else
  return queue_device_write(handle, file, packet_index, seg_ptr, seg_offset,
                            write_len, file_offset);
#endif
}

void finalize_completed_host_only_packet(FileWriteHandle &handle,
                                         uint32_t packet_index) {
  if (handle.packet_pending[packet_index] == 0) {
    handle.packet_done[packet_index] = true;
    ++handle.status.completed_packets;
  }
}

Status finish_handle_submission(std::unique_ptr<FileWriteHandle> &handle,
                                FileWriteHandle **out_handle) {
#if DAQIRI_ENABLE_GDS
  handle->total_entries = handle->params.size();
  if (handle->total_entries != 0) {
    const Status status = submit_batches(*handle);
    if (status != Status::SUCCESS) {
      while (!handle->batches.empty() &&
             poll_handle(*handle, nullptr) == Status::NOT_READY) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      handle->final_status = status;
      handle->done = true;
      release_handle_resources(*handle, true);
      return status;
    }
  } else
#endif
  {
    handle->total_entries = 0;
    handle->done = true;
    handle->final_status = handle->status.failed_packets == 0
                               ? Status::SUCCESS
                               : Status::GENERIC_FAILURE;
  }

  *out_handle = handle.release();
  return Status::SUCCESS;
}

PcapPacketHeader make_pcap_packet_header(uint32_t packet_len) {
  const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  const auto seconds = now_us.count() / 1000000;
  const auto useconds = now_us.count() % 1000000;

  PcapPacketHeader header{};
  header.ts_sec = static_cast<uint32_t>(seconds);
  header.ts_usec = static_cast<uint32_t>(useconds);
  header.incl_len = packet_len;
  header.orig_len = packet_len;
  return header;
}

Status initialize_or_validate_pcap_file(FileResource &file,
                                        uint32_t max_packet_len,
                                        off_t *append_offset,
                                        uint64_t *header_bytes_written) {
  struct stat st {};
  if (::fstat(file.fd, &st) != 0) {
    DAQIRI_LOG_ERROR("Failed to stat '{}': {}", file.path.string(),
                     std::strerror(errno));
    return Status::GENERIC_FAILURE;
  }

  if (st.st_size == 0) {
    const uint32_t snaplen = max_packet_len == 0 ? 65535U : max_packet_len;
    PcapGlobalHeader global_header{};
    global_header.magic_number = kPcapMagicUsec;
    global_header.version_major = kPcapVersionMajor;
    global_header.version_minor = kPcapVersionMinor;
    global_header.thiszone = 0;
    global_header.sigfigs = 0;
    global_header.snaplen = snaplen;
    global_header.network = kPcapLinktypeEthernet;

    const Status status =
        pwrite_all(file, &global_header, sizeof(global_header), 0);
    if (status != Status::SUCCESS) {
      return status;
    }

    *append_offset = static_cast<off_t>(sizeof(global_header));
    *header_bytes_written = sizeof(global_header);
    return Status::SUCCESS;
  }

  if (st.st_size < static_cast<off_t>(sizeof(PcapGlobalHeader))) {
    DAQIRI_LOG_ERROR("Existing pcap file '{}' is too small to append",
                     file.path.string());
    return Status::INVALID_PARAMETER;
  }

  PcapGlobalHeader global_header{};
  Status status = pread_all(file, &global_header, sizeof(global_header), 0);
  if (status != Status::SUCCESS) {
    return status;
  }

  if (global_header.magic_number != kPcapMagicUsec ||
      global_header.version_major != kPcapVersionMajor ||
      global_header.version_minor != kPcapVersionMinor ||
      global_header.network != kPcapLinktypeEthernet) {
    DAQIRI_LOG_ERROR(
        "Existing pcap file '{}' is not a compatible DAQIRI pcap file",
        file.path.string());
    return Status::INVALID_PARAMETER;
  }

  if (max_packet_len > global_header.snaplen) {
    DAQIRI_LOG_ERROR("Existing pcap file '{}' snaplen {} is smaller than burst "
                     "packet size {}",
                     file.path.string(), global_header.snaplen, max_packet_len);
    return Status::INVALID_PARAMETER;
  }

  *append_offset = st.st_size;
  *header_bytes_written = 0;
  return Status::SUCCESS;
}

Status build_raw_write_handle(BurstParams *burst,
                              const std::string &absolute_path,
                              const std::string &file_prefix,
                              uint64_t packet_data_offset,
                              FileWriteHandle **out_handle) {
  Status status =
      validate_write_request(burst, absolute_path, file_prefix, out_handle);
  if (status != Status::SUCCESS) {
    return status;
  }

  bool has_device_segments = false;
  status = preflight_device_segments(
      burst, packet_data_offset, BurstFileLayout::RAW, &has_device_segments);
  if (status != Status::SUCCESS) {
    return status;
  }

  auto handle = std::make_unique<FileWriteHandle>();
  const auto num_packets = static_cast<uint32_t>(get_num_packets(burst));
  const auto num_segs = burst->hdr.hdr.num_segs;
  const auto max_entries =
      static_cast<size_t>(num_packets) * static_cast<size_t>(num_segs);
  handle->files.reserve(num_packets);
#if DAQIRI_ENABLE_GDS
  handle->params.reserve(max_entries);
  handle->cookies.reserve(max_entries);
#else
  static_cast<void>(max_entries);
#endif
  handle->packet_pending.assign(num_packets, 0);
  handle->packet_failed.assign(num_packets, false);
  handle->packet_done.assign(num_packets, false);

  const std::filesystem::path output_dir(absolute_path);
  for (uint32_t pkt = 0; pkt < num_packets; ++pkt) {
    FileResource file;
    file.path = output_dir / (file_prefix + "_" + std::to_string(pkt));

    status = open_output_file(file, O_CREAT | O_RDWR);
    if (status != Status::SUCCESS) {
      handle->files.push_back(std::move(file));
      release_handle_resources(*handle, true);
      return status;
    }

#if DAQIRI_ENABLE_GDS
    if (has_device_segments) {
      status = register_output_file(file);
      if (status != Status::SUCCESS) {
        handle->files.push_back(std::move(file));
        release_handle_resources(*handle, true);
        return status;
      }
    }
#else
    static_cast<void>(has_device_segments);
#endif

    status = truncate_output_file(file);
    if (status != Status::SUCCESS) {
      handle->files.push_back(std::move(file));
      release_handle_resources(*handle, true);
      return status;
    }

    uint64_t bytes_to_skip = packet_data_offset;
    off_t file_offset = 0;
    for (int seg = 0; seg < num_segs; ++seg) {
      const auto seg_len =
          static_cast<uint64_t>(get_segment_packet_length(burst, seg, pkt));
      if (seg_len == 0) {
        continue;
      }

      void *seg_ptr = get_segment_packet_ptr(burst, seg, pkt);
      if (seg_ptr == nullptr) {
        DAQIRI_LOG_ERROR("Null packet segment pointer for packet {} segment {}",
                         pkt, seg);
        handle->files.push_back(std::move(file));
        release_handle_resources(*handle, true);
        return Status::NULL_PTR;
      }

      if (bytes_to_skip >= seg_len) {
        bytes_to_skip -= seg_len;
        continue;
      }

      const auto seg_offset = static_cast<off_t>(bytes_to_skip);
      const auto write_len = static_cast<size_t>(seg_len - bytes_to_skip);
      bytes_to_skip = 0;

      status = queue_or_write_segment(*handle, file, pkt, seg_ptr, seg_offset,
                                      write_len, file_offset);
      if (status != Status::SUCCESS) {
        handle->files.push_back(std::move(file));
        release_handle_resources(*handle, true);
        return status;
      }

      file_offset += static_cast<off_t>(write_len);
    }

    finalize_completed_host_only_packet(*handle, pkt);
    if (handle->packet_pending[pkt] == 0) {
      close_output_file(file);
    }
    handle->files.push_back(std::move(file));
  }

  return finish_handle_submission(handle, out_handle);
}

Status build_pcap_write_handle(BurstParams *burst,
                               const std::string &absolute_path,
                               const std::string &file_prefix,
                               FileWriteHandle **out_handle) {
  Status status =
      validate_write_request(burst, absolute_path, file_prefix, out_handle);
  if (status != Status::SUCCESS) {
    return status;
  }

  uint32_t max_packet_len = 0;
  status = validate_pcap_packet_lengths(burst, &max_packet_len);
  if (status != Status::SUCCESS) {
    return status;
  }

  bool has_device_segments = false;
  status = preflight_device_segments(burst, 0, BurstFileLayout::PCAP,
                                     &has_device_segments);
  if (status != Status::SUCCESS) {
    return status;
  }

  auto handle = std::make_unique<FileWriteHandle>();
  const auto num_packets = static_cast<uint32_t>(get_num_packets(burst));
  const auto num_segs = burst->hdr.hdr.num_segs;
  const auto max_entries =
      static_cast<size_t>(num_packets) * static_cast<size_t>(num_segs);
  handle->files.reserve(1);
#if DAQIRI_ENABLE_GDS
  handle->params.reserve(max_entries);
  handle->cookies.reserve(max_entries);
#else
  static_cast<void>(max_entries);
#endif
  handle->packet_pending.assign(num_packets, 0);
  handle->packet_failed.assign(num_packets, false);
  handle->packet_done.assign(num_packets, false);

  FileResource file;
  file.path = std::filesystem::path(absolute_path) / (file_prefix + ".pcap");
  status = open_output_file(file, O_CREAT | O_RDWR);
  if (status != Status::SUCCESS) {
    handle->files.push_back(std::move(file));
    release_handle_resources(*handle, true);
    return status;
  }

#if DAQIRI_ENABLE_GDS
  if (has_device_segments) {
    status = register_output_file(file);
    if (status != Status::SUCCESS) {
      handle->files.push_back(std::move(file));
      release_handle_resources(*handle, true);
      return status;
    }
  }
#else
  static_cast<void>(has_device_segments);
#endif

  off_t file_offset = 0;
  uint64_t pcap_header_bytes = 0;
  status = initialize_or_validate_pcap_file(file, max_packet_len, &file_offset,
                                            &pcap_header_bytes);
  if (status != Status::SUCCESS) {
    handle->files.push_back(std::move(file));
    release_handle_resources(*handle, true);
    return status;
  }
  handle->status.bytes_written += pcap_header_bytes;

  for (uint32_t pkt = 0; pkt < num_packets; ++pkt) {
    const auto packet_len =
        static_cast<uint32_t>(get_packet_logical_length(burst, pkt));
    const PcapPacketHeader packet_header = make_pcap_packet_header(packet_len);

    status =
        pwrite_all(file, &packet_header, sizeof(packet_header), file_offset);
    if (status != Status::SUCCESS) {
      handle->files.push_back(std::move(file));
      release_handle_resources(*handle, true);
      return status;
    }
    handle->status.bytes_written += sizeof(packet_header);
    file_offset += static_cast<off_t>(sizeof(packet_header));

    for (int seg = 0; seg < num_segs; ++seg) {
      const auto seg_len =
          static_cast<size_t>(get_segment_packet_length(burst, seg, pkt));
      if (seg_len == 0) {
        continue;
      }

      void *seg_ptr = get_segment_packet_ptr(burst, seg, pkt);
      if (seg_ptr == nullptr) {
        DAQIRI_LOG_ERROR("Null packet segment pointer for packet {} segment {}",
                         pkt, seg);
        handle->files.push_back(std::move(file));
        release_handle_resources(*handle, true);
        return Status::NULL_PTR;
      }

      status = queue_or_write_segment(*handle, file, pkt, seg_ptr, 0, seg_len,
                                      file_offset);
      if (status != Status::SUCCESS) {
        handle->files.push_back(std::move(file));
        release_handle_resources(*handle, true);
        return status;
      }

      file_offset += static_cast<off_t>(seg_len);
    }

    finalize_completed_host_only_packet(*handle, pkt);
  }

  if (num_packets == 0 ||
      std::all_of(handle->packet_pending.begin(), handle->packet_pending.end(),
                  [](uint32_t pending) { return pending == 0; })) {
    close_output_file(file);
  }
  handle->files.push_back(std::move(file));

  return finish_handle_submission(handle, out_handle);
}

} // namespace

Status daqiri_write_raw_to_file(BurstParams *burst,
                                const std::string &absolute_path,
                                const std::string &file_prefix,
                                uint64_t packet_data_offset) {
  FileWriteHandle *handle = nullptr;
  Status status = daqiri_write_raw_to_file_async(
      burst, absolute_path, file_prefix, packet_data_offset, &handle);
  if (status != Status::SUCCESS) {
    return status;
  }

  FileWriteStatus write_status{};
  status = daqiri_file_write_wait(handle, &write_status);
  const Status destroy_status = daqiri_file_write_destroy(handle);
  return status == Status::SUCCESS ? destroy_status : status;
}

Status daqiri_write_raw_to_file_async(BurstParams *burst,
                                      const std::string &absolute_path,
                                      const std::string &file_prefix,
                                      uint64_t packet_data_offset,
                                      FileWriteHandle **handle) {
  return build_raw_write_handle(burst, absolute_path, file_prefix,
                                packet_data_offset, handle);
}

Status daqiri_write_pcap_to_file(BurstParams *burst,
                                 const std::string &absolute_path,
                                 const std::string &file_prefix) {
  FileWriteHandle *handle = nullptr;
  Status status = daqiri_write_pcap_to_file_async(burst, absolute_path,
                                                  file_prefix, &handle);
  if (status != Status::SUCCESS) {
    return status;
  }

  FileWriteStatus write_status{};
  status = daqiri_file_write_wait(handle, &write_status);
  const Status destroy_status = daqiri_file_write_destroy(handle);
  return status == Status::SUCCESS ? destroy_status : status;
}

Status daqiri_write_pcap_to_file_async(BurstParams *burst,
                                       const std::string &absolute_path,
                                       const std::string &file_prefix,
                                       FileWriteHandle **handle) {
  return build_pcap_write_handle(burst, absolute_path, file_prefix, handle);
}

Status daqiri_file_write_poll(FileWriteHandle *handle,
                              FileWriteStatus *status) {
  if (handle == nullptr) {
    return Status::NULL_PTR;
  }
  return poll_handle(*handle, status);
}

Status daqiri_file_write_wait(FileWriteHandle *handle,
                              FileWriteStatus *status) {
  if (handle == nullptr) {
    return Status::NULL_PTR;
  }

  while (true) {
    const Status poll_status = poll_handle(*handle, status);
    if (poll_status != Status::NOT_READY) {
      return poll_status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

Status daqiri_file_write_destroy(FileWriteHandle *handle) {
  if (handle == nullptr) {
    return Status::NULL_PTR;
  }

  Status wait_status = Status::SUCCESS;
  if (!handle->done) {
    wait_status = daqiri_file_write_wait(handle, nullptr);
  }
  release_handle_resources(*handle);
  delete handle;
  return wait_status;
}

} // namespace daqiri
