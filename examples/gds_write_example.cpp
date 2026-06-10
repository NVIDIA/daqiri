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

#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

struct CliConfig {
  std::string config_path;
  std::string output_dir;
  std::string file_prefix;
  std::string mode = "both";
  std::string format = "raw";
  uint64_t offset = 60;
  uint64_t timeout_ms = 5000;
};

void print_usage(const char *program) {
  std::cerr << "Usage: " << program
            << " <config.yaml> <output_dir> <prefix> "
               "[--mode sync|async|both] [--format raw|pcap] "
               "[--offset N] [--timeout-ms N]\n";
}

bool parse_args(int argc, char **argv, CliConfig &cfg) {
  if (argc < 4) {
    return false;
  }

  cfg.config_path = argv[1];
  cfg.output_dir = argv[2];
  cfg.file_prefix = argv[3];

  for (int i = 4; i < argc; i += 2) {
    if (i + 1 >= argc) {
      return false;
    }
    const std::string key = argv[i];
    if (key == "--offset") {
      cfg.offset = std::stoull(argv[i + 1]);
    } else if (key == "--timeout-ms") {
      cfg.timeout_ms = std::stoull(argv[i + 1]);
    } else if (key == "--mode") {
      cfg.mode = argv[i + 1];
    } else if (key == "--format") {
      cfg.format = argv[i + 1];
    } else {
      return false;
    }
  }

  return (cfg.mode == "sync" || cfg.mode == "async" || cfg.mode == "both") &&
         (cfg.format == "raw" || cfg.format == "pcap");
}

bool copy_packet_to_burst_buffer(void *dst, const uint8_t *src, size_t size) {
  cudaPointerAttributes attrs{};
  const auto attr_status = cudaPointerGetAttributes(&attrs, dst);
  if (attr_status != cudaSuccess) {
    (void)cudaGetLastError();
    std::memcpy(dst, src, size);
    return true;
  }

#if CUDART_VERSION >= 10000
  if (attrs.type == cudaMemoryTypeDevice ||
      attrs.type == cudaMemoryTypeManaged) {
#else
  if (attrs.memoryType == cudaMemoryTypeDevice) {
#endif
    return cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice) == cudaSuccess;
  }

  std::memcpy(dst, src, size);
  return true;
}

bool fill_and_send_one_burst(const daqiri::bench::RawBenchTxConfig &cfg,
                             uint64_t offset) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid TX interface_name: " << cfg.interface_name << "\n";
    return false;
  }

  char eth_dst[6] = {};
  char eth_src[6] = {};
  daqiri::format_eth_addr(eth_src, cfg.eth_src_addr);
  daqiri::format_eth_addr(eth_dst, cfg.eth_dst_addr);

  uint32_t ip_src = 0;
  uint32_t ip_dst = 0;
  inet_pton(AF_INET, cfg.ip_src_addr.c_str(), &ip_src);
  inet_pton(AF_INET, cfg.ip_dst_addr.c_str(), &ip_dst);
  ip_src = ntohl(ip_src);
  ip_dst = ntohl(ip_dst);

  const auto src_ports = daqiri::bench::parse_udp_ports(cfg.udp_src_port);
  const auto dst_ports = daqiri::bench::parse_udp_ports(cfg.udp_dst_port);
  const auto packet_size =
      static_cast<size_t>(cfg.header_size) + cfg.payload_size;

  auto *msg = daqiri::create_tx_burst_params();
  daqiri::set_header(msg, static_cast<uint16_t>(port_id),
                     static_cast<uint16_t>(cfg.queue_id), cfg.batch_size, 1);

  if (!daqiri::is_tx_burst_available(msg)) {
    std::cerr << "No TX burst is available\n";
    daqiri::free_tx_metadata(msg);
    return false;
  }

  if (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS) {
    std::cerr << "Failed to allocate TX packet burst\n";
    daqiri::free_tx_metadata(msg);
    return false;
  }

  bool failed = false;
  for (int pkt = 0; pkt < static_cast<int>(daqiri::get_num_packets(msg));
       ++pkt) {
    std::vector<uint8_t> packet(packet_size, 0);
    daqiri::bench::populate_udp_ipv4_headers(
        packet.data(), cfg.header_size, cfg.payload_size, eth_src, eth_dst,
        ip_src, ip_dst, src_ports[pkt % src_ports.size()],
        dst_ports[pkt % dst_ports.size()]);

    for (size_t pos =
             static_cast<size_t>(std::min<uint64_t>(offset, packet.size()));
         pos < packet.size(); ++pos) {
      packet[pos] = static_cast<uint8_t>(
          (static_cast<uint64_t>(pkt) + pos - offset) & 0xffU);
    }
    daqiri::bench::finalize_udp_ipv4_checksums(packet.data());

    void *pkt_buf = daqiri::get_segment_packet_ptr(msg, 0, pkt);
    if (pkt_buf == nullptr ||
        !copy_packet_to_burst_buffer(pkt_buf, packet.data(), packet.size()) ||
        daqiri::set_packet_lengths(msg, pkt,
                                   {static_cast<int>(packet.size())}) !=
            daqiri::Status::SUCCESS) {
      failed = true;
      break;
    }
  }

  if (failed) {
    std::cerr << "Failed to populate TX burst\n";
    daqiri::free_all_packets_and_burst_tx(msg);
    return false;
  }

  if (daqiri::send_tx_burst(msg) != daqiri::Status::SUCCESS) {
    std::cerr << "Failed to send TX burst\n";
    daqiri::free_all_packets_and_burst_tx(msg);
    return false;
  }

  return true;
}

daqiri::BurstParams *
wait_for_rx_burst(const daqiri::bench::RawBenchRxConfig &cfg,
                  uint64_t timeout_ms) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << cfg.interface_name << "\n";
    return nullptr;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto num_rx_queues =
        static_cast<int>(daqiri::get_num_rx_queues(port_id));
    for (int q = 0; q < num_rx_queues; ++q) {
      daqiri::BurstParams *burst = nullptr;
      if (daqiri::get_rx_burst(&burst, port_id, q) == daqiri::Status::SUCCESS &&
          burst != nullptr) {
        return burst;
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  return nullptr;
}

daqiri::Status write_burst_sync(daqiri::BurstParams *burst,
                                const CliConfig &cli,
                                const std::string &prefix) {
  if (cli.format == "pcap") {
    return daqiri::daqiri_write_pcap_to_file(burst, cli.output_dir, prefix);
  }
  return daqiri::daqiri_write_raw_to_file(burst, cli.output_dir, prefix,
                                          cli.offset);
}

daqiri::Status write_burst_async(daqiri::BurstParams *burst,
                                 const CliConfig &cli,
                                 const std::string &prefix) {
  daqiri::FileWriteHandle *handle = nullptr;
  auto status = cli.format == "pcap"
                    ? daqiri::daqiri_write_pcap_to_file_async(
                          burst, cli.output_dir, prefix, &handle)
                    : daqiri::daqiri_write_raw_to_file_async(
                          burst, cli.output_dir, prefix, cli.offset, &handle);
  if (status != daqiri::Status::SUCCESS) {
    return status;
  }

  daqiri::FileWriteStatus write_status{};
  while (true) {
    status = daqiri::daqiri_file_write_poll(handle, &write_status);
    if (status != daqiri::Status::NOT_READY) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto destroy_status = daqiri::daqiri_file_write_destroy(handle);
  if (status != daqiri::Status::SUCCESS) {
    return status;
  }
  return destroy_status;
}

bool run_one_capture(const daqiri::bench::RawBenchTxConfig &tx_cfg,
                     const daqiri::bench::RawBenchRxConfig &rx_cfg,
                     const CliConfig &cli, const std::string &prefix,
                     bool async_write) {
  if (!daqiri::bench::set_current_thread_affinity(tx_cfg.cpu_core,
                                                  "bench_tx")) {
    return false;
  }
  if (!fill_and_send_one_burst(tx_cfg, cli.offset)) {
    return false;
  }

  if (!daqiri::bench::set_current_thread_affinity(rx_cfg.cpu_core,
                                                  "bench_rx")) {
    return false;
  }
  daqiri::BurstParams *rx_burst = wait_for_rx_burst(rx_cfg, cli.timeout_ms);
  if (rx_burst == nullptr) {
    std::cerr << "Timed out waiting for RX burst\n";
    return false;
  }

  const auto write_status = async_write
                                ? write_burst_async(rx_burst, cli, prefix)
                                : write_burst_sync(rx_burst, cli, prefix);
  daqiri::free_all_packets_and_burst_rx(rx_burst);

  if (write_status != daqiri::Status::SUCCESS) {
    std::cerr << (async_write ? "async" : "sync")
              << " burst file write failed with status "
              << static_cast<int>(write_status) << "\n";
    return false;
  }

  std::cout << "Wrote " << (async_write ? "async" : "sync") << " " << cli.format
            << " output with prefix " << prefix << "\n";
  return true;
}

} // namespace

int main(int argc, char **argv) {
  CliConfig cli;
  if (!parse_args(argc, argv, cli)) {
    print_usage(argv[0]);
    return 1;
  }

  const auto root = YAML::LoadFile(cli.config_path);
  if (!daqiri::bench::has_bench_tx(root) ||
      !daqiri::bench::has_bench_rx(root)) {
    std::cerr << "Config must define bench_tx and bench_rx\n";
    return 1;
  }

  if (daqiri::daqiri_init(cli.config_path) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  const auto tx_cfg = daqiri::bench::parse_tx(root);
  const auto rx_cfg = daqiri::bench::parse_rx(root);

  bool ok = true;
  if (cli.mode == "sync" || cli.mode == "both") {
    const std::string prefix =
        cli.mode == "both" ? cli.file_prefix + "_sync" : cli.file_prefix;
    ok = run_one_capture(tx_cfg, rx_cfg, cli, prefix, false) && ok;
  }
  if (ok && (cli.mode == "async" || cli.mode == "both")) {
    const std::string prefix =
        cli.mode == "both" ? cli.file_prefix + "_async" : cli.file_prefix;
    ok = run_one_capture(tx_cfg, rx_cfg, cli, prefix, true) && ok;
  }

  if (!ok) {
    daqiri::shutdown();
    return 1;
  }

  std::cout << "GDS write example complete: " << tx_cfg.batch_size
            << " packets per write in " << cli.output_dir << "\n";
  daqiri::shutdown();
  return 0;
}
