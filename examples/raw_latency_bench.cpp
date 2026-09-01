/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <arpa/inet.h>
#include <endian.h>
#include <fcntl.h>
#include <linux/ptp_clock.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <daqiri/daqiri.h>

namespace {

constexpr uint32_t kMarkerMagic = 0x4441514c;  // "DAQL"
constexpr uint32_t kMinPacketSize = 64;
constexpr uint32_t kMaxPacketSize = 8192;

struct __attribute__((packed)) WireMarker {
  uint32_t magic_be;
  uint32_t sequence_be;
  uint64_t tx_call_ns_be;
};

static_assert(sizeof(daqiri::UDPIPV4Pkt) + sizeof(WireMarker) <= kMinPacketSize,
              "latency marker must fit in the smallest packet");

struct Options {
  std::string config_path;
  uint32_t samples = 10000;
  uint32_t warmup = 1000;
  uint32_t timeout_ms = 100;
  int realtime_priority = 0;
  std::string csv_path;
};

struct BenchConfig {
  std::string tx_interface_name;
  std::string rx_interface_name;
  int tx_queue_id = 0;
  int rx_queue_id = 0;
  int cpu_core = -1;
  std::string ptp_device;
  std::string eth_dst_addr;
  std::string ip_src_addr = "192.0.2.1";
  std::string ip_dst_addr = "192.0.2.2";
  uint16_t udp_src_port = 4096;
  uint16_t udp_dst_port = 4096;
};

struct Sample {
  uint32_t packet_size = 0;
  uint32_t sequence = 0;
  uint64_t tx_call_realtime_ns = 0;
  int64_t clock_offset_ns = 0;
  int64_t tx_call_to_return_ns = 0;
  int64_t tx_call_to_rx_hw_ns = 0;
  int64_t rx_hw_to_app_ns = 0;
  int64_t tx_call_to_app_ns = 0;
};

struct ClockCalibration {
  uint64_t system_realtime_ns = 0;
  int64_t device_minus_system_ns = 0;
  uint64_t bracket_ns = 0;
};

struct MetricSummary {
  int64_t min = 0;
  int64_t p50 = 0;
  int64_t p90 = 0;
  int64_t p99 = 0;
  int64_t max = 0;
  double mean = 0.0;
};

uint64_t realtime_ns() {
  timespec ts{};
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    throw std::runtime_error("clock_gettime(CLOCK_REALTIME) failed: " +
                             std::string(std::strerror(errno)));
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t monotonic_ns() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
    throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC_RAW) failed: " +
                             std::string(std::strerror(errno)));
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

int64_t delta_ns(uint64_t later, uint64_t earlier) {
  if (later >= earlier) {
    const uint64_t value = later - earlier;
    return value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? std::numeric_limits<int64_t>::max()
               : static_cast<int64_t>(value);
  }
  const uint64_t value = earlier - later;
  return value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
             ? std::numeric_limits<int64_t>::min()
             : -static_cast<int64_t>(value);
}

uint32_t parse_u32(const std::string& value, const char* option) {
  size_t used = 0;
  const unsigned long parsed = std::stoul(value, &used);
  if (used != value.size() || parsed > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument(std::string("invalid value for ") + option + ": " + value);
  }
  return static_cast<uint32_t>(parsed);
}

Options parse_options(int argc, char** argv) {
  if (argc < 2) {
    throw std::invalid_argument(
        std::string("usage: ") + argv[0] +
        " <config.yaml> [--samples N] [--warmup N] [--timeout-ms N] [--csv PATH] "
        "[--realtime-priority N]");
  }
  Options options;
  options.config_path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (i + 1 >= argc) {
      throw std::invalid_argument("missing value for " + arg);
    }
    const std::string value = argv[++i];
    if (arg == "--samples") {
      options.samples = parse_u32(value, "--samples");
    } else if (arg == "--warmup") {
      options.warmup = parse_u32(value, "--warmup");
    } else if (arg == "--timeout-ms") {
      options.timeout_ms = parse_u32(value, "--timeout-ms");
    } else if (arg == "--csv") {
      options.csv_path = value;
    } else if (arg == "--realtime-priority") {
      options.realtime_priority = static_cast<int>(parse_u32(value, "--realtime-priority"));
    } else {
      throw std::invalid_argument("unknown option: " + arg);
    }
  }
  if (options.samples == 0 || options.timeout_ms == 0) {
    throw std::invalid_argument("--samples and --timeout-ms must be greater than zero");
  }
  if (options.realtime_priority < 0 ||
      options.realtime_priority >= sched_get_priority_max(SCHED_FIFO)) {
    throw std::invalid_argument("--realtime-priority must be between 1 and 98 (or 0 to disable)");
  }
  return options;
}

BenchConfig parse_bench_config(const YAML::Node& root) {
  const YAML::Node node = root["bench_latency"];
  if (!node || !node.IsMap()) {
    throw std::invalid_argument("config must contain a bench_latency map");
  }
  BenchConfig cfg;
  const std::string common_interface = node["interface_name"].as<std::string>("");
  cfg.tx_interface_name = node["tx_interface_name"].as<std::string>(common_interface);
  cfg.rx_interface_name = node["rx_interface_name"].as<std::string>(common_interface);
  const int common_queue = node["queue_id"].as<int>(0);
  cfg.tx_queue_id = node["tx_queue_id"].as<int>(common_queue);
  cfg.rx_queue_id = node["rx_queue_id"].as<int>(common_queue);
  cfg.cpu_core = node["cpu_core"].as<int>(cfg.cpu_core);
  cfg.ptp_device = node["ptp_device"].as<std::string>("");
  cfg.eth_dst_addr = node["eth_dst_addr"].as<std::string>();
  cfg.ip_src_addr = node["ip_src_addr"].as<std::string>(cfg.ip_src_addr);
  cfg.ip_dst_addr = node["ip_dst_addr"].as<std::string>(cfg.ip_dst_addr);
  cfg.udp_src_port = node["udp_src_port"].as<uint16_t>(cfg.udp_src_port);
  cfg.udp_dst_port = node["udp_dst_port"].as<uint16_t>(cfg.udp_dst_port);
  if (cfg.tx_interface_name.empty() || cfg.rx_interface_name.empty()) {
    throw std::invalid_argument("bench_latency requires tx_interface_name and rx_interface_name");
  }
  return cfg;
}

uint64_t ptp_time_ns(const ptp_clock_time& time) {
  return static_cast<uint64_t>(time.sec) * 1000000000ULL + time.nsec;
}

ClockCalibration calibrate_clocks(const std::string& ptp_device) {
  const int fd = open(ptp_device.c_str(), O_RDONLY);
  if (fd < 0) {
    throw std::runtime_error("cannot open PTP device " + ptp_device + ": " + std::strerror(errno));
  }
  ptp_sys_offset_extended offset{};
  offset.n_samples = PTP_MAX_SAMPLES;
  if (ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &offset) != 0) {
    const std::string message =
        "PTP_SYS_OFFSET_EXTENDED failed for " + ptp_device + ": " + std::strerror(errno);
    close(fd);
    throw std::runtime_error(message);
  }
  close(fd);

  uint64_t best_bracket = std::numeric_limits<uint64_t>::max();
  std::vector<ClockCalibration> best;
  for (unsigned int i = 0; i < offset.n_samples; ++i) {
    const uint64_t before = ptp_time_ns(offset.ts[i][0]);
    const uint64_t device = ptp_time_ns(offset.ts[i][1]);
    const uint64_t after = ptp_time_ns(offset.ts[i][2]);
    const uint64_t bracket = after - before;
    const uint64_t midpoint = before + bracket / 2;
    const ClockCalibration sample{midpoint, delta_ns(device, midpoint), bracket};
    if (bracket < best_bracket) {
      best_bracket = bracket;
      best.clear();
    }
    if (bracket == best_bracket) {
      best.push_back(sample);
    }
  }
  std::sort(best.begin(), best.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.device_minus_system_ns < rhs.device_minus_system_ns;
  });
  return best[best.size() / 2];
}

int64_t interpolated_clock_offset(const ClockCalibration& before, const ClockCalibration& after,
                                  uint64_t system_time_ns) {
  if (after.system_realtime_ns <= before.system_realtime_ns ||
      system_time_ns <= before.system_realtime_ns) {
    return before.device_minus_system_ns;
  }
  if (system_time_ns >= after.system_realtime_ns) {
    return after.device_minus_system_ns;
  }
  const int64_t drift = after.device_minus_system_ns - before.device_minus_system_ns;
  const uint64_t elapsed = system_time_ns - before.system_realtime_ns;
  const uint64_t duration = after.system_realtime_ns - before.system_realtime_ns;
  return before.device_minus_system_ns +
         static_cast<int64_t>((static_cast<__int128>(drift) * elapsed) / duration);
}

void correct_cross_clock_metrics(Sample& sample, const ClockCalibration& before,
                                 const ClockCalibration& after) {
  sample.clock_offset_ns = interpolated_clock_offset(before, after, sample.tx_call_realtime_ns);
  sample.tx_call_to_rx_hw_ns -= sample.clock_offset_ns;
  sample.rx_hw_to_app_ns += sample.clock_offset_ns;
}

void pin_current_thread(int cpu_core) {
  if (cpu_core < 0) {
    return;
  }
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_core, &cpuset);
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  if (rc != 0) {
    throw std::runtime_error("failed to pin latency thread to CPU " + std::to_string(cpu_core) +
                             ": " + std::strerror(rc));
  }
}

void configure_realtime(int priority) {
  if (priority == 0) {
    return;
  }
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    throw std::runtime_error("mlockall failed: " + std::string(std::strerror(errno)));
  }
  sched_param params{};
  params.sched_priority = priority;
  const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &params);
  if (rc != 0) {
    throw std::runtime_error("SCHED_FIFO setup failed: " + std::string(std::strerror(rc)));
  }
  int policy = 0;
  sched_param actual{};
  const int verify_rc = pthread_getschedparam(pthread_self(), &policy, &actual);
  if (verify_rc != 0 || policy != SCHED_FIFO || actual.sched_priority != priority) {
    throw std::runtime_error("failed to verify requested SCHED_FIFO policy");
  }
  std::cerr << "latency thread running SCHED_FIFO priority " << priority
            << " with current/future mappings locked\n";
}

void initialize_cuda() {
  const cudaError_t set_status = cudaSetDevice(0);
  if (set_status != cudaSuccess) {
    throw std::runtime_error("failed to select CUDA device 0: " +
                             std::string(cudaGetErrorString(set_status)));
  }
  const cudaError_t init_status = cudaFree(nullptr);
  if (init_status != cudaSuccess) {
    throw std::runtime_error("failed to initialize CUDA device 0: " +
                             std::string(cudaGetErrorString(init_status)));
  }
}

std::array<uint8_t, ETH_ALEN> parse_mac(const std::string& value) {
  std::array<uint8_t, ETH_ALEN> mac{};
  unsigned int bytes[ETH_ALEN]{};
  if (std::sscanf(value.c_str(), "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3],
                  &bytes[4], &bytes[5]) != ETH_ALEN) {
    throw std::invalid_argument("invalid eth_dst_addr: " + value);
  }
  for (size_t i = 0; i < mac.size(); ++i) {
    if (bytes[i] > 0xff) {
      throw std::invalid_argument("invalid eth_dst_addr: " + value);
    }
    mac[i] = static_cast<uint8_t>(bytes[i]);
  }
  return mac;
}

std::vector<uint8_t> make_packet_template(uint32_t packet_size, const BenchConfig& cfg) {
  std::vector<uint8_t> data(packet_size, 0);
  auto* pkt = reinterpret_cast<daqiri::UDPIPV4Pkt*>(data.data());
  const auto dst = parse_mac(cfg.eth_dst_addr);
  std::memcpy(pkt->eth.h_dest, dst.data(), dst.size());
  pkt->eth.h_proto = htons(ETH_P_IP);
  pkt->ip.version = 4;
  pkt->ip.ihl = 5;
  pkt->ip.ttl = 64;
  pkt->ip.protocol = IPPROTO_UDP;
  pkt->ip.tot_len = htons(static_cast<uint16_t>(packet_size - sizeof(ethhdr)));
  if (inet_pton(AF_INET, cfg.ip_src_addr.c_str(), &pkt->ip.saddr) != 1 ||
      inet_pton(AF_INET, cfg.ip_dst_addr.c_str(), &pkt->ip.daddr) != 1) {
    throw std::invalid_argument("invalid bench_latency IPv4 address");
  }
  pkt->udp.source = htons(cfg.udp_src_port);
  pkt->udp.dest = htons(cfg.udp_dst_port);
  pkt->udp.len = htons(static_cast<uint16_t>(packet_size - sizeof(ethhdr) - sizeof(iphdr)));
  return data;
}

void drain_rx(int port_id, int queue_id) {
  for (;;) {
    daqiri::BurstParams* burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id, queue_id) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      return;
    }
    daqiri::free_all_packets_and_burst_rx(burst);
  }
}

std::optional<Sample> run_sample(int tx_port_id, int rx_port_id, const BenchConfig& cfg,
                                 const std::vector<uint8_t>& packet_template, uint32_t sequence,
                                 uint32_t timeout_ms) {
  const uint64_t deadline = monotonic_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ULL;
  daqiri::BurstParams* tx = nullptr;
  while (monotonic_ns() < deadline) {
    tx = daqiri::create_tx_burst_params();
    if (tx == nullptr) {
      continue;
    }
    daqiri::set_header(tx, static_cast<uint16_t>(tx_port_id),
                       static_cast<uint16_t>(cfg.tx_queue_id), 1, 1);
    if (!daqiri::is_tx_burst_available(tx)) {
      daqiri::free_tx_metadata(tx);
      tx = nullptr;
      continue;
    }
    if (daqiri::get_tx_packet_burst(tx) == daqiri::Status::SUCCESS) {
      break;
    }
    daqiri::free_tx_metadata(tx);
    tx = nullptr;
  }
  if (tx == nullptr) {
    return std::nullopt;
  }

  std::vector<uint8_t> tx_staging(packet_template);
  auto* marker = reinterpret_cast<WireMarker*>(tx_staging.data() + sizeof(daqiri::UDPIPV4Pkt));
  marker->magic_be = htobe32(kMarkerMagic);
  marker->sequence_be = htobe32(sequence);
  marker->tx_call_ns_be = 0;
  void* tx_packet = daqiri::get_packet_ptr(tx, 0);
  const cudaError_t tx_copy_status =
      cudaMemcpy(tx_packet, tx_staging.data(), tx_staging.size(), cudaMemcpyDefault);
  if (tx_copy_status != cudaSuccess) {
    daqiri::free_all_packets_and_burst_tx(tx);
    throw std::runtime_error("failed to initialize TX packet: " +
                             std::string(cudaGetErrorString(tx_copy_status)));
  }
  daqiri::set_packet_lengths(tx, 0, {static_cast<int>(packet_template.size())});
  const uint64_t tx_call_ns = realtime_ns();

  const daqiri::Status send_status = daqiri::send_tx_burst(tx);
  const uint64_t tx_return_ns = realtime_ns();
  if (send_status != daqiri::Status::SUCCESS) {
    if (send_status != daqiri::Status::NO_SPACE_AVAILABLE) {
      daqiri::free_all_packets_and_burst_tx(tx);
    }
    return std::nullopt;
  }

  while (monotonic_ns() < deadline) {
    daqiri::BurstParams* rx = nullptr;
    const daqiri::Status rx_status = daqiri::get_rx_burst(&rx, rx_port_id, cfg.rx_queue_id);
    if (rx_status != daqiri::Status::SUCCESS || rx == nullptr) {
      continue;
    }
    const uint64_t rx_app_ns = realtime_ns();
    std::optional<Sample> result;
    const int packets = static_cast<int>(daqiri::get_num_packets(rx));
    for (int i = 0; i < packets; ++i) {
      if (daqiri::get_packet_length(rx, i) < sizeof(daqiri::UDPIPV4Pkt) + sizeof(WireMarker)) {
        continue;
      }
      WireMarker rx_marker{};
      const auto* marker_ptr =
          static_cast<const uint8_t*>(daqiri::get_packet_ptr(rx, i)) + sizeof(daqiri::UDPIPV4Pkt);
      const cudaError_t rx_copy_status =
          cudaMemcpy(&rx_marker, marker_ptr, sizeof(rx_marker), cudaMemcpyDefault);
      if (rx_copy_status != cudaSuccess) {
        daqiri::free_all_packets_and_burst_rx(rx);
        throw std::runtime_error("failed to inspect RX packet: " +
                                 std::string(cudaGetErrorString(rx_copy_status)));
      }
      if (be32toh(rx_marker.magic_be) != kMarkerMagic ||
          be32toh(rx_marker.sequence_be) != sequence) {
        continue;
      }
      uint64_t rx_hw_ns = 0;
      const daqiri::Status timestamp_status = daqiri::get_packet_rx_timestamp(rx, i, &rx_hw_ns);
      if (timestamp_status != daqiri::Status::SUCCESS) {
        daqiri::free_all_packets_and_burst_rx(rx);
        throw std::runtime_error(
            "matching packet did not provide an RX hardware timestamp; check "
            "rx.hardware_timestamps and NIC support");
      }
      result = Sample{static_cast<uint32_t>(packet_template.size()),
                      sequence,
                      tx_call_ns,
                      0,
                      delta_ns(tx_return_ns, tx_call_ns),
                      delta_ns(rx_hw_ns, tx_call_ns),
                      delta_ns(rx_app_ns, rx_hw_ns),
                      delta_ns(rx_app_ns, tx_call_ns)};
      break;
    }
    daqiri::free_all_packets_and_burst_rx(rx);
    if (result) {
      return result;
    }
  }
  return std::nullopt;
}

MetricSummary summarize(std::vector<int64_t> values) {
  std::sort(values.begin(), values.end());
  const auto percentile = [&](double fraction) {
    const size_t index =
        static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
  };
  long double total = 0.0;
  for (const int64_t value : values) {
    total += static_cast<long double>(value);
  }
  return MetricSummary{values.front(),   percentile(0.50),
                       percentile(0.90), percentile(0.99),
                       values.back(),    static_cast<double>(total / values.size())};
}

void print_metric(const char* name) {
  std::cout << ',' << name << "_min_ns," << name << "_p50_ns," << name << "_p90_ns," << name
            << "_p99_ns," << name << "_max_ns," << name << "_mean_ns";
}

void print_metric_values(const MetricSummary& metric) {
  std::cout << ',' << metric.min << ',' << metric.p50 << ',' << metric.p90 << ',' << metric.p99
            << ',' << metric.max << ',' << std::fixed << std::setprecision(1) << metric.mean;
}

void write_samples(const std::string& path, const std::vector<Sample>& samples) {
  if (path.empty()) {
    return;
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("cannot open CSV output: " + path);
  }
  output << "packet_size,sequence,tx_call_realtime_ns,clock_offset_ns,"
            "tx_call_to_return_ns,tx_call_to_rx_hw_ns,"
            "rx_hw_to_app_ns,tx_call_to_app_ns\n";
  for (const auto& sample : samples) {
    output << sample.packet_size << ',' << sample.sequence << ',' << sample.tx_call_realtime_ns
           << ',' << sample.clock_offset_ns << ',' << sample.tx_call_to_return_ns << ','
           << sample.tx_call_to_rx_hw_ns << ',' << sample.rx_hw_to_app_ns << ','
           << sample.tx_call_to_app_ns << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const YAML::Node root = YAML::LoadFile(options.config_path);
    const BenchConfig cfg = parse_bench_config(root);
    pin_current_thread(cfg.cpu_core);
    initialize_cuda();

    if (daqiri::daqiri_init(options.config_path) != daqiri::Status::SUCCESS) {
      std::cerr << "daqiri_init failed\n";
      return 1;
    }
    configure_realtime(options.realtime_priority);
    const int tx_port_id = daqiri::get_port_id(cfg.tx_interface_name);
    const int rx_port_id = daqiri::get_port_id(cfg.rx_interface_name);
    if (tx_port_id < 0 || rx_port_id < 0) {
      std::cerr << "invalid bench_latency TX/RX interface name\n";
      daqiri::shutdown();
      return 1;
    }

    std::vector<Sample> all_samples;
    all_samples.reserve(static_cast<size_t>(options.samples) * 8);
    uint32_t sequence = 1;
    bool clock_domain_warning = false;
    uint64_t max_clock_uncertainty_ns = 0;

    std::cout << "# packet_size is the L2 frame size supplied to DAQIRI (FCS excluded)\n";
    std::cout << "# tx_call_to_rx_hw includes direct TX submission, NIC TX, return path, and NIC "
                 "RX\n";
    std::cout << "# rx_hw_to_app measures NIC ingress to get_rx_burst() return; use ptp_device "
                 "calibration or PTP sync\n";
    std::cout << "packet_size,samples,timeouts";
    print_metric("tx_call_to_return");
    print_metric("tx_call_to_rx_hw");
    print_metric("rx_hw_to_app");
    print_metric("tx_call_to_app");
    std::cout << '\n';

    for (uint32_t packet_size = kMinPacketSize; packet_size <= kMaxPacketSize; packet_size *= 2) {
      const auto packet_template = make_packet_template(packet_size, cfg);
      drain_rx(rx_port_id, cfg.rx_queue_id);
      const std::optional<ClockCalibration> clock_before =
          cfg.ptp_device.empty()
              ? std::nullopt
              : std::optional<ClockCalibration>(calibrate_clocks(cfg.ptp_device));
      uint32_t timeouts = 0;
      std::vector<Sample> size_samples;
      size_samples.reserve(options.samples);
      const uint64_t iterations = static_cast<uint64_t>(options.warmup) + options.samples;
      for (uint64_t i = 0; i < iterations; ++i) {
        auto sample = run_sample(tx_port_id, rx_port_id, cfg, packet_template, sequence++,
                                 options.timeout_ms);
        if (!sample) {
          if (i >= options.warmup) {
            ++timeouts;
          }
          continue;
        }
        if (i >= options.warmup) {
          size_samples.push_back(*sample);
        }
      }
      if (clock_before) {
        const ClockCalibration clock_after = calibrate_clocks(cfg.ptp_device);
        max_clock_uncertainty_ns =
            std::max(max_clock_uncertainty_ns,
                     std::max(clock_before->bracket_ns, clock_after.bracket_ns) / 2);
        for (auto& sample : size_samples) {
          correct_cross_clock_metrics(sample, *clock_before, clock_after);
        }
      }
      for (const auto& sample : size_samples) {
        if (sample.tx_call_to_rx_hw_ns < 0 || sample.rx_hw_to_app_ns < 0) {
          clock_domain_warning = true;
        }
        all_samples.push_back(sample);
      }
      if (size_samples.empty()) {
        std::cout << packet_size << ",0," << timeouts << '\n';
        continue;
      }

      std::vector<int64_t> tx_return;
      std::vector<int64_t> tx_hw;
      std::vector<int64_t> rx_app;
      std::vector<int64_t> end_to_end;
      tx_return.reserve(size_samples.size());
      tx_hw.reserve(size_samples.size());
      rx_app.reserve(size_samples.size());
      end_to_end.reserve(size_samples.size());
      for (const auto& sample : size_samples) {
        tx_return.push_back(sample.tx_call_to_return_ns);
        tx_hw.push_back(sample.tx_call_to_rx_hw_ns);
        rx_app.push_back(sample.rx_hw_to_app_ns);
        end_to_end.push_back(sample.tx_call_to_app_ns);
      }
      std::cout << packet_size << ',' << size_samples.size() << ',' << timeouts;
      print_metric_values(summarize(std::move(tx_return)));
      print_metric_values(summarize(std::move(tx_hw)));
      print_metric_values(summarize(std::move(rx_app)));
      print_metric_values(summarize(std::move(end_to_end)));
      std::cout << '\n';
    }

    daqiri::print_stats();
    daqiri::shutdown();
    write_samples(options.csv_path, all_samples);
    if (!cfg.ptp_device.empty()) {
      std::cerr << "cross-clock metrics calibrated with " << cfg.ptp_device
                << "; maximum measured uncertainty +/- " << max_clock_uncertainty_ns << " ns\n";
    }
    if (clock_domain_warning) {
      std::cerr << "warning: negative cross-clock latency detected; clock calibration or PTP "
                   "synchronization is not accurate enough for these samples\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
