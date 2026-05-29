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

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr size_t kEthernetHeaderLen = 14;
constexpr size_t kIpv4HeaderLen = 20;
constexpr size_t kUdpHeaderLen = 8;
constexpr size_t kUdpIpv4EthernetHeaderLen =
    kEthernetHeaderLen + kIpv4HeaderLen + kUdpHeaderLen;
constexpr uint32_t kEtherTypeIpv4 = 0x0800;
constexpr uint8_t kIpProtocolUdp = 17;
constexpr uint32_t kPcapMagicUsec = 0xa1b2c3d4;
constexpr uint32_t kPcapLinkTypeEthernet = 1;
constexpr uint32_t kPcapSnaplen = 262144;

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int) { g_stop_requested = 1; }

void store_be16(uint8_t *dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value >> 8);
  dst[1] = static_cast<uint8_t>(value & 0xff);
}

uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t len) {
  while (len > 1) {
    sum += static_cast<uint16_t>((data[0] << 8) | data[1]);
    data += 2;
    len -= 2;
  }
  if (len == 1) {
    sum += static_cast<uint16_t>(data[0] << 8);
  }
  return sum;
}

uint16_t checksum_finish(uint32_t sum) {
  while ((sum >> 16) != 0) {
    sum = (sum & 0xffff) + (sum >> 16);
  }
  return static_cast<uint16_t>(~sum);
}

uint16_t ipv4_checksum(const uint8_t *ip_header) {
  return checksum_finish(checksum_add(0, ip_header, kIpv4HeaderLen));
}

uint16_t udp_checksum(const std::vector<uint8_t> &packet) {
  const uint8_t *ip = packet.data() + kEthernetHeaderLen;
  const uint8_t *udp = ip + kIpv4HeaderLen;
  const uint16_t udp_len = static_cast<uint16_t>(
      packet.size() - kEthernetHeaderLen - kIpv4HeaderLen);

  uint32_t sum = 0;
  sum = checksum_add(sum, ip + 12, 8);
  sum += kIpProtocolUdp;
  sum += udp_len;
  sum = checksum_add(sum, udp, udp_len);
  uint16_t result = checksum_finish(sum);
  return result == 0 ? 0xffff : result;
}

bool parse_ipv4(const std::string &text, std::array<uint8_t, 4> *out) {
  in_addr addr{};
  if (inet_pton(AF_INET, text.c_str(), &addr) != 1) {
    return false;
  }
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&addr.s_addr);
  (*out)[0] = bytes[0];
  (*out)[1] = bytes[1];
  (*out)[2] = bytes[2];
  (*out)[3] = bytes[3];
  return true;
}

std::vector<uint8_t>
make_udp_packet_template(const daqiri::bench::RawBenchTxConfig &cfg,
                         const std::string &eth_src_addr) {
  if (cfg.header_size < kUdpIpv4EthernetHeaderLen) {
    throw std::runtime_error("bench_tx.header_size must be at least 42 bytes");
  }

  const uint32_t packet_len = cfg.header_size + cfg.payload_size;
  if (packet_len > kPcapSnaplen) {
    throw std::runtime_error("packet length exceeds pcap snaplen");
  }
  if (packet_len - kEthernetHeaderLen > std::numeric_limits<uint16_t>::max()) {
    throw std::runtime_error("packet is too large for IPv4");
  }

  std::array<uint8_t, 4> src_ip{};
  std::array<uint8_t, 4> dst_ip{};
  if (!parse_ipv4(cfg.ip_src_addr, &src_ip)) {
    throw std::runtime_error("invalid bench_tx.ip_src_addr");
  }
  if (!parse_ipv4(cfg.ip_dst_addr, &dst_ip)) {
    throw std::runtime_error("invalid bench_tx.ip_dst_addr");
  }
  const auto src_ports = daqiri::bench::parse_udp_ports(cfg.udp_src_port);
  const auto dst_ports = daqiri::bench::parse_udp_ports(cfg.udp_dst_port);

  char dst_mac[6] = {0};
  char src_mac[6] = {0};
  daqiri::format_eth_addr(dst_mac, cfg.eth_dst_addr);
  daqiri::format_eth_addr(src_mac, eth_src_addr);

  std::vector<uint8_t> packet(packet_len, 0);
  std::memcpy(packet.data(), dst_mac, sizeof(dst_mac));
  std::memcpy(packet.data() + 6, src_mac, sizeof(src_mac));
  store_be16(packet.data() + 12, kEtherTypeIpv4);

  uint8_t *ip = packet.data() + kEthernetHeaderLen;
  ip[0] = 0x45;
  ip[1] = 0;
  store_be16(ip + 2, static_cast<uint16_t>(packet_len - kEthernetHeaderLen));
  store_be16(ip + 4, 0);
  store_be16(ip + 6, 0x4000);
  ip[8] = 64;
  ip[9] = kIpProtocolUdp;
  std::memcpy(ip + 12, src_ip.data(), src_ip.size());
  std::memcpy(ip + 16, dst_ip.data(), dst_ip.size());

  uint8_t *udp = ip + kIpv4HeaderLen;
  store_be16(udp, src_ports.front());
  store_be16(udp + 2, dst_ports.front());
  store_be16(udp + 4, static_cast<uint16_t>(packet_len - kEthernetHeaderLen -
                                            kIpv4HeaderLen));

  for (uint32_t i = kUdpIpv4EthernetHeaderLen; i < packet_len; ++i) {
    packet[i] = static_cast<uint8_t>((i - kUdpIpv4EthernetHeaderLen) & 0xff);
  }

  store_be16(ip + 10, ipv4_checksum(ip));
  store_be16(udp + 6, udp_checksum(packet));
  return packet;
}

bool is_device_pointer(const void *ptr) {
  cudaPointerAttributes attrs{};
  cudaError_t err = cudaPointerGetAttributes(&attrs, ptr);
  if (err != cudaSuccess) {
    cudaGetLastError();
    return false;
  }
#if CUDART_VERSION >= 10000
  return attrs.type == cudaMemoryTypeDevice ||
         attrs.type == cudaMemoryTypeManaged;
#else
  return attrs.memoryType == cudaMemoryTypeDevice;
#endif
}

bool write_all(int fd, const void *data, size_t len) {
  const uint8_t *cursor = static_cast<const uint8_t *>(data);
  while (len > 0) {
    ssize_t written = ::write(fd, cursor, len);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    cursor += written;
    len -= static_cast<size_t>(written);
  }
  return true;
}

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

class PcapWriter {
public:
  explicit PcapWriter(std::string output_path)
      : output_path_(std::move(output_path)) {}

  ~PcapWriter() {
    if (copy_stream_ != nullptr) {
      cudaStreamDestroy(copy_stream_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  void open_file() {
    fd_ = ::open(output_path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC,
                 0644);
    if (fd_ < 0) {
      throw std::runtime_error("failed to open " + output_path_ + ": " +
                               std::strerror(errno));
    }

    PcapGlobalHeader header{
        kPcapMagicUsec, 2, 4, 0, 0, kPcapSnaplen, kPcapLinkTypeEthernet,
    };
    if (!write_all(fd_, &header, sizeof(header))) {
      throw std::runtime_error("failed to write pcap header: " +
                               std::string(std::strerror(errno)));
    }

    cudaError_t err =
        cudaStreamCreateWithFlags(&copy_stream_, cudaStreamNonBlocking);
    if (err != cudaSuccess) {
      throw std::runtime_error("failed to create CUDA copy stream: " +
                               std::string(cudaGetErrorString(err)));
    }
  }

  bool write_burst(daqiri::BurstParams *burst) {
    if (burst == nullptr) {
      std::cerr << "received null burst\n";
      return false;
    }

    for (int packet = 0; packet < daqiri::get_num_packets(burst); ++packet) {
      if (!write_packet(burst, packet)) {
        return false;
      }
    }
    ++bursts_;
    return true;
  }

  uint64_t packets() const { return packets_; }
  uint64_t bytes() const { return bytes_; }
  uint64_t bursts() const { return bursts_; }
  const std::string &output_path() const { return output_path_; }

private:
  uint32_t packet_length(daqiri::BurstParams *burst, int packet) const {
    uint64_t total = 0;
    for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
      total += daqiri::get_segment_packet_length(burst, seg, packet);
    }
    if (total > kPcapSnaplen) {
      throw std::runtime_error("received packet exceeds pcap snaplen");
    }
    return static_cast<uint32_t>(total);
  }

  bool write_segment(const void *data, size_t len) {
    if (len == 0) {
      return true;
    }
    if (!is_device_pointer(data)) {
      return write_all(fd_, data, len);
    }

    if (staging_.capacity() < len) {
      if (!staging_.resize(len)) {
        std::cerr << "failed to allocate pinned host staging buffer\n";
        return false;
      }
    }
    cudaError_t err = cudaMemcpyAsync(staging_.data(), data, len,
                                      cudaMemcpyDeviceToHost, copy_stream_);
    if (err != cudaSuccess) {
      std::cerr << "cudaMemcpyAsync failed while staging packet data: "
                << cudaGetErrorString(err) << "\n";
      return false;
    }
    err = cudaStreamSynchronize(copy_stream_);
    if (err != cudaSuccess) {
      std::cerr << "cudaStreamSynchronize failed while staging packet data: "
                << cudaGetErrorString(err) << "\n";
      return false;
    }
    return write_all(fd_, staging_.data(), len);
  }

  bool write_packet(daqiri::BurstParams *burst, int packet) {
    uint32_t len = 0;
    try {
      len = packet_length(burst, packet);
    } catch (const std::exception &e) {
      std::cerr << e.what() << "\n";
      return false;
    }

    auto now = std::chrono::system_clock::now();
    auto epoch_usec = std::chrono::duration_cast<std::chrono::microseconds>(
                          now.time_since_epoch())
                          .count();

    PcapPacketHeader packet_header{
        static_cast<uint32_t>(epoch_usec / 1000000),
        static_cast<uint32_t>(epoch_usec % 1000000),
        len,
        len,
    };
    if (!write_all(fd_, &packet_header, sizeof(packet_header))) {
      std::cerr << "failed to write pcap packet header: "
                << std::strerror(errno) << "\n";
      return false;
    }

    for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
      auto *data = daqiri::get_segment_packet_ptr(burst, seg, packet);
      auto seg_len = daqiri::get_segment_packet_length(burst, seg, packet);
      if (!write_segment(data, seg_len)) {
        std::cerr << "failed to write packet data: " << std::strerror(errno)
                  << "\n";
        return false;
      }
    }

    ++packets_;
    bytes_ += len;
    return true;
  }

  std::string output_path_;
  int fd_ = -1;
  cudaStream_t copy_stream_ = nullptr;
  daqiri::bench::PinnedHostBuffer staging_;
  uint64_t bursts_ = 0;
  uint64_t packets_ = 0;
  uint64_t bytes_ = 0;
};

struct CliArgs {
  std::string config_path;
  std::string output_path;
  bool start_tx = false;
};

void print_usage(const char *argv0) {
  std::cerr << "Usage: " << argv0 << " <config.yaml> <output.pcap> [--tx]\n"
            << "  --tx  start the built-in demo transmitter from bench_tx\n";
}

bool parse_args(int argc, char **argv, CliArgs *args) {
  if (argc < 3) {
    return false;
  }
  args->config_path = argv[1];
  args->output_path = argv[2];
  for (int i = 3; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--tx") {
      args->start_tx = true;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return true;
}

struct PcapTxConfig {
  daqiri::bench::RawBenchTxConfig raw;
  std::string eth_src_addr = "02:00:00:00:00:01";
};

PcapTxConfig parse_pcap_tx_config(const YAML::Node &root) {
  PcapTxConfig cfg;
  cfg.raw = daqiri::bench::parse_tx(root);
  const auto bench_tx = root["bench_tx"];
  const auto tx = bench_tx && bench_tx.IsSequence() && bench_tx.size() > 0
                      ? bench_tx[0]
                      : bench_tx;
  if (tx && tx["eth_src_addr"]) {
    cfg.eth_src_addr = tx["eth_src_addr"].as<std::string>();
  }
  return cfg;
}

void tx_worker(PcapTxConfig cfg, std::atomic<bool> *stop) {
  const int port_id = daqiri::get_port_id(cfg.raw.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid TX interface_name: " << cfg.raw.interface_name
              << "\n";
    stop->store(true, std::memory_order_relaxed);
    return;
  }

  auto packet_template = make_udp_packet_template(cfg.raw, cfg.eth_src_addr);
  std::unordered_set<void *> initialized_buffers;
  uint64_t bursts = 0;
  uint64_t packets = 0;

  while (!stop->load(std::memory_order_relaxed)) {
    auto *msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id),
                       static_cast<uint16_t>(cfg.raw.queue_id),
                       cfg.raw.batch_size, 1);

    if (!daqiri::is_tx_burst_available(msg)) {
      daqiri::free_tx_metadata(msg);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    if (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(msg);
      continue;
    }

    const int num_packets = daqiri::get_num_packets(msg);
    bool failed = false;

    for (int i = 0; i < num_packets; ++i) {
      auto *dst = daqiri::get_segment_packet_ptr(msg, 0, i);
      if (initialized_buffers.insert(dst).second) {
        cudaError_t err =
            cudaMemcpy(dst, packet_template.data(), packet_template.size(),
                       cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
          std::cerr << "TX cudaMemcpy failed: " << cudaGetErrorString(err)
                    << "\n";
          failed = true;
          break;
        }
      }

      if (daqiri::set_packet_lengths(
              msg, i,
              {static_cast<int>(cfg.raw.header_size + cfg.raw.payload_size)}) !=
          daqiri::Status::SUCCESS) {
        failed = true;
        break;
      }
    }

    if (failed) {
      daqiri::free_all_packets_and_burst_tx(msg);
      stop->store(true, std::memory_order_relaxed);
      break;
    }

    if (daqiri::send_tx_burst(msg) != daqiri::Status::SUCCESS) {
      std::cerr << "send_tx_burst failed\n";
      stop->store(true, std::memory_order_relaxed);
      break;
    }

    ++bursts;
    packets += static_cast<uint64_t>(num_packets);
  }

  std::cout << "TX stopped after " << bursts << " bursts and " << packets
            << " packets\n";
}

void rx_pcap_loop(const daqiri::bench::RawBenchRxConfig &rx_cfg,
                  PcapWriter *writer, std::atomic<bool> *stop) {
  const int port_id = daqiri::get_port_id(rx_cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << rx_cfg.interface_name << "\n";
    stop->store(true, std::memory_order_relaxed);
    return;
  }

  auto last_report = std::chrono::steady_clock::now();
  while (!stop->load(std::memory_order_relaxed)) {
    if (g_stop_requested != 0) {
      stop->store(true, std::memory_order_relaxed);
      break;
    }

    bool got_work = false;
    int num_queues = daqiri::get_num_rx_queues(port_id);
    for (int queue_id = 0; queue_id < num_queues; ++queue_id) {
      daqiri::BurstParams *burst = nullptr;
      if (daqiri::get_rx_burst(&burst, port_id, queue_id) !=
              daqiri::Status::SUCCESS ||
          burst == nullptr) {
        continue;
      }
      got_work = true;
      if (!writer->write_burst(burst)) {
        stop->store(true, std::memory_order_relaxed);
      }
      daqiri::free_all_packets_and_burst_rx(burst);
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(2)) {
      std::cout << "captured " << writer->packets() << " packets, "
                << writer->bytes() << " bytes into " << writer->output_path()
                << "\n";
      last_report = now;
    }
    if (!got_work) {
      std::this_thread::yield();
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  CliArgs args;
  if (!parse_args(argc, argv, &args)) {
    print_usage(argv[0]);
    return 1;
  }
  bool daqiri_initialized = false;

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  try {
    YAML::Node root = YAML::LoadFile(args.config_path);
    if (!daqiri::bench::has_bench_rx(root)) {
      std::cerr << "config must contain bench_rx\n";
      return 1;
    }
    if (args.start_tx && !daqiri::bench::has_bench_tx(root)) {
      std::cerr << "--tx requires bench_tx in the config\n";
      return 1;
    }
    if (!args.start_tx && daqiri::bench::has_bench_tx(root)) {
      std::cout << "bench_tx is present but disabled; pass --tx to start the "
                   "demo transmitter\n";
    }

    auto rx_cfg = daqiri::bench::parse_rx(root);
    std::unique_ptr<PcapTxConfig> tx_cfg;
    if (args.start_tx) {
      tx_cfg = std::make_unique<PcapTxConfig>(parse_pcap_tx_config(root));
    }

    if (daqiri::daqiri_init(args.config_path) != daqiri::Status::SUCCESS) {
      std::cerr << "daqiri_init failed\n";
      return 1;
    }
    daqiri_initialized = true;

    PcapWriter writer(args.output_path);
    writer.open_file();

    std::atomic<bool> stop{false};
    std::thread tx_thread;
    if (tx_cfg) {
      tx_thread = std::thread(tx_worker, *tx_cfg, &stop);
    }

    std::cout << "Capturing to " << args.output_path
              << ". Press Ctrl+C to stop.\n";
    rx_pcap_loop(rx_cfg, &writer, &stop);

    stop.store(true, std::memory_order_relaxed);
    if (tx_thread.joinable()) {
      tx_thread.join();
    }

    std::cout << "Wrote " << writer.packets() << " packets (" << writer.bytes()
              << " packet bytes) to " << writer.output_path() << "\n";
    daqiri::print_stats();
    daqiri::shutdown();
    daqiri_initialized = false;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "pcap writer failed: " << e.what() << "\n";
    if (daqiri_initialized) {
      daqiri::shutdown();
    }
    return 1;
  }
}
