/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <yaml-cpp/yaml.h>
#include <cuda_runtime_api.h>

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <daqiri/daqiri.h>

#include "ethernet_udp_view.h"
#include "frame_assembler.h"
#include "gvsp_packet.h"

namespace {

using daqiri::examples::gvsp::EthernetUdpView;
using daqiri::examples::gvsp::FrameAssembler;
using daqiri::examples::gvsp::FrameAssemblerConfig;
using daqiri::examples::gvsp::FrameView;
using daqiri::examples::gvsp::GvspPacketView;
using daqiri::examples::gvsp::IncompleteFrameReport;
using daqiri::examples::gvsp::IncompleteReason;
using daqiri::examples::gvsp::PacketKind;

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int) {
  g_stop_requested = 1;
}

struct AppConfig {
  std::string interface_name;
  int queue_id = 0;
  int cpu_core = -1;
  FrameAssemblerConfig assembler;
  bool log_incomplete = true;
  int report_interval_seconds = 2;
};

struct RunOptions {
  int seconds = 0;
  uint64_t max_frames = 0;
  bool log_every_frame = false;
  std::string dump_first_frame;
};

struct RuntimeStats {
  uint64_t raw_packets = 0;
  uint64_t raw_frame_bytes = 0;
  uint64_t gvsp_packets = 0;
  uint64_t completed_frames = 0;
  uint64_t completed_image_bytes = 0;
  uint64_t malformed_wire_packets = 0;
  uint64_t malformed_gvsp_packets = 0;
  uint64_t unsupported_gvsp_packets = 0;
};

class BurstGuard {
 public:
  explicit BurstGuard(daqiri::BurstParams* burst) : burst_(burst) {}
  ~BurstGuard() {
    if (burst_ != nullptr) {
      daqiri::free_all_packets_and_burst_rx(burst_);
    }
  }
  BurstGuard(const BurstGuard&) = delete;
  BurstGuard& operator=(const BurstGuard&) = delete;

 private:
  daqiri::BurstParams* burst_;
};

class PinnedFrameBuffer {
 public:
  explicit PinnedFrameBuffer(size_t size) : size_(size) {
    const cudaError_t result =
        cudaHostAlloc(reinterpret_cast<void**>(&data_), size_, cudaHostAllocDefault);
    if (result != cudaSuccess) {
      throw std::runtime_error(std::string("cudaHostAlloc for completed frame failed: ") +
                               cudaGetErrorString(result));
    }
  }
  ~PinnedFrameBuffer() {
    if (data_ != nullptr) cudaFreeHost(data_);
  }
  PinnedFrameBuffer(const PinnedFrameBuffer&) = delete;
  PinnedFrameBuffer& operator=(const PinnedFrameBuffer&) = delete;
  std::byte* data() const {
    return data_;
  }
  size_t size() const {
    return size_;
  }

 private:
  std::byte* data_ = nullptr;
  size_t size_ = 0;
};
uint32_t parse_u32(const YAML::Node& node, const char* name) {
  if (!node[name]) {
    throw std::runtime_error(std::string("missing gvsp_receiver.") + name);
  }
  if (node[name].IsScalar()) {
    const std::string text = node[name].as<std::string>();
    size_t consumed = 0;
    const auto value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value > UINT32_MAX) {
      throw std::runtime_error(std::string("invalid gvsp_receiver.") + name);
    }
    return static_cast<uint32_t>(value);
  }
  throw std::runtime_error(std::string("invalid gvsp_receiver.") + name);
}

AppConfig load_app_config(const std::string& path) {
  const YAML::Node root = YAML::LoadFile(path);
  const YAML::Node node = root["gvsp_receiver"];
  if (!node) {
    throw std::runtime_error("missing gvsp_receiver config block");
  }

  AppConfig config;
  config.interface_name = node["interface_name"].as<std::string>();
  config.queue_id = node["queue_id"].as<int>(0);
  config.cpu_core = node["cpu_core"].as<int>(-1);
  config.assembler.width = node["width"].as<uint32_t>();
  config.assembler.height = node["height"].as<uint32_t>();
  config.assembler.pixel_format = parse_u32(node, "pixel_format_code");
  config.assembler.frame_bytes = node["frame_bytes"].as<size_t>();
  config.assembler.data_payload_bytes = node["data_payload_bytes"].as<size_t>();
  config.assembler.frame_timeout = std::chrono::milliseconds(node["frame_timeout_ms"].as<int>(100));
  config.report_interval_seconds = node["report_interval_seconds"].as<int>(2);
  const std::string policy = node["incomplete_frame_policy"].as<std::string>("log");
  if (policy != "drop" && policy != "log") {
    throw std::runtime_error("incomplete_frame_policy must be 'drop' or 'log'");
  }
  config.log_incomplete = policy == "log";
  if (config.interface_name.empty() || config.queue_id < 0 || config.cpu_core < -1 ||
      config.report_interval_seconds <= 0) {
    throw std::runtime_error("invalid interface, queue, CPU, or report interval");
  }
  return config;
}

RunOptions parse_options(int argc, char** argv) {
  RunOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--log-every-frame") {
      options.log_every_frame = true;
    } else if (arg == "--seconds" && i + 1 < argc) {
      options.seconds = std::stoi(argv[++i]);
    } else if (arg == "--max-frames" && i + 1 < argc) {
      options.max_frames = std::stoull(argv[++i]);
    } else if (arg == "--dump-first-frame" && i + 1 < argc) {
      options.dump_first_frame = argv[++i];
    } else {
      throw std::runtime_error("unknown or incomplete option: " + arg);
    }
  }
  if (options.seconds < 0) {
    throw std::runtime_error("--seconds must be non-negative");
  }
  return options;
}

bool pin_current_thread(int core) {
  if (core < 0) {
    return true;
  }
  if (core >= CPU_SETSIZE) {
    std::cerr << "GVSP worker CPU " << core << " exceeds CPU_SETSIZE " << CPU_SETSIZE << '\n';
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  const int result = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (result != 0) {
    std::cerr << "failed to pin GVSP worker to CPU " << core << ": " << std::strerror(result)
              << '\n';
    return false;
  }
  return true;
}

const char* reason_name(IncompleteReason reason) {
  switch (reason) {
    case IncompleteReason::TIMEOUT:
      return "timeout";
    case IncompleteReason::NEW_LEADER:
      return "new-leader";
    case IncompleteReason::SHUTDOWN:
      return "shutdown";
  }
  return "unknown";
}

void print_usage(const char* program) {
  std::cerr << "Usage: " << program
            << " <config.yaml> [--seconds N] [--max-frames N] [--log-every-frame] "
               "[--dump-first-frame PATH]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  AppConfig config;
  RunOptions options;
  try {
    config = load_app_config(argv[1]);
    options = parse_options(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "configuration error: " << e.what() << '\n';
    print_usage(argv[0]);
    return 1;
  }

  RuntimeStats stats;
  bool dumped_frame = false;
  std::optional<PinnedFrameBuffer> completed_frame_buffer;
  try {
    completed_frame_buffer.emplace(config.assembler.frame_bytes);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }

  FrameAssembler assembler(
      config.assembler,
      [&](const FrameView& frame) {
        ++stats.completed_frames;
        stats.completed_image_bytes += frame.size_bytes;
        if (options.log_every_frame) {
          std::cout << "frame=" << frame.block_id << " camera_timestamp_ticks=";
          if (frame.camera_timestamp_ticks.has_value()) {
            std::cout << *frame.camera_timestamp_ticks;
          } else {
            std::cout << "n/a";
          }
          std::cout << " bytes=" << frame.size_bytes << " packets=" << frame.received_data_packets
                    << " duplicates=" << frame.duplicate_data_packets << " complete=true\n";
        }
        if (!dumped_frame && !options.dump_first_frame.empty()) {
          std::ofstream output(options.dump_first_frame, std::ios::binary | std::ios::trunc);
          if (!output.write(reinterpret_cast<const char*>(frame.data), frame.size_bytes)) {
            std::cerr << "failed to write " << options.dump_first_frame << '\n';
          } else {
            std::cout << "wrote first complete frame to " << options.dump_first_frame << '\n';
          }
          dumped_frame = true;
        }
      },
      [&](const IncompleteFrameReport& report) {
        if (config.log_incomplete) {
          std::cerr << "frame=" << report.block_id
                    << " complete=false reason=" << reason_name(report.reason)
                    << " bytes=" << report.received_bytes
                    << " received_packets=" << report.received_data_packets
                    << " missing_packets=" << report.missing_data_packets
                    << " duplicates=" << report.duplicate_data_packets << '\n';
        }
      },
      completed_frame_buffer->data(), completed_frame_buffer->size());
  std::string validation_error;
  if (!assembler.valid(&validation_error)) {
    std::cerr << "invalid assembler config: " << validation_error << '\n';
    return 1;
  }

  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }
  const int port_id = daqiri::get_port_id(config.interface_name);
  if (port_id < 0) {
    std::cerr << "unknown interface: " << config.interface_name << '\n';
    daqiri::shutdown();
    return 1;
  }
  const int num_rx_queues = static_cast<int>(daqiri::get_num_rx_queues(port_id));
  if (config.queue_id >= num_rx_queues) {
    std::cerr << "invalid RX queue " << config.queue_id << " for interface "
              << config.interface_name << " (configured queues: " << num_rx_queues << ")\n";
    daqiri::shutdown();
    return 1;
  }
  if (!pin_current_thread(config.cpu_core)) {
    daqiri::shutdown();
    return 1;
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  const auto started = std::chrono::steady_clock::now();
  auto last_report = started;
  RuntimeStats last_stats;

  while (g_stop_requested == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (options.seconds > 0 && now - started >= std::chrono::seconds(options.seconds)) {
      break;
    }
    if (options.max_frames > 0 && stats.completed_frames >= options.max_frames) {
      break;
    }

    daqiri::BurstParams* burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id, config.queue_id) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      assembler.check_timeout(now);
      std::this_thread::yield();
    } else {
      BurstGuard guard(burst);
      const int packet_count = static_cast<int>(daqiri::get_num_packets(burst));
      for (int i = 0; i < packet_count; ++i) {
        const auto* frame = static_cast<const uint8_t*>(daqiri::get_packet_ptr(burst, i));
        const size_t frame_size = daqiri::get_packet_length(burst, i);
        ++stats.raw_packets;
        stats.raw_frame_bytes += frame_size;

        EthernetUdpView udp;
        std::string error;
        if (!daqiri::examples::gvsp::parse_ethernet_udp(frame, frame_size, &udp, &error)) {
          ++stats.malformed_wire_packets;
          continue;
        }
        GvspPacketView packet;
        if (!daqiri::examples::gvsp::parse_gvsp_packet(udp.payload, udp.payload_size, &packet,
                                                       &error)) {
          ++stats.malformed_gvsp_packets;
          continue;
        }
        if (packet.kind == PacketKind::UNSUPPORTED) {
          ++stats.unsupported_gvsp_packets;
          continue;
        }
        ++stats.gvsp_packets;

        uint64_t rx_timestamp_ns = 0;
        std::optional<uint64_t> timestamp;
        if (daqiri::get_packet_rx_timestamp(burst, i, &rx_timestamp_ns) ==
            daqiri::Status::SUCCESS) {
          timestamp = rx_timestamp_ns;
        }
        assembler.consume(packet, now, timestamp);
      }
    }

    const auto report_now = std::chrono::steady_clock::now();
    if (report_now - last_report >= std::chrono::seconds(config.report_interval_seconds)) {
      const double interval = std::chrono::duration<double>(report_now - last_report).count();
      const uint64_t frames = stats.completed_frames - last_stats.completed_frames;
      const uint64_t image_bytes = stats.completed_image_bytes - last_stats.completed_image_bytes;
      const uint64_t raw_bytes = stats.raw_frame_bytes - last_stats.raw_frame_bytes;
      std::cout << std::fixed << std::setprecision(2) << "frames=" << stats.completed_frames
                << " fps=" << frames / interval
                << " image_gbps=" << (image_bytes * 8.0 / interval / 1e9)
                << " raw_l2_gbps=" << (raw_bytes * 8.0 / interval / 1e9)
                << " raw_packets=" << stats.raw_packets
                << " incomplete=" << assembler.stats().incomplete_frames
                << " malformed_wire=" << stats.malformed_wire_packets
                << " malformed_gvsp=" << stats.malformed_gvsp_packets
                << " unsupported_gvsp=" << stats.unsupported_gvsp_packets
                << " duplicates=" << assembler.stats().duplicate_packets << '\n';
      last_stats = stats;
      last_report = report_now;
    }
  }

  assembler.shutdown();
  daqiri::print_stats();
  daqiri::shutdown();
  std::cout << "GVSP receiver complete: frames=" << stats.completed_frames
            << " incomplete=" << assembler.stats().incomplete_frames
            << " raw_packets=" << stats.raw_packets << " gvsp_packets=" << stats.gvsp_packets
            << '\n';
  return 0;
}
