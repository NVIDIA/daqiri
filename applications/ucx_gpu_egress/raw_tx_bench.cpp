// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "image_geometry.h"
#include "raw/image_pattern.h"

#include <daqiri/daqiri.h>

#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <arpa/inet.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

namespace geometry = daqiri::ucx_example::geometry;
using daqiri::ucx_example::raw_image_pixel;

constexpr std::size_t kEtherIpv4UdpBytes = 42;
constexpr std::size_t kSequenceOffset = kEtherIpv4UdpBytes;
constexpr std::size_t kPayloadOffset = 80;
constexpr std::size_t kPacketsPerImage = 16;
constexpr std::size_t kFragmentBytes = geometry::kImageBytes / kPacketsPerImage;
constexpr std::size_t kPacketsPerBatch = geometry::kImagesPerBatch * kPacketsPerImage;
constexpr std::size_t kFrameBytes = kPayloadOffset + kFragmentBytes;
constexpr std::uint64_t kSequenceSpace = std::uint64_t{1} << 32U;
constexpr std::uint64_t kBatchSpace = kSequenceSpace / kPacketsPerBatch;

static_assert(geometry::kImageBytes % kPacketsPerImage == 0);
static_assert(kFrameBytes <= std::numeric_limits<std::uint16_t>::max());

struct SourceConfig {
  std::string interface_name{"tx_port"};
  int queue_id{0};
  int cpu_core{-1};
  std::uint32_t burst_packets{2048};
  std::string eth_dst_addr;
  std::string ip_src_addr{"1.1.1.1"};
  std::string ip_dst_addr{"2.2.2.2"};
  std::uint16_t udp_src_port{4096};
  std::uint16_t udp_dst_port{4096};
};

struct BufferState {
  std::uint16_t fragment_slot{0};
  bool initialized{false};
};

struct RunLimit {
  std::optional<int> seconds;
  std::optional<std::uint64_t> batches;
};

void store_u16_be(std::uint8_t* output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 8U);
  output[1] = static_cast<std::uint8_t>(value);
}

void store_u32_be(std::uint8_t* output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 24U);
  output[1] = static_cast<std::uint8_t>(value >> 16U);
  output[2] = static_cast<std::uint8_t>(value >> 8U);
  output[3] = static_cast<std::uint8_t>(value);
}

bool parse_mac(const std::string& text, std::array<std::uint8_t, 6>& output) {
  unsigned values[6]{};
  char trailing = 0;
  if (std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x%c", &values[0], &values[1], &values[2],
                  &values[3], &values[4], &values[5], &trailing) != 6) {
    return false;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    if (values[index] > 0xffU) {
      return false;
    }
    output[index] = static_cast<std::uint8_t>(values[index]);
  }
  return true;
}

void pin_current_thread(int core) {
  if (core < 0 || core >= CPU_SETSIZE) {
    throw std::runtime_error("invalid source cpu_core " + std::to_string(core));
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  const int status = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (status != 0) {
    throw std::runtime_error("pthread_setaffinity_np failed for core " + std::to_string(core));
  }
}

SourceConfig load_source_config(const YAML::Node& root) {
  const YAML::Node node = root["ucx_gpu_raw_source"];
  if (!node || !node.IsMap()) {
    throw std::runtime_error("config requires a ucx_gpu_raw_source map");
  }
  SourceConfig config;
  config.interface_name = node["interface_name"].as<std::string>(config.interface_name);
  config.queue_id = node["queue_id"].as<int>(config.queue_id);
  config.cpu_core = node["cpu_core"].as<int>(config.cpu_core);
  config.burst_packets = node["burst_packets"].as<std::uint32_t>(config.burst_packets);
  config.eth_dst_addr = node["eth_dst_addr"].as<std::string>("");
  config.ip_src_addr = node["ip_src_addr"].as<std::string>(config.ip_src_addr);
  config.ip_dst_addr = node["ip_dst_addr"].as<std::string>(config.ip_dst_addr);
  config.udp_src_port = node["udp_src_port"].as<std::uint16_t>(config.udp_src_port);
  config.udp_dst_port = node["udp_dst_port"].as<std::uint16_t>(config.udp_dst_port);
  if (config.interface_name.empty() || config.queue_id < 0 || config.cpu_core < 0 ||
      config.burst_packets == 0 || config.burst_packets % kPacketsPerBatch != 0 ||
      config.eth_dst_addr.empty()) {
    throw std::runtime_error(
        "interface, queue, core, and destination MAC must be valid; burst_packets must be a "
        "nonzero multiple of 256");
  }
  return config;
}

RunLimit parse_limit(int argc, char** argv) {
  RunLimit limit;
  for (int index = 2; index < argc; ++index) {
    const std::string option = argv[index];
    if (index + 1 >= argc) {
      throw std::runtime_error(std::string("usage: ") + argv[0] +
                               " CONFIG (--seconds N | --batches N)");
    }
    if (option == "--seconds") {
      limit.seconds = std::stoi(argv[++index]);
    } else if (option == "--batches") {
      limit.batches = std::stoull(argv[++index]);
    } else {
      throw std::runtime_error("unknown option: " + option);
    }
  }
  if (limit.seconds.has_value() == limit.batches.has_value() ||
      (limit.seconds && *limit.seconds <= 0) || (limit.batches && *limit.batches == 0) ||
      (limit.batches && *limit.batches > kBatchSpace)) {
    throw std::runtime_error(
        "specify exactly one positive run limit; --batches must not exceed the 32-bit sequence "
        "space");
  }
  return limit;
}

void initialize_outer_header(std::uint8_t* frame, const SourceConfig& config,
                             const std::array<std::uint8_t, 6>& destination_mac,
                             const in_addr& source_address, const in_addr& destination_address) {
  std::memset(frame, 0, kFrameBytes);
  std::memcpy(frame, destination_mac.data(), destination_mac.size());
  store_u16_be(frame + 12, 0x0800);
  frame[14] = 0x45;
  store_u16_be(frame + 16, static_cast<std::uint16_t>(kFrameBytes - 14));
  store_u16_be(frame + 20, 0x4000);
  frame[22] = 64;
  frame[23] = IPPROTO_UDP;
  std::memcpy(frame + 26, &source_address.s_addr, sizeof(source_address.s_addr));
  std::memcpy(frame + 30, &destination_address.s_addr, sizeof(destination_address.s_addr));
  store_u16_be(frame + 34, config.udp_src_port);
  store_u16_be(frame + 36, config.udp_dst_port);
  store_u16_be(frame + 38, static_cast<std::uint16_t>(kFrameBytes - 34));
}

void initialize_fragment_payload(std::uint8_t* payload, std::uint16_t fragment_slot) {
  const std::uint64_t image_sequence = fragment_slot / kPacketsPerImage;
  const std::uint32_t first_pixel =
      static_cast<std::uint32_t>(fragment_slot % kPacketsPerImage) *
      static_cast<std::uint32_t>(kFragmentBytes / sizeof(std::uint16_t));
  auto* pixels = reinterpret_cast<std::uint16_t*>(payload);
  for (std::uint32_t index = 0; index < kFragmentBytes / sizeof(std::uint16_t); ++index) {
    pixels[index] = raw_image_pixel(image_sequence, first_pixel + index);
  }
}

void update_image_sequence_tag(std::uint8_t* payload, std::uint64_t image_sequence) {
  auto* pixels = reinterpret_cast<std::uint16_t*>(payload);
  for (std::uint32_t index = 0; index < 4; ++index) {
    pixels[index] = raw_image_pixel(image_sequence, index);
  }
}

int run(const char* config_path, const RunLimit& limit) {
  const YAML::Node root = YAML::LoadFile(config_path);
  const SourceConfig config = load_source_config(root);
  pin_current_thread(config.cpu_core);

  std::array<std::uint8_t, 6> destination_mac{};
  if (!parse_mac(config.eth_dst_addr, destination_mac)) {
    throw std::runtime_error("invalid eth_dst_addr: " + config.eth_dst_addr);
  }
  in_addr source_address{};
  in_addr destination_address{};
  if (inet_pton(AF_INET, config.ip_src_addr.c_str(), &source_address) != 1 ||
      inet_pton(AF_INET, config.ip_dst_addr.c_str(), &destination_address) != 1) {
    throw std::runtime_error("ip_src_addr and ip_dst_addr must be numeric IPv4 addresses");
  }

  if (daqiri::daqiri_init(config_path) != daqiri::Status::SUCCESS) {
    throw std::runtime_error("daqiri_init failed");
  }
  const int port_id = daqiri::get_port_id(config.interface_name);
  if (port_id < 0) {
    daqiri::shutdown();
    throw std::runtime_error("unknown interface_name: " + config.interface_name);
  }

  std::unordered_map<void*, BufferState> buffers;
  std::uint64_t next_packet = 0;
  std::uint64_t sent_packets = 0;
  std::uint64_t sent_bytes = 0;
  std::uint64_t sent_bursts = 0;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline =
      start + std::chrono::seconds(limit.seconds.value_or(std::numeric_limits<int>::max()));

  while ((!limit.seconds || std::chrono::steady_clock::now() < deadline) &&
         (!limit.batches || next_packet / kPacketsPerBatch < *limit.batches)) {
    std::uint32_t requested_packets = config.burst_packets;
    if (limit.batches) {
      const std::uint64_t remaining_packets =
          (*limit.batches - next_packet / kPacketsPerBatch) * kPacketsPerBatch;
      requested_packets =
          static_cast<std::uint32_t>(std::min<std::uint64_t>(requested_packets, remaining_packets));
    }
    daqiri::BurstParams* burst = daqiri::create_tx_burst_params();
    daqiri::set_header(burst, static_cast<std::uint16_t>(port_id),
                       static_cast<std::uint16_t>(config.queue_id), requested_packets, 1);
    if (!daqiri::is_tx_burst_available(burst)) {
      daqiri::free_tx_metadata(burst);
      std::this_thread::yield();
      continue;
    }
    if (daqiri::get_tx_packet_burst(burst) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(burst);
      continue;
    }

    const auto packet_count = static_cast<std::size_t>(daqiri::get_num_packets(burst));
    bool failed = packet_count == 0 || packet_count % kPacketsPerBatch != 0;
    for (std::size_t packet_index = 0; packet_index < packet_count && !failed; ++packet_index) {
      auto* frame = static_cast<std::uint8_t*>(
          daqiri::get_segment_packet_ptr(burst, 0, static_cast<int>(packet_index)));
      if (frame == nullptr) {
        failed = true;
        break;
      }

      const std::uint64_t packet_sequence = next_packet + packet_index;
      const auto fragment_slot = static_cast<std::uint16_t>(packet_sequence % kPacketsPerBatch);
      BufferState& state = buffers[frame];
      if (!state.initialized || state.fragment_slot != fragment_slot) {
        cudaPointerAttributes attributes{};
        const cudaError_t pointer_status = cudaPointerGetAttributes(&attributes, frame);
        if (pointer_status != cudaSuccess || attributes.type != cudaMemoryTypeHost) {
          failed = true;
          std::cerr << "raw source requires CPU-visible host-pinned TX buffers\n";
          break;
        }
        initialize_outer_header(frame, config, destination_mac, source_address,
                                destination_address);
        initialize_fragment_payload(frame + kPayloadOffset, fragment_slot);
        state.fragment_slot = fragment_slot;
        state.initialized = true;
      }

      store_u32_be(frame + kSequenceOffset, static_cast<std::uint32_t>(packet_sequence));
      if (fragment_slot % kPacketsPerImage == 0) {
        const std::uint64_t image_sequence = packet_sequence / kPacketsPerImage;
        update_image_sequence_tag(frame + kPayloadOffset, image_sequence);
      }
      if (daqiri::set_packet_lengths(burst, static_cast<int>(packet_index),
                                     {static_cast<int>(kFrameBytes)}) != daqiri::Status::SUCCESS) {
        failed = true;
      }
    }

    if (failed) {
      daqiri::free_all_packets_and_burst_tx(burst);
      daqiri::shutdown();
      throw std::runtime_error("failed to prepare a raw sequence TX burst");
    }
    const daqiri::Status send_status = daqiri::send_tx_burst(burst);
    if (send_status == daqiri::Status::SUCCESS) {
      next_packet += packet_count;
      sent_packets += packet_count;
      sent_bytes += packet_count * kFrameBytes;
      ++sent_bursts;
    } else if (send_status != daqiri::Status::NO_SPACE_AVAILABLE) {
      daqiri::free_all_packets_and_burst_tx(burst);
      daqiri::shutdown();
      throw std::runtime_error("send_tx_burst failed without consuming the burst");
    }
  }

  if (daqiri::wait_for_tx_idle(10'000) != daqiri::Status::SUCCESS) {
    daqiri::shutdown();
    throw std::runtime_error("timed out draining raw TX operations");
  }
  daqiri::print_stats();
  daqiri::shutdown();
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "raw TX complete: packets=" << sent_packets
            << " batches=" << sent_packets / kPacketsPerBatch << " bytes=" << sent_bytes
            << " bursts=" << sent_bursts << " seconds=" << elapsed << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      throw std::runtime_error(std::string("usage: ") + argv[0] +
                               " CONFIG (--seconds N | --batches N)");
    }
    return run(argv[1], parse_limit(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
