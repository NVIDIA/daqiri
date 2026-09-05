// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol.h"

#include <cuda_runtime_api.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace daqiri::ucx_gpu {

struct ReceiverOptions {
  std::string server_endpoint;
  std::string local_endpoint;
  std::uint64_t image_count{1024};
  std::size_t queue_depth{64};
  int gpu_id{0};
  int cpu_core{-1};
  MemoryKind memory_kind{MemoryKind::host_pinned_mapped};
  std::chrono::seconds timeout{30};
};

struct ExternalBatchProducerOptions {
  std::string listen_endpoint{"0.0.0.0:13341"};
  std::uint64_t image_count{1024};
  std::size_t batch_slot_count{16};
  std::size_t max_receiver_queue_depth{64};
  int gpu_id{0};
  int cpu_core{-1};
  MemoryKind memory_kind{MemoryKind::host_pinned_mapped};
  // The UCX-only benchmark waits for a complete receiver credit window so it
  // measures transport rather than the composed pipeline's drop-newest policy.
  // The composed DAQIRI pipeline leaves this false to exercise drop-newest.
  bool wait_for_credit{false};
  std::chrono::seconds timeout{30};
};

struct TransportStats {
  std::uint64_t generated{0};
  std::uint64_t admitted{0};
  std::uint64_t dropped_no_connection{0};
  std::uint64_t dropped_no_credit{0};
  std::uint64_t send_completed{0};
  std::uint64_t send_failed{0};
  std::uint64_t outstanding{0};
  std::uint64_t delivery_unknown{0};
  std::uint64_t delivered{0};
  std::uint64_t released{0};
  std::uint64_t sequence_gaps{0};
  std::uint64_t validation_failures{0};
  std::uint64_t bytes{0};
  std::uint64_t active_nanoseconds{0};
};

class ReceivedImage {
 public:
  ReceivedImage() = default;
  ReceivedImage(const ReceivedImage&) = delete;
  ReceivedImage& operator=(const ReceivedImage&) = delete;
  ReceivedImage(ReceivedImage&& other) noexcept;
  ReceivedImage& operator=(ReceivedImage&& other) = delete;

  void* device_data() const noexcept {
    return device_data_;
  }
  std::size_t size() const noexcept {
    return size_;
  }
  std::uint64_t sequence() const noexcept {
    return sequence_;
  }
  std::uint64_t preceding_gap() const noexcept {
    return preceding_gap_;
  }
  explicit operator bool() const noexcept {
    return generation_ != 0;
  }

 private:
  friend class Receiver;
  ReceivedImage(void* device_data, std::size_t size, std::uint64_t sequence,
                std::uint64_t preceding_gap, std::size_t slot, std::uint64_t generation) noexcept;
  void invalidate() noexcept;

  void* device_data_{nullptr};
  std::size_t size_{0};
  std::uint64_t sequence_{0};
  std::uint64_t preceding_gap_{0};
  std::size_t slot_{0};
  std::uint64_t generation_{0};
};

enum class ReceiveStatus { image, end_of_stream, timeout, failed };

struct ReceiveResult {
  ReceiveStatus status{ReceiveStatus::timeout};
  std::optional<ReceivedImage> image;
  std::string error;
};

// One producer-owned 2-MiB slot containing up to sixteen contiguous images.
// host_pinned_mapped returns distinct CPU/UCX and CUDA aliases; cuda_device
// returns the same pointer for both. A terminal handoff consumes the lease; the
// underlying storage remains producer-owned until the matching retirement.
class BatchLease {
 public:
  BatchLease() = default;
  BatchLease(const BatchLease&) = delete;
  BatchLease& operator=(const BatchLease&) = delete;
  BatchLease(BatchLease&& other) noexcept;
  BatchLease& operator=(BatchLease&& other) = delete;

  void* ucx_data() const noexcept {
    return ucx_data_;
  }
  void* device_data() const noexcept {
    return device_data_;
  }
  std::size_t size() const noexcept {
    return size_;
  }
  std::size_t slot() const noexcept {
    return slot_;
  }
  std::uint64_t generation() const noexcept {
    return generation_;
  }
  explicit operator bool() const noexcept {
    return generation_ != 0;
  }

 private:
  friend class ExternalBatchProducer;
  BatchLease(void* ucx_data, void* device_data, std::size_t size, std::size_t slot,
             std::uint64_t generation) noexcept;
  void invalidate() noexcept;

  void* ucx_data_{nullptr};
  void* device_data_{nullptr};
  std::size_t size_{0};
  std::size_t slot_{0};
  std::uint64_t generation_{0};
};

struct RetiredBatch {
  std::size_t slot{0};
  std::uint64_t generation{0};
  std::uint64_t first_sequence{0};
  std::uint32_t image_count{0};
  std::uint32_t admitted{0};
  std::uint32_t dropped_no_credit{0};
};

struct ExternalBatchProducerStats {
  TransportStats transport;
  std::uint64_t submitted_images{0};
  std::uint64_t dropped_before_submit{0};
  std::uint64_t submitted_batches{0};
  std::uint64_t retired_batches{0};
};

// Thread ownership:
// - the internal pinned progress thread is the only thread that calls UCP and
//   polls producer-owned CUDA readiness events;
// - exactly one RX/orchestration thread calls try_acquire() and poll_retired();
// - exactly one processing thread calls submit_after();
//   that same thread calls cancel() for rejected input batches;
//   it calls release_unused() for acquired leases left over at fixed-run end;
// - a BatchLease is a move-only ownership token consumed by every terminal
//   handoff, so raw pointers cannot be submitted or released twice;
// - the producer owns one reusable CUDA event per slot and records it on the
//   caller's stream during submit_after().
class ExternalBatchProducer {
 public:
  explicit ExternalBatchProducer(ExternalBatchProducerOptions options);
  ~ExternalBatchProducer();
  ExternalBatchProducer(const ExternalBatchProducer&) = delete;
  ExternalBatchProducer& operator=(const ExternalBatchProducer&) = delete;

  void start();
  // Waits until HELLO/ACCEPT has established the fixed receiver capacity.
  // No batch slots should enter the raw pipeline before this returns.
  void wait_for_receiver();
  std::optional<BatchLease> try_acquire();
  void submit_after(BatchLease&& lease, std::uint64_t first_sequence, std::uint32_t image_count,
                    cudaStream_t processing_stream);
  void cancel(BatchLease&& lease, std::uint64_t first_sequence, std::uint32_t image_count);
  void release_unused(BatchLease&& lease);
  std::optional<RetiredBatch> poll_retired();
  void finish_input(std::uint64_t total_generated);
  ExternalBatchProducerStats stats() const;
  std::optional<std::string> error() const;
  // On a failed run, call only after all application CUDA streams that may
  // reference an acquired batch slot have been synchronized and joined.
  void acknowledge_local_quiescence();
  void close();

 private:
  struct AcquiredSlot {
    void* ucx_data{nullptr};
    void* device_data{nullptr};
    std::size_t size{0};
    std::size_t slot{0};
    std::uint64_t generation{0};
  };
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class Receiver {
 public:
  explicit Receiver(ReceiverOptions options);
  ~Receiver();
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  void start();
  ReceiveResult receive(std::chrono::milliseconds timeout);
  void release(ReceivedImage image);
  void release_after(ReceivedImage image, cudaStream_t stream);
  TransportStats stats() const;
  std::optional<std::string> error() const;
  void close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace daqiri::ucx_gpu
