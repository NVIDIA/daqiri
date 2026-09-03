/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

constexpr uint32_t kPacketsPerFrame = 4320;
constexpr uint32_t kCustomerHeaderBytes = 62;
constexpr uint32_t kCustomerPayloadBytes = 1440;
constexpr uint32_t kInlineFrameBytes = 64;
constexpr uint64_t kActiveTimeNs = 16000000;
constexpr uint64_t kFrameNumeratorNs = 1000000000;
constexpr uint64_t kFrameDenominator = 60;
constexpr uint32_t kWaitForPacket = 3000;
constexpr uint64_t kStartLeadNs = 100000000;
constexpr uint32_t kMarkerMagic = 0x44515754;  // "DQWT"
constexpr uint32_t kPrimeFrame = std::numeric_limits<uint32_t>::max();
constexpr size_t kMarkerOffset = 42;  // Ethernet + IPv4 + UDP headers.

struct PacketMarker {
  uint32_t magic;
  uint32_t frame;
  uint32_t packet;
};

struct Observation {
  uint32_t frame;
  uint32_t packet;
  uint64_t rx_timestamp_ns;
};

struct SharedState {
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> clock_seed_ns{0};
  std::atomic<uint64_t> clock_seed_steady_ns{0};
  // One plus the greatest zero-based packet ordinal received. The TX worker
  // uses this as the observable equivalent of waiting for packet 3000's CQE.
  std::atomic<uint64_t> received_progress{0};
  std::atomic<uint64_t> first_frame_base_ns{0};
  std::vector<Observation> observations;
};

uint64_t steady_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint64_t frame_base_ns(uint64_t first_base, uint64_t frame) {
  return first_base + (frame * kFrameNumeratorNs) / kFrameDenominator;
}

uint64_t packet_time_ns(uint64_t first_base, uint64_t frame, uint32_t packet) {
  return frame_base_ns(first_base, frame) +
         (static_cast<uint64_t>(packet) * kActiveTimeNs) / kPacketsPerFrame;
}

void publish_max(std::atomic<uint64_t>& value, uint64_t candidate) {
  uint64_t current = value.load(std::memory_order_relaxed);
  while (current < candidate &&
         !value.compare_exchange_weak(current, candidate, std::memory_order_release,
                                      std::memory_order_relaxed)) {
  }
}

bool wait_until(SharedState& shared, const std::atomic<uint64_t>& value, uint64_t target) {
  while (!shared.stop.load(std::memory_order_relaxed) &&
         value.load(std::memory_order_acquire) < target) {
    std::this_thread::yield();
  }
  return !shared.stop.load(std::memory_order_relaxed);
}

bool fill_burst(daqiri::BurstParams* burst, const daqiri::bench::RawBenchTxConfig& cfg,
                uint32_t frame, uint32_t count) {
  char eth_dst[6] = {0};
  char eth_src[6] = {0};
  daqiri::format_eth_addr(eth_src, cfg.eth_src_addr);
  daqiri::format_eth_addr(eth_dst, cfg.eth_dst_addr);

  uint32_t ip_src = 0;
  uint32_t ip_dst = 0;
  inet_pton(AF_INET, cfg.ip_src_addr.c_str(), &ip_src);
  inet_pton(AF_INET, cfg.ip_dst_addr.c_str(), &ip_dst);
  ip_src = ntohl(ip_src);
  ip_dst = ntohl(ip_dst);

  // The common helper initializes the complete logical packet, then this test
  // copies the configured host segment. The normal reproduction uses a
  // 62-byte header plus a 1,440-byte payload; the inline variant uses one
  // complete 64-byte host segment.
  std::vector<uint8_t> packet_template(cfg.header_size + cfg.payload_size, 0);
  daqiri::bench::populate_udp_ipv4_headers(packet_template.data(), cfg.header_size,
                                           cfg.payload_size, eth_src, eth_dst, ip_src, ip_dst,
                                           static_cast<uint16_t>(std::stoi(cfg.udp_src_port)),
                                           static_cast<uint16_t>(std::stoi(cfg.udp_dst_port)));
  for (uint32_t packet = 0; packet < count; ++packet) {
    auto* dst = static_cast<uint8_t*>(daqiri::get_segment_packet_ptr(burst, 0, packet));
    std::memcpy(dst, packet_template.data(), cfg.header_size);
    const PacketMarker marker{htonl(kMarkerMagic), htonl(frame), htonl(packet)};
    std::memcpy(dst + kMarkerOffset, &marker, sizeof(marker));
  }
  const daqiri::Status status =
      cfg.payload_size == 0
          ? daqiri::set_all_packet_lengths(burst, {static_cast<int>(cfg.header_size)})
          : daqiri::set_all_packet_lengths(
                burst,
                {static_cast<int>(cfg.header_size), static_cast<int>(cfg.payload_size)});
  return status == daqiri::Status::SUCCESS;
}

daqiri::BurstParams* allocate_burst(SharedState& shared, int port_id, int queue_id,
                                    uint32_t count, uint16_t num_segs) {
  while (!shared.stop.load(std::memory_order_relaxed)) {
    auto* burst = daqiri::create_tx_burst_params();
    if (burst == nullptr) {
      std::this_thread::yield();
      continue;
    }
    daqiri::set_header(burst, static_cast<uint16_t>(port_id), static_cast<uint16_t>(queue_id),
                       count, num_segs);
    if (!daqiri::is_tx_burst_available(burst)) {
      daqiri::free_tx_metadata(burst);
      std::this_thread::yield();
      continue;
    }
    if (daqiri::get_tx_packet_burst(burst) == daqiri::Status::SUCCESS) {
      return burst;
    }
    daqiri::free_tx_metadata(burst);
  }
  return nullptr;
}

bool send_prime(SharedState& shared, const daqiri::bench::RawBenchTxConfig& cfg, int port_id) {
  const uint16_t num_segs = cfg.payload_size == 0 ? 1 : 2;
  auto* burst = allocate_burst(shared, port_id, cfg.queue_id, 1, num_segs);
  if (burst == nullptr) {
    return false;
  }
  if (!fill_burst(burst, cfg, kPrimeFrame, 1)) {
    daqiri::free_all_packets_and_burst_tx(burst);
    return false;
  }
  return daqiri::send_tx_burst(burst) == daqiri::Status::SUCCESS;
}

void tx_worker(const daqiri::bench::RawBenchTxConfig& cfg, SharedState& shared) {
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, "frame_repro_tx")) {
    shared.stop.store(true);
    return;
  }
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0 || !send_prime(shared, cfg, port_id)) {
    std::cerr << "Failed to send the hardware-clock priming packet\n";
    shared.stop.store(true);
    return;
  }
  if (!wait_until(shared, shared.clock_seed_ns, 1)) {
    return;
  }

  uint64_t frame = 0;
  while (!shared.stop.load(std::memory_order_relaxed)) {
    if (frame != 0) {
      const uint64_t completion_target = (frame - 1) * kPacketsPerFrame + kWaitForPacket;
      if (!wait_until(shared, shared.received_progress, completion_target)) {
        break;
      }
    }

    const uint16_t num_segs = cfg.payload_size == 0 ? 1 : 2;
    auto* burst = allocate_burst(shared, port_id, cfg.queue_id, kPacketsPerFrame, num_segs);
    if (burst == nullptr) {
      break;
    }
    if (!fill_burst(burst, cfg, static_cast<uint32_t>(frame), kPacketsPerFrame)) {
      daqiri::free_all_packets_and_burst_tx(burst);
      shared.stop.store(true);
      break;
    }

    uint64_t first_base = shared.first_frame_base_ns.load(std::memory_order_acquire);
    if (first_base == 0) {
      const uint64_t seed = shared.clock_seed_ns.load(std::memory_order_acquire);
      const uint64_t seed_steady = shared.clock_seed_steady_ns.load(std::memory_order_acquire);
      first_base = seed + (steady_now_ns() - seed_steady) + kStartLeadNs;
      std::cout << "First scheduled frame base: " << first_base << " ns\n";
      shared.first_frame_base_ns.store(first_base, std::memory_order_release);
    }
    for (uint32_t packet = 0; packet < kPacketsPerFrame; ++packet) {
      const auto status =
          daqiri::set_packet_tx_time(burst, packet, packet_time_ns(first_base, frame, packet));
      if (status != daqiri::Status::SUCCESS) {
        std::cerr << "set_packet_tx_time failed; accurate send is unavailable (status "
                  << static_cast<int>(status) << ")\n";
        daqiri::free_all_packets_and_burst_tx(burst);
        shared.stop.store(true);
        return;
      }
    }
    if (daqiri::send_tx_burst(burst) != daqiri::Status::SUCCESS) {
      std::cerr << "send_tx_burst failed for frame " << frame << "\n";
      shared.stop.store(true);
      return;
    }
    ++frame;
  }
}

void rx_worker(const daqiri::bench::RawBenchRxConfig& cfg, SharedState& shared) {
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, "frame_repro_rx")) {
    shared.stop.store(true);
    return;
  }
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    shared.stop.store(true);
    return;
  }

  while (!shared.stop.load(std::memory_order_relaxed)) {
    daqiri::BurstParams* burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id, cfg.queue_id) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      std::this_thread::yield();
      continue;
    }
    const int count = static_cast<int>(daqiri::get_num_packets(burst));
    for (int i = 0; i < count; ++i) {
      uint64_t timestamp = 0;
      if (daqiri::get_packet_rx_timestamp(burst, i, &timestamp) != daqiri::Status::SUCCESS ||
          timestamp == 0 ||
          daqiri::get_segment_packet_length(burst, 0, i) < kMarkerOffset + sizeof(PacketMarker)) {
        continue;
      }
      PacketMarker marker{};
      std::memcpy(
          &marker,
          static_cast<const uint8_t*>(daqiri::get_segment_packet_ptr(burst, 0, i)) + kMarkerOffset,
          sizeof(marker));
      if (ntohl(marker.magic) != kMarkerMagic) {
        continue;
      }
      const uint32_t frame = ntohl(marker.frame);
      const uint32_t packet = ntohl(marker.packet);
      if (frame == kPrimeFrame) {
        std::cout << "RX clock seed: " << timestamp << " ns\n";
        shared.clock_seed_steady_ns.store(steady_now_ns(), std::memory_order_relaxed);
        shared.clock_seed_ns.store(timestamp, std::memory_order_release);
        continue;
      }
      if (packet >= kPacketsPerFrame) {
        continue;
      }
      shared.observations.push_back({frame, packet, timestamp});
      const uint64_t ordinal = static_cast<uint64_t>(frame) * kPacketsPerFrame + packet + 1;
      publish_max(shared.received_progress, ordinal);
    }
    daqiri::free_all_packets_and_burst_rx(burst);
  }
}

double percentile(std::vector<uint64_t> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const size_t index = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[index]);
}

bool write_and_report(SharedState& shared, const std::string& csv_path) {
  auto observations = std::move(shared.observations);
  std::sort(observations.begin(), observations.end(),
            [](const auto& a, const auto& b) { return a.rx_timestamp_ns < b.rx_timestamp_ns; });
  const uint64_t first_base = shared.first_frame_base_ns.load(std::memory_order_acquire);
  std::ofstream csv(csv_path);
  if (!csv) {
    std::cerr << "Cannot open timing CSV: " << csv_path << "\n";
    return false;
  }
  csv << "frame,packet,scheduled_ns,rx_timestamp_ns,rx_minus_scheduled_ns,delta_ns\n";

  std::vector<uint64_t> intra_frame_deltas;
  std::vector<uint64_t> inter_frame_deltas;
  struct Gap {
    uint64_t delta;
    Observation before;
    Observation after;
  };
  std::vector<Gap> unexpected;
  for (size_t i = 0; i < observations.size(); ++i) {
    const auto& o = observations[i];
    const uint64_t scheduled = packet_time_ns(first_base, o.frame, o.packet);
    const uint64_t delta = i == 0 ? 0 : o.rx_timestamp_ns - observations[i - 1].rx_timestamp_ns;
    const int64_t error = o.rx_timestamp_ns >= scheduled
                              ? static_cast<int64_t>(o.rx_timestamp_ns - scheduled)
                              : -static_cast<int64_t>(scheduled - o.rx_timestamp_ns);
    csv << o.frame << ',' << o.packet << ',' << scheduled << ',' << o.rx_timestamp_ns << ','
        << error << ',' << delta << '\n';
    if (i == 0) {
      continue;
    }
    const auto& prev = observations[i - 1];
    if (o.frame == prev.frame && o.packet == prev.packet + 1) {
      intra_frame_deltas.push_back(delta);
      if (delta > 50000) {
        unexpected.push_back({delta, prev, o});
      }
    } else if (o.frame == prev.frame + 1 && prev.packet == kPacketsPerFrame - 1 && o.packet == 0) {
      inter_frame_deltas.push_back(delta);
    } else {
      unexpected.push_back({delta, prev, o});
    }
  }
  csv.close();

  std::sort(unexpected.begin(), unexpected.end(),
            [](const auto& a, const auto& b) { return a.delta > b.delta; });
  const auto max_intra = intra_frame_deltas.empty() ? 0
                                                    : *std::max_element(intra_frame_deltas.begin(),
                                                                        intra_frame_deltas.end());
  std::cout << std::fixed << std::setprecision(3) << "Captured " << observations.size()
            << " packets to " << csv_path << "\n"
            << "Intra-frame RX spacing (us): p50=" << percentile(intra_frame_deltas, 0.50) / 1000.0
            << " p99=" << percentile(intra_frame_deltas, 0.99) / 1000.0
            << " max=" << max_intra / 1000.0 << "\n"
            << "Inter-frame RX gap (us): p50=" << percentile(inter_frame_deltas, 0.50) / 1000.0
            << " p99=" << percentile(inter_frame_deltas, 0.99) / 1000.0 << "\n"
            << "Unexpected/non-contiguous gaps: " << unexpected.size() << "\n";
  for (size_t i = 0; i < std::min<size_t>(unexpected.size(), 20); ++i) {
    const auto& gap = unexpected[i];
    std::cout << "  " << gap.delta / 1000.0 << " us: frame " << gap.before.frame << " packet "
              << gap.before.packet << " -> frame " << gap.after.frame << " packet "
              << gap.after.packet << "\n";
  }
  return !observations.empty();
}

std::string parse_csv_path(int argc, char** argv) {
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--timing-csv") {
      return argv[i + 1];
    }
  }
  return "daqiri_wait_frame_timing.csv";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <config.yaml> [--seconds N] [--timing-csv PATH]\n";
    return 1;
  }
  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);
  const std::string csv_path = parse_csv_path(argc, argv);
  const auto root = YAML::LoadFile(argv[1]);
  const auto tx = daqiri::bench::parse_tx(root);
  const auto rx = daqiri::bench::parse_rx(root);
  const bool customer_packet = tx.header_size == kCustomerHeaderBytes &&
                               tx.payload_size == kCustomerPayloadBytes;
  const bool inline_packet = tx.header_size == kInlineFrameBytes && tx.payload_size == 0;
  if (tx.batch_size != kPacketsPerFrame || (!customer_packet && !inline_packet)) {
    std::cerr << "bench_tx must specify batch_size=4320 and either header_size=62, "
                 "payload_size=1440 or header_size=64, payload_size=0\n";
    return 1;
  }
  if (cudaSetDevice(0) != cudaSuccess || cudaFree(nullptr) != cudaSuccess) {
    std::cerr << "Failed to initialize CUDA device 0\n";
    return 1;
  }
  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  SharedState shared;
  shared.observations.reserve(static_cast<size_t>(run_seconds) * 60 * kPacketsPerFrame);
  std::thread rx_thread(rx_worker, std::cref(rx), std::ref(shared));
  std::thread tx_thread(tx_worker, std::cref(tx), std::ref(shared));
  daqiri::bench::wait_for_stop(run_seconds, shared.stop);
  if (tx_thread.joinable()) {
    tx_thread.join();
  }
  if (rx_thread.joinable()) {
    rx_thread.join();
  }
  daqiri::print_stats();
  daqiri::shutdown();
  return write_and_report(shared, csv_path) ? 0 : 1;
}
