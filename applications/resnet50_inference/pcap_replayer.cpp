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

#include "pcap_replayer.h"

#include <arpa/inet.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

#include <daqiri/daqiri.h>

namespace daqiri::apps::resnet {

namespace {

constexpr uint32_t kPcapMagicLE = 0xa1b2c3d4;  // microsecond, little-endian
constexpr uint32_t kPcapMagicLE_ns = 0xa1b23c4d;
constexpr int kLinkTypeEthernet = 1;
// Absolute cap even if the file's snaplen is attacker-controlled (classic pcap
// default is 65535; DAQIRI Ethernet frames are far smaller).
constexpr uint32_t kMaxPcapFrameBytes = 65535U;
// Cumulative bounds so a large CLI-selected pcap cannot exhaust host memory.
// CIFAR smoke (~256 images × 128 pkts) is ~32k frames / ~40 MiB.
constexpr size_t kMaxPcapFrames = 1u << 20;             // 1M frames
constexpr uint64_t kMaxPcapTotalBytes = 512ull << 20;  // 512 MiB of frame bodies

}  // namespace

bool PcapReplayer::load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::cerr << "PcapReplayer: cannot open " << path << "\n";
    return false;
  }

  uint8_t global[24];
  if (!f.read(reinterpret_cast<char*>(global), sizeof(global))) {
    std::cerr << "PcapReplayer: " << path << " truncated global header\n";
    return false;
  }
  uint32_t magic;
  std::memcpy(&magic, global, sizeof(magic));
  if (magic != kPcapMagicLE && magic != kPcapMagicLE_ns) {
    std::cerr << "PcapReplayer: " << path << " is not a little-endian pcap (magic mismatch)\n";
    return false;
  }
  uint32_t snaplen = 0;
  std::memcpy(&snaplen, global + 16, sizeof(snaplen));
  if (snaplen == 0) {
    std::cerr << "PcapReplayer: " << path << " snaplen is 0\n";
    return false;
  }
  const uint32_t max_incl = std::min(snaplen, kMaxPcapFrameBytes);
  uint32_t network;
  std::memcpy(&network, global + 20, sizeof(network));
  if (network != kLinkTypeEthernet) {
    std::cerr << "PcapReplayer: " << path << " linktype " << network << " is not Ethernet (1)\n";
    return false;
  }

  frames_.clear();
  uint64_t total_bytes = 0;
  uint8_t rec[16];
  while (f.read(reinterpret_cast<char*>(rec), sizeof(rec))) {
    uint32_t incl_len = 0;
    std::memcpy(&incl_len, rec + 8, sizeof(incl_len));
    if (incl_len == 0 || incl_len > max_incl) {
      std::cerr << "PcapReplayer: " << path << " record incl_len " << incl_len
                << " out of range (max " << max_incl << ")\n";
      frames_.clear();
      return false;
    }
    if (frames_.size() >= kMaxPcapFrames) {
      std::cerr << "PcapReplayer: " << path << " exceeds max frame count " << kMaxPcapFrames
                << " (loaded " << frames_.size() << " frames, " << total_bytes << " bytes)\n";
      frames_.clear();
      return false;
    }
    if (total_bytes + incl_len > kMaxPcapTotalBytes) {
      std::cerr << "PcapReplayer: " << path << " exceeds max total bytes " << kMaxPcapTotalBytes
                << " (would be " << (total_bytes + incl_len) << " after frame " << frames_.size()
                << ")\n";
      frames_.clear();
      return false;
    }
    PcapFrame frame(incl_len);
    if (!f.read(reinterpret_cast<char*>(frame.data()), incl_len)) {
      std::cerr << "PcapReplayer: " << path << " truncated frame body\n";
      break;
    }
    total_bytes += incl_len;
    frames_.push_back(std::move(frame));
  }

  std::cerr << "PcapReplayer: loaded " << frames_.size() << " frames (" << total_bytes
            << " bytes) from " << path << "\n";
  return !frames_.empty();
}

namespace {

void set_bits_be(uint8_t* data, uint32_t bit_offset, uint8_t bit_width, uint32_t value) {
  for (uint8_t i = 0; i < bit_width; ++i) {
    const uint32_t src_shift = static_cast<uint32_t>(bit_width - 1U - i);
    const uint8_t bit = static_cast<uint8_t>((value >> src_shift) & 0x1U);
    const uint32_t bit_idx = bit_offset + i;
    const uint32_t byte_idx = bit_idx / 8U;
    const uint8_t bit_pos = static_cast<uint8_t>(7U - (bit_idx % 8U));
    const uint8_t mask = static_cast<uint8_t>(1U << bit_pos);
    data[byte_idx] = static_cast<uint8_t>((data[byte_idx] & ~mask) | (bit << bit_pos));
  }
}

}  // namespace

std::vector<PcapFrame> build_synthetic_frames(const AppConfig& cfg) {
  char eth_src[6] = {0};
  char eth_dst[6] = {0};
  daqiri::format_eth_addr(eth_src, cfg.tx.eth_src_addr);
  daqiri::format_eth_addr(eth_dst, cfg.tx.eth_dst_addr);

  uint32_t ip_src = 0;
  uint32_t ip_dst = 0;
  inet_pton(AF_INET, cfg.tx.ip_src_addr.c_str(), &ip_src);
  inet_pton(AF_INET, cfg.tx.ip_dst_addr.c_str(), &ip_dst);
  ip_src = ntohl(ip_src);
  ip_dst = ntohl(ip_dst);

  const auto src_port = static_cast<uint16_t>(std::stoi(cfg.tx.udp_src_port));
  const auto dst_port = static_cast<uint16_t>(std::stoi(cfg.tx.udp_dst_port));

  // Wire: header + int8 payload (seq lives in the header at seq_bit_offset).
  const uint32_t udp_payload = cfg.out_payload_len;
  const uint32_t frame_bytes = cfg.tx.header_size + udp_payload;

  // Emit a multiple of images_per_batch so every reorder batch completes.
  const uint32_t num_images = cfg.images_per_batch > 0 ? cfg.images_per_batch : 1;
  std::vector<PcapFrame> frames;
  frames.reserve(static_cast<size_t>(num_images) * cfg.packets_per_image);

  for (uint32_t img = 0; img < num_images; ++img) {
    for (uint32_t pkt = 0; pkt < cfg.packets_per_image; ++pkt) {
      const uint32_t batch_seq = img * cfg.packets_per_image + pkt;  // in [0, packets_per_batch)
      PcapFrame frame(frame_bytes);
      daqiri::bench::populate_udp_ipv4_headers(frame.data(), cfg.tx.header_size, udp_payload,
                                               eth_src, eth_dst, ip_src, ip_dst, src_port,
                                               dst_port);
      set_bits_be(frame.data(), cfg.seq_bit_offset, cfg.seq_bit_width, batch_seq);
      for (uint32_t b = 0; b < cfg.out_payload_len; ++b) {
        frame[cfg.payload_byte_offset + b] = static_cast<uint8_t>((pkt + b) & 0xff);
      }
      daqiri::bench::finalize_udp_ipv4_checksums(frame.data());
      frames.push_back(std::move(frame));
    }
  }
  std::cerr << "build_synthetic_frames: " << frames.size() << " frames (" << num_images
            << " images x " << cfg.packets_per_image << " pkts, int8 payload "
            << cfg.out_payload_len << " B)\n";
  return frames;
}

void pcap_tx_worker(const AppConfig& cfg, const std::vector<PcapFrame>& frames,
                    const std::string& eth_dst_addr, bool loop, std::atomic<bool>& tx_done,
                    std::atomic<bool>& stop) {
  if (!daqiri::bench::set_current_thread_affinity(cfg.tx.cpu_core, "resnet_tx")) {
    stop.store(true);
    return;
  }
  if (frames.empty()) {
    std::cerr << "pcap_tx_worker: no frames to replay\n";
    stop.store(true);
    return;
  }

  const int port_id = daqiri::get_port_id(cfg.tx.interface_name);
  if (port_id < 0) {
    std::cerr << "pcap_tx_worker: invalid TX interface " << cfg.tx.interface_name << "\n";
    stop.store(true);
    return;
  }

  // Resolved destination MAC patched into every frame at send time so the pcap
  // is host-independent and the NIC accepts the frame on the RX port.
  char dst_mac[6] = {0};
  daqiri::format_eth_addr(dst_mac, eth_dst_addr);
  const size_t expected_frame_bytes = static_cast<size_t>(cfg.frame_bytes());

  std::vector<uint8_t> scratch;
  size_t cursor = 0;
  uint64_t total_sent = 0;
  uint64_t send_failures = 0;
  uint64_t fill_failures = 0;
  uint64_t no_burst_polls = 0;
  const auto tx_t0 = std::chrono::steady_clock::now();
  // Every failure path below backs off by 100 us, so a NIC or TX pool that never
  // recovers would otherwise spin here for the life of the process with no output.
  constexpr uint64_t kStallIters = 100000;  // ~10 s without a single frame sent
  uint64_t stall_iters = 0;
  uint64_t last_reported_sent = 0;

  while (!stop.load()) {
    if (stall_iters >= kStallIters) {
      std::cerr << "pcap_tx_worker: stalled after " << total_sent << "/" << frames.size()
                << " frames (send_failures=" << send_failures << " fill_failures=" << fill_failures
                << " no_burst_polls=" << no_burst_polls << "); giving up\n";
      stop.store(true);
      break;
    }
    if (total_sent != last_reported_sent) {
      last_reported_sent = total_sent;
      stall_iters = 0;
    }

    // In replay-once mode, size the final burst to the exact remainder so the
    // dataset is sent exactly once. `batch_size` (>> packets_per_image) would
    // otherwise over-send by up to a full burst, wrapping the cursor and
    // re-sending the first images -- skewing the per-class sample counts.
    uint32_t want = cfg.tx.batch_size;
    if (!loop) {
      if (total_sent >= frames.size()) break;
      want =
          static_cast<uint32_t>(std::min<uint64_t>(cfg.tx.batch_size, frames.size() - total_sent));
    }

    auto* msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id), static_cast<uint16_t>(cfg.tx.queue_id),
                       want, 1);

    if (!daqiri::is_tx_burst_available(msg)) {
      daqiri::free_tx_metadata(msg);
      ++no_burst_polls;
      ++stall_iters;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    if (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(msg);
      ++no_burst_polls;
      ++stall_iters;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    bool failed = false;
    const int num_pkts = static_cast<int>(daqiri::get_num_packets(msg));
    // Snapshot so fill/send failure can rewind: cursor advances per filled
    // packet, but an unsent burst must not skip those frames on retry.
    const size_t cursor_at_burst_start = cursor;
    for (int i = 0; i < num_pkts; ++i) {
      const size_t frame_index = cursor;
      const PcapFrame& frame = frames[frame_index];
      cursor = (cursor + 1) % frames.size();

      if (frame.size() != expected_frame_bytes) {
        if (fill_failures == 0) {
          std::cerr << "pcap_tx_worker: frame " << frame_index << " has " << frame.size()
                    << " bytes, expected " << expected_frame_bytes << " (header "
                    << cfg.tx.header_size << " + payload " << cfg.out_payload_len
                    << "); regenerate the PCAP or update the config\n";
        }
        stop.store(true);
        failed = true;
        break;
      }

      scratch.assign(frame.begin(), frame.end());
      if (scratch.size() >= 6) {
        std::memcpy(scratch.data(), dst_mac, 6);  // patch dst MAC
      }

      // cudaMemcpyDefault (not HostToDevice): the TX memory region may be
      // host_pinned (GB10 / DGX Spark) rather than device memory, and an
      // explicit HostToDevice with a host destination fails.
      auto* pkt_dst = daqiri::get_segment_packet_ptr(msg, 0, i);
      if (pkt_dst == nullptr) {
        if (fill_failures == 0) {
          std::cerr << "pcap_tx_worker: destination packet pointer is null\n";
        }
        failed = true;
        break;
      }
      const cudaError_t cerr =
          cudaMemcpy(pkt_dst, scratch.data(), scratch.size(), cudaMemcpyDefault);
      if (cerr != cudaSuccess) {
        if (fill_failures == 0) {
          std::cerr << "pcap_tx_worker: packet copy failed: " << cudaGetErrorString(cerr) << "\n";
        }
        failed = true;
        break;
      }
      if (daqiri::set_packet_lengths(msg, i, {static_cast<int>(scratch.size())}) !=
          daqiri::Status::SUCCESS) {
        failed = true;
        break;
      }
    }

    if (failed) {
      cursor = cursor_at_burst_start;
      daqiri::free_all_packets_and_burst_tx(msg);
      ++fill_failures;
      ++stall_iters;
      // Back off instead of hot-spinning on a persistent failure.
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    const daqiri::Status send_status = daqiri::send_tx_burst(msg);
    if (send_status == daqiri::Status::SUCCESS) {
      total_sent += static_cast<uint64_t>(num_pkts);
    } else {
      cursor = cursor_at_burst_start;
      if (send_status != daqiri::Status::NO_SPACE_AVAILABLE) {
        daqiri::free_all_packets_and_burst_tx(msg);
      }
      ++send_failures;
      ++stall_iters;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    // --replay-once: the whole dataset has now been sent exactly once (the burst
    // above was sized to the remainder), so stop.
    if (!loop && total_sent >= frames.size()) {
      break;
    }
  }

  const double tx_secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - tx_t0).count();
  std::cerr << "pcap_tx_worker: sent " << total_sent << "/" << frames.size() << " frames in "
            << tx_secs << " s (send_failures=" << send_failures
            << " fill_failures=" << fill_failures << " no_burst_polls=" << no_burst_polls << ")\n";

  // Signal the RX worker that no more packets are coming so it can drain the
  // ring to quiescence and stop (example / --replay-once mode).
  tx_done.store(true);
}

}  // namespace daqiri::apps::resnet
