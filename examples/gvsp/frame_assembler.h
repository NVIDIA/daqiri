/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "gvsp_packet.h"

namespace daqiri::examples::gvsp {

struct FrameAssemblerConfig {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pixel_format = 0;
  size_t frame_bytes = 0;
  size_t data_payload_bytes = 0;
  std::chrono::milliseconds frame_timeout{100};
};

struct FrameView {
  uint64_t block_id = 0;
  std::optional<uint64_t> camera_timestamp_ticks;
  std::optional<uint64_t> rx_first_packet_timestamp_ns;
  std::optional<uint64_t> rx_last_packet_timestamp_ns;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pixel_format = 0;
  const std::byte* data = nullptr;
  size_t size_bytes = 0;
  uint32_t expected_data_packets = 0;
  uint32_t received_data_packets = 0;
  uint32_t duplicate_data_packets = 0;
};

enum class IncompleteReason { TIMEOUT, NEW_LEADER, SHUTDOWN };

struct IncompleteFrameReport {
  uint64_t block_id = 0;
  IncompleteReason reason = IncompleteReason::TIMEOUT;
  uint32_t expected_data_packets = 0;
  uint32_t received_data_packets = 0;
  uint32_t missing_data_packets = 0;
  uint32_t duplicate_data_packets = 0;
  size_t received_bytes = 0;
};

struct FrameAssemblerStats {
  uint64_t complete_frames = 0;
  uint64_t incomplete_frames = 0;
  uint64_t data_before_leader = 0;
  uint64_t late_or_wrong_block_packets = 0;
  uint64_t invalid_packets = 0;
  uint64_t duplicate_packets = 0;
};

class FrameAssembler {
 public:
  using Clock = std::chrono::steady_clock;
  using FrameCallback = std::function<void(const FrameView&)>;
  using IncompleteCallback = std::function<void(const IncompleteFrameReport&)>;

  FrameAssembler(FrameAssemblerConfig config, FrameCallback frame_callback,
                 IncompleteCallback incomplete_callback = {}, std::byte* external_buffer = nullptr,
                 size_t external_capacity = 0);

  bool valid(std::string* error = nullptr) const;
  void consume(const GvspPacketView& packet, Clock::time_point now,
               std::optional<uint64_t> rx_timestamp_ns = std::nullopt);
  void check_timeout(Clock::time_point now);
  void shutdown();

  const FrameAssemblerStats& stats() const {
    return stats_;
  }
  bool has_active_frame() const {
    return active_;
  }

 private:
  void start_frame(const GvspPacketView& packet, Clock::time_point now,
                   std::optional<uint64_t> rx_timestamp_ns);
  void consume_data(const GvspPacketView& packet, std::optional<uint64_t> rx_timestamp_ns);
  void consume_trailer(const GvspPacketView& packet, std::optional<uint64_t> rx_timestamp_ns);
  void maybe_complete();
  void finish_incomplete(IncompleteReason reason);
  uint32_t missing_packets() const;

  FrameAssemblerConfig config_;
  FrameCallback frame_callback_;
  IncompleteCallback incomplete_callback_;
  std::vector<std::byte> owned_frame_buffer_;
  std::byte* frame_data_ = nullptr;
  size_t frame_capacity_ = 0;
  std::vector<uint8_t> received_;
  FrameAssemblerStats stats_;

  bool active_ = false;
  bool trailer_received_ = false;
  uint64_t block_id_ = 0;
  uint32_t received_packets_ = 0;
  uint32_t duplicate_packets_ = 0;
  size_t received_bytes_ = 0;
  std::optional<uint64_t> camera_timestamp_ticks_;
  std::optional<uint64_t> rx_first_packet_timestamp_ns_;
  std::optional<uint64_t> rx_last_packet_timestamp_ns_;
  Clock::time_point deadline_{};
};

}  // namespace daqiri::examples::gvsp
