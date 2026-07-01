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
  uint32_t network;
  std::memcpy(&network, global + 20, sizeof(network));
  if (network != kLinkTypeEthernet) {
    std::cerr << "PcapReplayer: " << path << " linktype " << network << " is not Ethernet (1)\n";
    return false;
  }

  frames_.clear();
  uint8_t rec[16];
  while (f.read(reinterpret_cast<char*>(rec), sizeof(rec))) {
    uint32_t incl_len;
    std::memcpy(&incl_len, rec + 8, sizeof(incl_len));
    PcapFrame frame(incl_len);
    if (!f.read(reinterpret_cast<char*>(frame.data()), incl_len)) {
      std::cerr << "PcapReplayer: " << path << " truncated frame body\n";
      break;
    }
    frames_.push_back(std::move(frame));
  }

  std::cerr << "PcapReplayer: loaded " << frames_.size() << " frames from " << path << "\n";
  return !frames_.empty();
}

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

  const uint32_t udp_payload = 4u + cfg.out_payload_len;  // seq prefix + chunk
  const uint32_t frame_bytes = cfg.tx.header_size + udp_payload;

  // images_per_batch images so the TX has a full inference batch of variety.
  const uint32_t num_images = cfg.images_per_batch > 0 ? cfg.images_per_batch : 1;
  std::vector<PcapFrame> frames;
  frames.reserve(static_cast<size_t>(num_images) * cfg.packets_per_image);

  for (uint32_t img = 0; img < num_images; ++img) {
    for (uint32_t seq = 0; seq < cfg.packets_per_image; ++seq) {
      PcapFrame frame(frame_bytes);
      daqiri::bench::populate_udp_ipv4_headers(frame.data(), cfg.tx.header_size, udp_payload,
                                               eth_src, eth_dst, ip_src, ip_dst, src_port,
                                               dst_port);
      // Per-image sequence number (network byte order) at the payload start.
      const uint32_t seq_be = htonl(seq);
      std::memcpy(frame.data() + cfg.tx.header_size, &seq_be, sizeof(seq_be));
      // Arbitrary pixel bytes (content irrelevant to throughput).
      for (uint32_t b = 0; b < cfg.out_payload_len; ++b) {
        frame[cfg.tx.header_size + 4u + b] = static_cast<uint8_t>((seq + b) & 0xff);
      }
      daqiri::bench::finalize_udp_ipv4_checksums(frame.data());
      frames.push_back(std::move(frame));
    }
  }
  std::cerr << "build_synthetic_frames: " << frames.size() << " frames (" << num_images
            << " images x " << cfg.packets_per_image << " pkts)\n";
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

  std::vector<uint8_t> scratch;
  size_t cursor = 0;
  uint64_t total_sent = 0;

  while (!stop.load()) {
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
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    if (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(msg);
      continue;
    }

    bool failed = false;
    const int num_pkts = static_cast<int>(daqiri::get_num_packets(msg));
    for (int i = 0; i < num_pkts; ++i) {
      const PcapFrame& frame = frames[cursor];
      cursor = (cursor + 1) % frames.size();

      scratch.assign(frame.begin(), frame.end());
      if (scratch.size() >= 6) {
        std::memcpy(scratch.data(), dst_mac, 6);  // patch dst MAC
      }

      auto* gpu_pkt = daqiri::get_segment_packet_ptr(msg, 0, i);
      if (cudaMemcpy(gpu_pkt, scratch.data(), scratch.size(), cudaMemcpyHostToDevice) !=
          cudaSuccess) {
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
      daqiri::free_all_packets_and_burst_tx(msg);
      continue;
    }
    if (daqiri::send_tx_burst(msg) == daqiri::Status::SUCCESS) {
      total_sent += static_cast<uint64_t>(num_pkts);
    } else {
      // Caller owns the burst after get_tx_packet_burst; free it on a failed send
      // so a persistent error doesn't drain the TX mempool one slot at a time.
      daqiri::free_all_packets_and_burst_tx(msg);
      continue;
    }
    // --replay-once: the whole dataset has now been sent exactly once (the burst
    // above was sized to the remainder), so stop.
    if (!loop && total_sent >= frames.size()) {
      break;
    }
  }

  // Signal the RX worker that no more packets are coming so it can drain the
  // ring to quiescence and stop (example / --replay-once mode).
  tx_done.store(true);
}

}  // namespace daqiri::apps::resnet
