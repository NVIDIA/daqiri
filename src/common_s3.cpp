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
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cuda_runtime.h>

#if DAQIRI_ENABLE_S3
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSAuthSigner.h>
#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#endif

namespace daqiri {

namespace {

static constexpr uint64_t kMaxSinglePutObjectBytes =
    5ULL * 1024ULL * 1024ULL * 1024ULL;

enum class PointerKind {
  HOST,
  CUDA,
};

struct HostStagingBuffer {
  uint8_t *data = nullptr;
  size_t size = 0;

  HostStagingBuffer() = default;
  ~HostStagingBuffer() { reset(); }

  HostStagingBuffer(const HostStagingBuffer &) = delete;
  HostStagingBuffer &operator=(const HostStagingBuffer &) = delete;

  HostStagingBuffer(HostStagingBuffer &&other) noexcept
      : data(other.data), size(other.size) {
    other.data = nullptr;
    other.size = 0;
  }

  HostStagingBuffer &operator=(HostStagingBuffer &&other) noexcept {
    if (this != &other) {
      reset();
      data = other.data;
      size = other.size;
      other.data = nullptr;
      other.size = 0;
    }
    return *this;
  }

  Status allocate(size_t nbytes) {
    reset();
    size = nbytes;
    if (size == 0) {
      return Status::SUCCESS;
    }

    void *raw_data = nullptr;
    const cudaError_t err = cudaMallocHost(&raw_data, size);
    if (err != cudaSuccess) {
      DAQIRI_LOG_ERROR("cudaMallocHost for S3 staging failed: {}",
                       cudaGetErrorString(err));
      data = nullptr;
      size = 0;
      return Status::GENERIC_FAILURE;
    }

    data = static_cast<uint8_t *>(raw_data);
    return Status::SUCCESS;
  }

  void reset() {
    if (data == nullptr) {
      size = 0;
      return;
    }

    release_memory();
    size = 0;
  }

  void release_memory() {
    if (data == nullptr) {
      return;
    }

    const cudaError_t err = cudaFreeHost(data);
    if (err != cudaSuccess) {
      DAQIRI_LOG_ERROR("cudaFreeHost for S3 staging failed: {}",
                       cudaGetErrorString(err));
    }
    data = nullptr;
  }
};

PointerKind classify_pointer(const void *ptr) {
  cudaPointerAttributes attrs{};
  const cudaError_t err = cudaPointerGetAttributes(&attrs, ptr);
  if (err != cudaSuccess) {
    cudaGetLastError();
    return PointerKind::HOST;
  }

#if defined(CUDART_VERSION) && CUDART_VERSION >= 10000
  if (attrs.type == cudaMemoryTypeDevice ||
      attrs.type == cudaMemoryTypeManaged) {
    return PointerKind::CUDA;
  }
#else
  if (attrs.memoryType == cudaMemoryTypeDevice) {
    return PointerKind::CUDA;
  }
#endif

  return PointerKind::HOST;
}

uint64_t packet_length_after_offset(BurstParams *burst, uint32_t packet_index,
                                    uint64_t packet_data_offset) {
  uint64_t packet_len = 0;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
    packet_len += static_cast<uint64_t>(
        get_segment_packet_length(burst, seg, packet_index));
  }

  if (packet_data_offset >= packet_len) {
    return 0;
  }
  return packet_len - packet_data_offset;
}

Status validate_s3_write_request(S3Writer *writer, BurstParams *burst,
                                 const std::string &object_prefix,
                                 S3WriteHandle **handle,
                                 uint32_t *num_packets) {
  if (writer == nullptr || burst == nullptr || handle == nullptr ||
      num_packets == nullptr) {
    return Status::NULL_PTR;
  }
  *handle = nullptr;

  if (object_prefix.empty()) {
    DAQIRI_LOG_ERROR("S3 object prefix must not be empty");
    return Status::INVALID_PARAMETER;
  }

  const int64_t packet_count = get_num_packets(burst);
  if (packet_count < 0 ||
      static_cast<uint64_t>(packet_count) >
          std::numeric_limits<uint32_t>::max()) {
    DAQIRI_LOG_ERROR("Invalid packet count for S3 write: {}", packet_count);
    return Status::INVALID_PARAMETER;
  }

  const int num_segs = burst->hdr.hdr.num_segs;
  if (num_segs <= 0 || num_segs > MAX_NUM_SEGS) {
    DAQIRI_LOG_ERROR("Invalid segment count for S3 write: {}", num_segs);
    return Status::INVALID_PARAMETER;
  }

  *num_packets = static_cast<uint32_t>(packet_count);
  return Status::SUCCESS;
}

Status copy_segment_to_host(const void *src, size_t nbytes, uint8_t *dst) {
  if (nbytes == 0) {
    return Status::SUCCESS;
  }
  if (src == nullptr || dst == nullptr) {
    return Status::NULL_PTR;
  }

  if (classify_pointer(src) == PointerKind::CUDA) {
    const cudaError_t err = cudaMemcpy(dst, src, nbytes, cudaMemcpyDefault);
    if (err != cudaSuccess) {
      DAQIRI_LOG_ERROR("cudaMemcpy for S3 staging failed: {}",
                       cudaGetErrorString(err));
      return Status::GENERIC_FAILURE;
    }
  } else {
    std::memcpy(dst, src, nbytes);
  }

  return Status::SUCCESS;
}

Status copy_packet_to_staging(BurstParams *burst, uint32_t packet_index,
                              uint64_t packet_data_offset,
                              HostStagingBuffer &staging) {
  const uint64_t output_len =
      packet_length_after_offset(burst, packet_index, packet_data_offset);
  if (output_len > kMaxSinglePutObjectBytes ||
      output_len > std::numeric_limits<size_t>::max()) {
    return Status::NOT_SUPPORTED;
  }

  const Status alloc_status = staging.allocate(static_cast<size_t>(output_len));
  if (alloc_status != Status::SUCCESS || output_len == 0) {
    return alloc_status;
  }

  uint64_t bytes_to_skip = packet_data_offset;
  size_t dst_offset = 0;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
    const auto seg_len =
        static_cast<uint64_t>(get_segment_packet_length(burst, seg,
                                                        packet_index));
    if (seg_len == 0) {
      continue;
    }

    if (bytes_to_skip >= seg_len) {
      bytes_to_skip -= seg_len;
      continue;
    }

    auto *seg_ptr =
        static_cast<const uint8_t *>(get_segment_packet_ptr(burst, seg,
                                                           packet_index));
    if (seg_ptr == nullptr) {
      DAQIRI_LOG_ERROR("Null packet segment pointer for packet {} segment {}",
                       packet_index, seg);
      return Status::NULL_PTR;
    }

    const auto seg_offset = static_cast<size_t>(bytes_to_skip);
    const auto copy_len = static_cast<size_t>(seg_len - bytes_to_skip);
    bytes_to_skip = 0;

    const Status status = copy_segment_to_host(seg_ptr + seg_offset, copy_len,
                                               staging.data + dst_offset);
    if (status != Status::SUCCESS) {
      return status;
    }
    dst_offset += copy_len;
  }

  return Status::SUCCESS;
}

#if DAQIRI_ENABLE_S3

static constexpr const char *kAwsAllocTag = "daqiri_s3";

class AwsSdkLease {
public:
  explicit AwsSdkLease(bool owns_sdk) : owns_sdk_(owns_sdk) {}
  ~AwsSdkLease();

  AwsSdkLease(const AwsSdkLease &) = delete;
  AwsSdkLease &operator=(const AwsSdkLease &) = delete;

private:
  bool owns_sdk_ = false;
};

std::mutex g_aws_sdk_mutex;
uint32_t g_aws_sdk_refcount = 0;
Aws::SDKOptions g_aws_sdk_options;

void release_aws_sdk() {
  std::lock_guard<std::mutex> lock(g_aws_sdk_mutex);
  if (g_aws_sdk_refcount == 0) {
    return;
  }
  --g_aws_sdk_refcount;
  if (g_aws_sdk_refcount == 0) {
    Aws::ShutdownAPI(g_aws_sdk_options);
  }
}

AwsSdkLease::~AwsSdkLease() {
  if (owns_sdk_) {
    release_aws_sdk();
  }
}

Status acquire_aws_sdk(bool already_initialized,
                       std::shared_ptr<AwsSdkLease> *lease) {
  if (lease == nullptr) {
    return Status::NULL_PTR;
  }

  if (already_initialized) {
    *lease = std::make_shared<AwsSdkLease>(false);
    return Status::SUCCESS;
  }

  std::lock_guard<std::mutex> lock(g_aws_sdk_mutex);
  if (g_aws_sdk_refcount == 0) {
    Aws::InitAPI(g_aws_sdk_options);
  }
  ++g_aws_sdk_refcount;
  *lease = std::make_shared<AwsSdkLease>(true);
  return Status::SUCCESS;
}

struct S3UploadEntry {
  uint32_t packet_index = 0;
  std::string key;
  HostStagingBuffer staging;
  std::shared_ptr<Aws::StringStream> body;
  Aws::S3::Model::PutObjectRequest request;
  bool submitted = false;
  bool done = false;
};

#endif

} // namespace

#if DAQIRI_ENABLE_S3

struct S3Writer {
  S3WriterConfig config;
  std::shared_ptr<AwsSdkLease> sdk_lease;
  std::shared_ptr<Aws::S3::S3Client> client;
};

struct S3WriteHandle {
  std::shared_ptr<AwsSdkLease> sdk_lease;
  std::shared_ptr<Aws::S3::S3Client> client;
  std::vector<S3UploadEntry> entries;
  std::mutex mutex;
  std::condition_variable cv;
  S3WriteStatus status;
  uint32_t max_inflight_uploads = 1;
  size_t next_to_submit = 0;
  size_t inflight = 0;
  size_t completed = 0;
  bool done = false;
  Status final_status = Status::SUCCESS;
};

namespace {

void populate_status(const S3WriteHandle &handle, S3WriteStatus *status) {
  if (status != nullptr) {
    *status = handle.status;
  }
}

void submit_ready_uploads(S3WriteHandle &handle);

void mark_upload_complete(S3WriteHandle &handle, size_t index, bool success) {
  bool should_submit_more = false;
  {
    std::lock_guard<std::mutex> lock(handle.mutex);
    if (index >= handle.entries.size() || handle.entries[index].done) {
      return;
    }

    handle.entries[index].done = true;
    if (handle.inflight > 0) {
      --handle.inflight;
    }
    ++handle.completed;

    if (success) {
      ++handle.status.completed_objects;
      handle.status.bytes_uploaded +=
          static_cast<uint64_t>(handle.entries[index].staging.size);
    } else {
      ++handle.status.failed_objects;
    }

    if (handle.completed == handle.entries.size()) {
      handle.done = true;
      handle.final_status = handle.status.failed_objects == 0
                                ? Status::SUCCESS
                                : Status::GENERIC_FAILURE;
      handle.cv.notify_all();
      return;
    }

    should_submit_more = true;
  }

  handle.cv.notify_all();
  if (should_submit_more) {
    submit_ready_uploads(handle);
  }
}

void submit_ready_uploads(S3WriteHandle &handle) {
  while (true) {
    size_t index = 0;
    {
      std::lock_guard<std::mutex> lock(handle.mutex);
      if (handle.done ||
          handle.inflight >= handle.max_inflight_uploads ||
          handle.next_to_submit >= handle.entries.size()) {
        return;
      }

      index = handle.next_to_submit++;
      ++handle.inflight;
      handle.entries[index].submitted = true;
    }

    auto *handle_ptr = &handle;
    try {
      handle.client->PutObjectAsync(
          handle.entries[index].request,
          [handle_ptr, index](
              const Aws::S3::S3Client *,
              const Aws::S3::Model::PutObjectRequest &,
              const Aws::S3::Model::PutObjectOutcome &outcome,
              const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
            if (!outcome.IsSuccess()) {
              const auto &error = outcome.GetError();
              DAQIRI_LOG_ERROR("S3 PutObject failed for object {}: {}",
                               handle_ptr->entries[index].key,
                               error.GetMessage().c_str());
            }
            mark_upload_complete(*handle_ptr, index, outcome.IsSuccess());
          },
          nullptr);
    } catch (const std::exception &e) {
      DAQIRI_LOG_ERROR("S3 PutObjectAsync failed for object {}: {}",
                       handle.entries[index].key, e.what());
      mark_upload_complete(handle, index, false);
    }
  }
}

Status prepare_s3_request(S3Writer &writer, S3UploadEntry &entry) {
  entry.body = Aws::MakeShared<Aws::StringStream>(kAwsAllocTag);
  if (entry.staging.size != 0) {
    entry.body->write(reinterpret_cast<const char *>(entry.staging.data),
                      static_cast<std::streamsize>(entry.staging.size));
    if (!entry.body->good()) {
      DAQIRI_LOG_ERROR("Failed to copy S3 object {} into request body",
                       entry.key);
      return Status::GENERIC_FAILURE;
    }
    entry.staging.release_memory();
  }
  entry.body->seekg(0);

  entry.request.SetBucket(writer.config.bucket.c_str());
  entry.request.SetKey(entry.key.c_str());
  entry.request.SetContentLength(static_cast<long long>(entry.staging.size));
  entry.request.SetBody(entry.body);
  return Status::SUCCESS;
}

Status poll_handle(S3WriteHandle &handle, S3WriteStatus *status) {
  std::lock_guard<std::mutex> lock(handle.mutex);
  populate_status(handle, status);
  if (handle.done) {
    return handle.final_status;
  }
  return Status::NOT_READY;
}

} // namespace

Status daqiri_s3_writer_create(const S3WriterConfig &config,
                               S3Writer **writer) {
  if (writer == nullptr) {
    return Status::NULL_PTR;
  }
  *writer = nullptr;

  if (config.bucket.empty() || config.region.empty() ||
      config.max_inflight_uploads == 0) {
    return Status::INVALID_PARAMETER;
  }

  try {
    auto s3_writer = std::make_unique<S3Writer>();
    s3_writer->config = config;

    Status status = acquire_aws_sdk(config.aws_sdk_already_initialized,
                                    &s3_writer->sdk_lease);
    if (status != Status::SUCCESS) {
      return status;
    }

    Aws::Client::ClientConfiguration client_config;
    client_config.region = config.region.c_str();
    if (!config.endpoint_override.empty()) {
      client_config.endpointOverride = config.endpoint_override.c_str();
    }

    s3_writer->client = std::make_shared<Aws::S3::S3Client>(
        client_config,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::RequestDependent,
        !config.path_style);
    *writer = s3_writer.release();
    return Status::SUCCESS;
  } catch (const std::exception &e) {
    DAQIRI_LOG_ERROR("Failed to create S3 writer: {}", e.what());
    return Status::GENERIC_FAILURE;
  }
}

Status daqiri_write_raw_to_s3_objects_async(S3Writer *writer,
                                            BurstParams *burst,
                                            const std::string &object_prefix,
                                            uint64_t packet_data_offset,
                                            S3WriteHandle **out_handle) {
  uint32_t num_packets = 0;
  Status status = validate_s3_write_request(writer, burst, object_prefix,
                                            out_handle, &num_packets);
  if (status != Status::SUCCESS) {
    return status;
  }

  uint64_t staged_bytes = 0;
  uint64_t largest_object_len = 0;
  for (uint32_t pkt = 0; pkt < num_packets; ++pkt) {
    const uint64_t object_len =
        packet_length_after_offset(burst, pkt, packet_data_offset);
    if (object_len > kMaxSinglePutObjectBytes) {
      DAQIRI_LOG_ERROR("S3 object for packet {} is larger than 5 GiB", pkt);
      return Status::NOT_SUPPORTED;
    }
    if (object_len > writer->config.max_staged_bytes ||
        staged_bytes > writer->config.max_staged_bytes - object_len) {
      DAQIRI_LOG_ERROR("S3 request body bytes exceed configured limit");
      return Status::NO_SPACE_AVAILABLE;
    }
    staged_bytes += object_len;
    largest_object_len = std::max(largest_object_len, object_len);
  }

  if (largest_object_len > writer->config.max_staged_bytes ||
      staged_bytes > writer->config.max_staged_bytes - largest_object_len) {
    DAQIRI_LOG_ERROR(
        "S3 request body and staging bytes exceed configured limit");
    return Status::NO_SPACE_AVAILABLE;
  }

  auto handle = std::make_unique<S3WriteHandle>();
  handle->sdk_lease = writer->sdk_lease;
  handle->client = writer->client;
  handle->max_inflight_uploads =
      std::max<uint32_t>(1, writer->config.max_inflight_uploads);
  handle->entries.reserve(num_packets);

  try {
    for (uint32_t pkt = 0; pkt < num_packets; ++pkt) {
      S3UploadEntry entry;
      entry.packet_index = pkt;
      entry.key = object_prefix + "_" + std::to_string(pkt);

      status = copy_packet_to_staging(burst, pkt, packet_data_offset,
                                      entry.staging);
      if (status != Status::SUCCESS) {
        return status;
      }

      status = prepare_s3_request(*writer, entry);
      if (status != Status::SUCCESS) {
        return status;
      }

      handle->entries.push_back(std::move(entry));
    }

    if (handle->entries.empty()) {
      handle->done = true;
      handle->final_status = Status::SUCCESS;
    } else {
      submit_ready_uploads(*handle);
    }

    *out_handle = handle.release();
    return Status::SUCCESS;
  } catch (const std::exception &e) {
    DAQIRI_LOG_ERROR("Failed to submit S3 uploads: {}", e.what());
    return Status::GENERIC_FAILURE;
  }
}

Status daqiri_s3_write_poll(S3WriteHandle *handle, S3WriteStatus *status) {
  if (handle == nullptr) {
    return Status::NULL_PTR;
  }
  return poll_handle(*handle, status);
}

Status daqiri_s3_write_wait(S3WriteHandle *handle, S3WriteStatus *status) {
  if (handle == nullptr) {
    return Status::NULL_PTR;
  }

  std::unique_lock<std::mutex> lock(handle->mutex);
  handle->cv.wait(lock, [&handle]() { return handle->done; });
  populate_status(*handle, status);
  return handle->final_status;
}

Status daqiri_s3_write_destroy(S3WriteHandle *handle) {
  if (handle == nullptr) {
    return Status::NULL_PTR;
  }

  S3WriteStatus status{};
  const Status wait_status = daqiri_s3_write_wait(handle, &status);
  delete handle;
  return wait_status;
}

Status daqiri_s3_writer_destroy(S3Writer *writer) {
  if (writer == nullptr) {
    return Status::NULL_PTR;
  }
  delete writer;
  return Status::SUCCESS;
}

#else

struct S3Writer {};
struct S3WriteHandle {};

Status daqiri_s3_writer_create(const S3WriterConfig &, S3Writer **writer) {
  if (writer == nullptr) {
    return Status::NULL_PTR;
  }
  *writer = nullptr;
  return Status::NOT_SUPPORTED;
}

Status daqiri_write_raw_to_s3_objects_async(S3Writer *, BurstParams *,
                                            const std::string &, uint64_t,
                                            S3WriteHandle **handle) {
  if (handle != nullptr) {
    *handle = nullptr;
  }
  return Status::NOT_SUPPORTED;
}

Status daqiri_s3_write_poll(S3WriteHandle *, S3WriteStatus *) {
  return Status::NOT_SUPPORTED;
}

Status daqiri_s3_write_wait(S3WriteHandle *, S3WriteStatus *) {
  return Status::NOT_SUPPORTED;
}

Status daqiri_s3_write_destroy(S3WriteHandle *) {
  return Status::NOT_SUPPORTED;
}

Status daqiri_s3_writer_destroy(S3Writer *) { return Status::NOT_SUPPORTED; }

#endif

} // namespace daqiri
