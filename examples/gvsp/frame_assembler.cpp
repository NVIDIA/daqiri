/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "frame_assembler.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace daqiri::examples::gvsp {

FrameAssembler::FrameAssembler(FrameAssemblerConfig config, FrameCallback frame_callback,
                               IncompleteCallback incomplete_callback, std::byte* external_buffer,
                               size_t external_capacity)
    : config_(std::move(config)),
      frame_callback_(std::move(frame_callback)),
      incomplete_callback_(std::move(incomplete_callback)),
      owned_frame_buffer_(external_buffer == nullptr ? config_.frame_bytes : 0),
      frame_data_(external_buffer == nullptr ? owned_frame_buffer_.data() : external_buffer),
      frame_capacity_(external_buffer == nullptr ? owned_frame_buffer_.size() : external_capacity) {
  if (config_.data_payload_bytes != 0 && config_.frame_bytes != 0) {
    const size_t count = 1 + (config_.frame_bytes - 1) / config_.data_payload_bytes;
    if (count <= std::numeric_limits<uint32_t>::max()) {
      received_.resize(count);
    }
  }
}

bool FrameAssembler::valid(std::string* error) const {
  auto fail = [&](const char* message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (config_.width == 0 || config_.height == 0) {
    return fail("frame width and height must be nonzero");
  }
  if (config_.pixel_format == 0) {
    return fail("pixel format code must be nonzero");
  }
  if (config_.frame_bytes == 0 || config_.data_payload_bytes == 0) {
    return fail("frame_bytes and data_payload_bytes must be nonzero");
  }
  if (received_.empty()) {
    return fail("expected data-packet count is out of range");
  }
  if (frame_data_ == nullptr || frame_capacity_ < config_.frame_bytes) {
    return fail("frame buffer is null or smaller than frame_bytes");
  }
  if (config_.frame_timeout.count() <= 0) {
    return fail("frame timeout must be positive");
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void FrameAssembler::consume(const GvspPacketView& packet, Clock::time_point now,
                             std::optional<uint64_t> rx_timestamp_ns) {
  check_timeout(now);
  switch (packet.kind) {
    case PacketKind::LEADER:
      if (packet.packet_id != 0 || !packet.leader.has_value()) {
        ++stats_.invalid_packets;
        return;
      }
      if (active_ && packet.block_id == block_id_) {
        ++stats_.duplicate_packets;
        return;
      }
      if (active_) {
        finish_incomplete(IncompleteReason::NEW_LEADER);
      }
      start_frame(packet, now, rx_timestamp_ns);
      break;
    case PacketKind::DATA:
      if (!active_) {
        ++stats_.data_before_leader;
        return;
      }
      if (packet.block_id != block_id_) {
        ++stats_.late_or_wrong_block_packets;
        return;
      }
      consume_data(packet, rx_timestamp_ns);
      maybe_complete();
      break;
    case PacketKind::TRAILER:
      if (!active_ || packet.block_id != block_id_) {
        ++stats_.late_or_wrong_block_packets;
        return;
      }
      consume_trailer(packet, rx_timestamp_ns);
      maybe_complete();
      break;
    case PacketKind::UNSUPPORTED:
      ++stats_.invalid_packets;
      break;
  }
}

void FrameAssembler::check_timeout(Clock::time_point now) {
  if (active_ && now >= deadline_) {
    finish_incomplete(IncompleteReason::TIMEOUT);
  }
}

void FrameAssembler::shutdown() {
  if (active_) {
    finish_incomplete(IncompleteReason::SHUTDOWN);
  }
}

void FrameAssembler::start_frame(const GvspPacketView& packet, Clock::time_point now,
                                 std::optional<uint64_t> rx_timestamp_ns) {
  const ImageLeader& leader = *packet.leader;
  if (leader.width != config_.width || leader.height != config_.height ||
      leader.pixel_format != config_.pixel_format) {
    ++stats_.invalid_packets;
    return;
  }

  std::fill(received_.begin(), received_.end(), 0);
  active_ = true;
  trailer_received_ = false;
  block_id_ = packet.block_id;
  received_packets_ = 0;
  duplicate_packets_ = 0;
  received_bytes_ = 0;
  camera_timestamp_ticks_ = leader.timestamp_ticks;
  rx_first_packet_timestamp_ns_ = rx_timestamp_ns;
  rx_last_packet_timestamp_ns_ = rx_timestamp_ns;
  deadline_ = now + config_.frame_timeout;
}

void FrameAssembler::consume_data(const GvspPacketView& packet,
                                  std::optional<uint64_t> rx_timestamp_ns) {
  if (packet.packet_id == 0) {
    ++stats_.invalid_packets;
    return;
  }
  const uint64_t slot64 = static_cast<uint64_t>(packet.packet_id) - 1;
  if (slot64 >= received_.size()) {
    ++stats_.invalid_packets;
    return;
  }
  const size_t slot = static_cast<size_t>(slot64);
  if (received_[slot] != 0) {
    ++duplicate_packets_;
    ++stats_.duplicate_packets;
    return;
  }

  const size_t offset = slot * config_.data_payload_bytes;
  const size_t expected_size = std::min(config_.data_payload_bytes, config_.frame_bytes - offset);
  if (packet.data == nullptr || packet.data_size != expected_size) {
    ++stats_.invalid_packets;
    return;
  }
  std::memcpy(frame_data_ + offset, packet.data, packet.data_size);
  received_[slot] = 1;
  ++received_packets_;
  received_bytes_ += packet.data_size;
  if (rx_timestamp_ns.has_value()) {
    if (!rx_first_packet_timestamp_ns_.has_value()) {
      rx_first_packet_timestamp_ns_ = rx_timestamp_ns;
    }
    rx_last_packet_timestamp_ns_ = rx_timestamp_ns;
  }
}

void FrameAssembler::consume_trailer(const GvspPacketView& packet,
                                     std::optional<uint64_t> rx_timestamp_ns) {
  const uint64_t expected_trailer_id = static_cast<uint64_t>(received_.size()) + 1;
  if (packet.packet_id != expected_trailer_id) {
    ++stats_.invalid_packets;
    return;
  }
  trailer_received_ = true;
  if (rx_timestamp_ns.has_value()) {
    rx_last_packet_timestamp_ns_ = rx_timestamp_ns;
  }
}

void FrameAssembler::maybe_complete() {
  if (!active_ || !trailer_received_ || received_packets_ != received_.size() ||
      received_bytes_ != config_.frame_bytes) {
    return;
  }

  FrameView frame;
  frame.block_id = block_id_;
  frame.camera_timestamp_ticks = camera_timestamp_ticks_;
  frame.rx_first_packet_timestamp_ns = rx_first_packet_timestamp_ns_;
  frame.rx_last_packet_timestamp_ns = rx_last_packet_timestamp_ns_;
  frame.width = config_.width;
  frame.height = config_.height;
  frame.pixel_format = config_.pixel_format;
  frame.data = frame_data_;
  frame.size_bytes = config_.frame_bytes;
  frame.expected_data_packets = static_cast<uint32_t>(received_.size());
  frame.received_data_packets = received_packets_;
  frame.duplicate_data_packets = duplicate_packets_;
  if (frame_callback_) {
    frame_callback_(frame);
  }

  ++stats_.complete_frames;
  active_ = false;
}

void FrameAssembler::finish_incomplete(IncompleteReason reason) {
  ++stats_.incomplete_frames;
  if (incomplete_callback_) {
    IncompleteFrameReport report;
    report.block_id = block_id_;
    report.reason = reason;
    report.expected_data_packets = static_cast<uint32_t>(received_.size());
    report.received_data_packets = received_packets_;
    report.missing_data_packets = missing_packets();
    report.duplicate_data_packets = duplicate_packets_;
    report.received_bytes = received_bytes_;
    incomplete_callback_(report);
  }
  active_ = false;
}

uint32_t FrameAssembler::missing_packets() const {
  return static_cast<uint32_t>(
      std::count(received_.begin(), received_.end(), static_cast<uint8_t>(0)));
}

}  // namespace daqiri::examples::gvsp
