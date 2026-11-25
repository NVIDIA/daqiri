/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <daqiri/daqiri.h>
#include <yaml-cpp/yaml.h>

#include "tcp_record.h"
#include "../rdma_receive_batch.h"
#include "../rdma_pingpong.h"

#include <arpa/inet.h>
#include <endian.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using daqiri::BurstParams;
using daqiri::RDMAOpCode;
using daqiri::Status;
using namespace std::chrono_literals;

constexpr int kTimestampUnavailable = 3;
constexpr int kRawHeader = sizeof(daqiri::UDPIPV4Pkt);
constexpr auto kDrainTime = 500ms;

struct TimestampUnavailable : std::runtime_error {
  using std::runtime_error::runtime_error;
};

uint64_t now_ns() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

template <typename Rep, typename Period>
uint64_t ns(std::chrono::duration<Rep, Period> value) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
}

void check(Status status, const char* action) {
  if (status != Status::SUCCESS) {
    throw std::runtime_error(std::string(action) + " failed with status " +
                             std::to_string(static_cast<int>(status)));
  }
}

void pin(int cpu) {
  if (cpu < 0 || cpu >= CPU_SETSIZE) throw std::runtime_error("CPU index out of range");
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  const int error = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
  if (error) throw std::runtime_error("pinning benchmark thread: " + std::string(strerror(error)));
}

struct Fd {
  int value = -1;
  ~Fd() {
    if (value >= 0) close(value);
  }
};

// Only the out-of-band peer protocol uses this socket. An attempt gets a new
// connection, allowing either peer to close it immediately on a failed case.
class Control {
 public:
  Control(int fd, uint64_t timeout) : fd_(fd), timeout_(timeout) {}

  void send(std::array<uint64_t, 3> fields) const {
    for (auto& field : fields) field = htobe64(field);
    transfer(fields, true, timeout_);
  }

  std::array<uint64_t, 3> receive(uint64_t extra = 0) const {
    std::array<uint64_t, 3> fields{};
    transfer(fields, false, timeout_ + extra);
    for (auto& field : fields) field = be64toh(field);
    return fields;
  }

  void barrier(uint64_t phase, uint64_t extra = 0) const {
    if (fd_ < 0) return;
    send({phase, 0, 0});
    if (receive(extra)[0] != phase) throw std::runtime_error("peer phase mismatch");
  }

  void abort() const {
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
  }

 private:
  void transfer(std::array<uint64_t, 3>& fields, bool sending, uint64_t timeout) const {
    auto* bytes = reinterpret_cast<char*>(fields.data());
    size_t remaining = sizeof(fields);
    const uint64_t deadline = now_ns() + timeout;
    while (remaining) {
      const auto result = sending ? ::send(fd_, bytes, remaining, MSG_DONTWAIT | MSG_NOSIGNAL)
                                  : ::recv(fd_, bytes, remaining, MSG_DONTWAIT);
      if (result > 0) {
        remaining -= static_cast<size_t>(result);
        bytes += result;
        continue;
      }
      if (result == 0) throw std::runtime_error("peer closed benchmark control");
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        throw std::runtime_error("benchmark control: " + std::string(strerror(errno)));
      const auto now = now_ns();
      if (now >= deadline) throw std::runtime_error("benchmark control timed out");
      pollfd descriptor{fd_, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
      if (poll(&descriptor, 1, static_cast<int>((deadline - now + 999999) / 1000000)) < 0 &&
          errno != EINTR) {
        throw std::runtime_error("polling benchmark control: " + std::string(strerror(errno)));
      }
    }
  }

  int fd_;
  uint64_t timeout_;
};

struct Settings {
  std::string transport, role, client_ip, server_ip;
  bool raw, roce, tcp, latency, hardware, rtt, hardware_rtt, bidirectional, tx_direct, rx_direct;
  bool direct_roce_rtt, named_sender, prepared_device_frames, opaque_payload;
  int bytes, message_size, batch, rx_depth, tx_depth, samples, warmup_samples, realtime_priority;
  uint64_t duration, warmup, timeout;

  explicit Settings(const YAML::Node& cfg)
      : transport(cfg["transport"].as<std::string>()),
        role(cfg["role"].as<std::string>()),
        client_ip(cfg["client_ip"].as<std::string>()),
        server_ip(cfg["server_ip"].as<std::string>()),
        raw(transport == "raw"),
        roce(transport == "roce"),
        tcp(transport == "tcp"),
        latency(cfg["test"].as<std::string>() == "lat"),
        hardware(cfg["hardware"].as<bool>()),
        rtt(cfg["rtt"].as<bool>()),
        hardware_rtt(cfg["hardware_rtt"].as<bool>(false)),
        bidirectional(cfg["bidirectional"].as<bool>()),
        tx_direct(cfg["tx_direct"].as<bool>()),
        rx_direct(cfg["rx_direct"].as<bool>()),
        direct_roce_rtt(cfg["direct_roce_rtt"].as<bool>(false)),
        named_sender(cfg["named_sender"].as<bool>(false)),
        prepared_device_frames(cfg["prepared_device_frames"].as<bool>(false)),
        opaque_payload(cfg["opaque_payload"].as<bool>(false)),
        bytes(cfg["size"].as<int>()),
        message_size(cfg["message_size"].as<int>(0)),
        batch(cfg["batch"].as<int>()),
        rx_depth(cfg["rx_depth"].as<int>()),
        tx_depth(cfg["tx_depth"].as<int>()),
        samples(cfg["samples"].as<int>()),
        warmup_samples(cfg["warmup_samples"].as<int>()),
        realtime_priority(cfg["realtime_priority"].as<int>(0)),
        duration(ns(std::chrono::seconds(cfg["seconds"].as<int>()))),
        warmup(ns(std::chrono::seconds(cfg["warmup_seconds"].as<int>()))),
        timeout(ns(std::chrono::seconds(cfg["timeout"].as<int>()))) {}

  int application_bytes() const {
    return message_size ? message_size : bytes;
  }

  int tx_buffer_bytes() const {
    if (named_sender) return bytes - kRawHeader;
    return message_size ? std::max(bytes, ETH_ZLEN) : bytes;
  }
};

struct Segment {
  std::string name;
  int capacity;
  bool device;

  explicit Segment(const YAML::Node& node)
      : name(node["name"].as<std::string>()),
        capacity(node["buf_size"].as<int>()),
        device(node["kind"].as<std::string>() == "device") {}
};

struct Counts {
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t errors = 0;
};

struct ReceiveTime {
  uint64_t software = 0;
  uint64_t hardware = 0;
};

class Lane {
 public:
  Lane(const Settings& settings, const YAML::Node& node)
      : server(node["role"].as<std::string>() == "server"),
        queue(node["queue"].as<int>()),
        qid_(node["qid"].as<int>()),
        cpu_(node["cpu"].as<int>()),
        rx_app_cpu_(node["rx_app_cpu"].as<int>(cpu_)),
        server_port_(node["server_port"].as<uint16_t>()),
        rx_flow_id_(node["rx_flow_id"].as<daqiri::FlowId>(0)),
        cfg_(settings),
        name_(node["interface"].as<std::string>()),
        packet_(cfg_.tx_buffer_bytes()) {
    for (const auto& region : node["tx"]) {
      tx_.emplace_back(region);
      if (tx_.back().device && (!cfg_.opaque_payload || !region["zero_init"].as<bool>(false))) {
        throw std::runtime_error("device TX requires zero-initialized opaque payload buffers");
      }
    }
    for (const auto& region : node["rx"]) {
      rx_.emplace_back(region);
      if (rx_.back().device && !cfg_.opaque_payload) {
        throw std::runtime_error("device RX requires opaque payload buffers");
      }
    }
    if (cfg_.prepared_device_frames && !tx_.empty() &&
        (!cfg_.raw || cfg_.latency || cfg_.named_sender || cfg_.tx_direct || cfg_.batch < 2 ||
         tx_.size() != 1 || !tx_[0].device || tx_[0].capacity < cfg_.bytes)) {
      throw std::runtime_error(
          "prepared device frames require one complete raw TX buffer per frame");
    }
    if (cfg_.named_sender && !tx_.empty()) {
      auto& sender = sender_config_.emplace();
      sender.name_ = "perftest-" + node["role"].as<std::string>() + "-" + std::to_string(queue);
      sender.interface_ = name_;
      sender.dst_mac_ = node["dst_mac"].as<std::string>();
      sender.src_ipv4_ = node["local_ip"].as<std::string>();
      sender.dst_ipv4_ = node["peer_ip"].as<std::string>();
      sender.src_port_ = sender.dst_port_ = node["tx_udp_port"].as<uint16_t>();
      sender.mtu_ = cfg_.bytes;
    }
    if (cfg_.raw && !cfg_.named_sender) {
      daqiri::UDPIPV4Pkt header{};
      daqiri::format_eth_addr(reinterpret_cast<char*>(header.eth.h_source),
                              node["src_mac"].as<std::string>());
      daqiri::format_eth_addr(reinterpret_cast<char*>(header.eth.h_dest),
                              node["dst_mac"].as<std::string>());
      header.eth.h_proto = htons(ETH_P_IP);
      header.ip.version = 4;
      header.ip.ihl = 5;
      header.ip.ttl = 64;
      header.ip.protocol = IPPROTO_UDP;
      header.ip.tot_len = htons(cfg_.bytes - sizeof(ethhdr));
      header.ip.saddr = inet_addr(node["local_ip"].as<std::string>().c_str());
      header.ip.daddr = inet_addr(node["peer_ip"].as<std::string>().c_str());
      const auto* ip = reinterpret_cast<const uint8_t*>(&header.ip);
      uint32_t sum = 0;
      for (size_t i = 0; i < sizeof(iphdr); i += 2)
        sum += (static_cast<uint32_t>(ip[i]) << 8) | ip[i + 1];
      while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
      header.ip.check = htons(static_cast<uint16_t>(~sum));
      header.udp.source = header.udp.dest = htons(node["tx_udp_port"].as<uint16_t>());
      header.udp.len = htons(cfg_.bytes - sizeof(ethhdr) - sizeof(iphdr));
      // IPv4 UDP permits an omitted checksum.
      std::memcpy(packet_.data(), &header, sizeof(header));
    }
  }

  void add_device_frame_prefix(daqiri::DeviceMemoryPrefixes& prefixes) const {
    if (cfg_.prepared_device_frames && !tx_.empty()) {
      prefixes.emplace(tx_[0].name,
                       std::vector<uint8_t>(packet_.begin(), packet_.begin() + kRawHeader));
    }
  }

  void connect() {
    const int port = daqiri::get_port_id(name_);
    if (cfg_.raw) {
      if (port < 0) throw std::runtime_error("DAQIRI did not initialize " + name_);
      port_id_ = port;
      if (sender_config_) {
        check(daqiri::add_endpoint(*sender_config_, &sender_id_), "add named sender");
        // The engine owns the endpoint until shutdown. Keep only its ID for TX.
        sender_config_.reset();
      }
      return;
    }
    const auto deadline = now_ns() + cfg_.timeout;
    do {
      Status status;
      if (cfg_.roce) {
        status = server
                     ? daqiri::rdma_get_server_conn_id(cfg_.server_ip, server_port_, &connection_)
                     : daqiri::rdma_connect_to_server(cfg_.server_ip, server_port_, cfg_.client_ip,
                                                      &connection_);
      } else {
        status = server
                     ? daqiri::socket_get_server_conn_id(cfg_.server_ip, server_port_, &connection_)
                     : daqiri::socket_connect_to_server(cfg_.server_ip, server_port_,
                                                        cfg_.client_ip, &connection_);
      }
      if (status == Status::SUCCESS) {
        uint16_t port_id = 0, queue_id = 0;
        status = cfg_.roce ? daqiri::rdma_get_port_queue(connection_, &port_id, &queue_id)
                           : daqiri::socket_get_port_queue(connection_, &port_id, &queue_id);
        check(status, "resolve connection");
        if (queue_id != qid_ || (!cfg_.roce && (port < 0 || port_id != port))) {
          throw std::runtime_error("connection resolved to an unexpected interface or queue");
        }
        if (cfg_.tcp) {
          const timeval send_timeout{static_cast<time_t>(cfg_.timeout / ns(1s)), 0};
          check(daqiri::socket_setsockopt(connection_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                                          sizeof(send_timeout)),
                "bound TCP send");
        }
        port_id_ = port_id;
        return;
      }
      std::this_thread::sleep_for(10ms);
    } while (now_ns() < deadline);
    if (cfg_.roce && cfg_.hardware) {
      throw TimestampUnavailable("RoCE setup with hardware RX timestamps failed on " + name_);
    }
    throw std::runtime_error("connection timed out on " + name_);
  }

  void bind_thread(bool reader = false) const {
    const int cpu = reader ? rx_app_cpu_ : cpu_;
    pin(cpu);
    if (!cfg_.realtime_priority) return;
    sched_param priority{};
    priority.sched_priority = cfg_.realtime_priority;
    const int error = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
    if (error) throw std::runtime_error("setting SCHED_FIFO: " + std::string(strerror(error)));
    int policy = 0;
    sched_param actual{};
    if (pthread_getschedparam(pthread_self(), &policy, &actual) || policy != SCHED_FIFO ||
        actual.sched_priority != cfg_.realtime_priority) {
      throw std::runtime_error("could not verify latency thread SCHED_FIFO priority");
    }
    std::cerr << ("Latency " + std::string(server ? "server" : "client") + " queue " +
                  std::to_string(queue) + " on CPU " + std::to_string(cpu) +
                  " running SCHED_FIFO priority " + std::to_string(actual.sched_priority) + "\n");
  }
  bool can_receive() const {
    return !rx_.empty();
  }

  void prime() {
    if (!cfg_.roce || !can_receive()) return;
    const auto deadline = now_ns() + cfg_.timeout;
    while (rx_posted_ < cfg_.rx_depth) {
      refill();
      if (now_ns() >= deadline) throw std::runtime_error("RoCE receive posting timed out");
    }
  }

  void refill() {
    if (!cfg_.roce) return;
    while (rx_posted_ < cfg_.rx_depth && submit(1, true, false, nullptr, nullptr)) ++rx_posted_;
  }

  bool send_bandwidth(Counts& counts) {
    if (cfg_.roce && tx_posted_ + cfg_.batch > cfg_.tx_depth) return false;
    if (!submit(cfg_.batch, false, false, nullptr, nullptr, &counts.errors)) return false;
    counts.packets += cfg_.batch;
    counts.bytes += static_cast<uint64_t>(cfg_.batch) * cfg_.application_bytes();
    return true;
  }

  bool poll_bandwidth(Counts& counts, bool receive) {
    if (cfg_.roce && cfg_.rx_direct && cfg_.tx_direct && server && !cfg_.bidirectional &&
        bulk_receive_) {
      daqiri::bench::RdmaReceiveBatch batch;
      const auto status =
          daqiri::bench::recycle_rdma_receives(connection_, cfg_.bytes, true, batch);
      if (status != Status::NOT_SUPPORTED) {
        if (status == Status::NOT_READY) return false;
        check(status, "recycle RoCE receives");
        rx_posted_ -= static_cast<int>(batch.completed - batch.reposted);
        counts.errors += batch.completed - batch.valid;
        if (receive) {
          counts.packets += batch.valid;
          counts.bytes += batch.valid * cfg_.application_bytes();
        }
        return batch.completed != 0;
      }
      bulk_receive_ = false;
    }
    auto burst = poll(false, nullptr);
    if (!burst) return false;
    if (completion_is_send(burst.get())) return true;
    if (receive) {
      for (int i = 0; i < packet_count(burst.get()); ++i) {
        const auto length = daqiri::get_packet_length(burst.get(), i);
        if (cfg_.tcp) {
          counts.bytes += length;
        } else if (matches_frame_length(length)) {
          counts.bytes += cfg_.application_bytes();
          ++counts.packets;
        } else {
          ++counts.errors;
        }
      }
      if (cfg_.tcp) counts.packets = counts.bytes / cfg_.bytes;
    }
    if (cfg_.roce) {
      // A completed single receive still owns its registered buffer. Post it again as
      // soon as it has been counted, including during the phase's unmeasured drain.
      if (daqiri::rdma_get_opcode(burst.get()) != RDMAOpCode::RECEIVE)
        throw std::runtime_error("bandwidth expected a RoCE SEND receive completion");
      check(daqiri::set_packet_lengths(burst.get(), 0, {cfg_.bytes}),
            "restore RoCE receive length");
      if (post_burst(burst.release(), 1, true, nullptr, nullptr, &counts.errors)) ++rx_posted_;
    }
    return true;
  }

  bool send_probe(uint64_t id, uint64_t deadline, uint64_t& start, daqiri::TxTimestampId* tx_id) {
    set_probe_id(id);
    do {
      if (cfg_.roce && tx_posted_ >= cfg_.tx_depth) poll(false, nullptr);
      if ((!cfg_.roce || tx_posted_ < cfg_.tx_depth) && submit(1, false, true, &start, tx_id))
        return true;
      std::this_thread::yield();
    } while (now_ns() < deadline);
    return false;
  }

  bool prepares_echo() const {
    return cfg_.raw && cfg_.rtt && cfg_.tx_direct && cfg_.rx_direct;
  }

  daqiri::bench::RdmaPingPong prepare_rdma_ping_pong() const {
    return daqiri::bench::RdmaPingPong(connection_, server, cfg_.bytes, cfg_.rx_depth,
                                       cfg_.opaque_payload, cfg_.timeout, tx_.front().name,
                                       rx_.front().name);
  }

  bool echo_prepared_probe(uint64_t id, uint64_t sequence, const Control& control) {
    set_probe_id(id);
    auto deleter = [this](BurstParams* burst) { free_unsent(burst); };
    std::unique_ptr<BurstParams, decltype(deleter)> reply(nullptr, deleter);
    const auto prepare_deadline = now_ns() + cfg_.timeout;
    do {
      reply.reset(prepare_burst(1, false, true, nullptr));
      if (reply) break;
      std::this_thread::yield();
    } while (now_ns() < prepare_deadline);
    if (!reply) throw std::runtime_error("latency echo buffer preparation timed out");

    control.send({sequence, 0, 0});
    const auto receive_deadline = now_ns() + cfg_.timeout;
    do {
      auto burst = poll(false, nullptr);
      if (!burst) continue;
      for (int i = 0; i < packet_count(burst.get()); ++i) {
        if (!matches_receive_metadata(burst.get(), i)) continue;
        if (!post_burst(reply.release(), 1, false, nullptr, nullptr, nullptr)) {
          throw std::runtime_error("failed to send prepared latency echo");
        }
        // Host-memory requests carry a token. Validate it after posting.
        if (!cfg_.opaque_payload && !matches_probe(burst.get(), i, id)) {
          throw std::runtime_error("prepared latency echo received a mismatched probe");
        }
        return true;
      }
    } while (now_ns() < receive_deadline);
    return false;
  }

  ReceiveTime receive_probe(uint64_t id, uint64_t deadline, bool timestamp) {
    do {
      ReceiveTime time{};
      auto burst = poll(true, &time.software);
      if (!burst) continue;
      if (completion_is_send(burst.get())) continue;
      for (int i = 0; i < packet_count(burst.get()); ++i) {
        if (!matches_probe(burst.get(), i, id)) continue;
        if (timestamp &&
            (daqiri::get_packet_rx_timestamp(burst.get(), i, &time.hardware) != Status::SUCCESS ||
             !time.hardware)) {
          throw TimestampUnavailable("hardware RX timestamp unavailable on " + name_);
        }
        burst.reset();
        refill();
        return time;
      }
      burst.reset();
      refill();
    } while (now_ns() < deadline);
    return {};
  }

  const bool server;
  const int queue;

 private:
  struct FreeRx {
    bool roce;
    void operator()(BurstParams* burst) const {
      if (roce)
        daqiri::free_tx_burst(burst);
      else
        daqiri::free_all_packets_and_burst_rx(burst);
    }
  };
  using RxBurst = std::unique_ptr<BurstParams, FreeRx>;

  RxBurst poll(bool timed, uint64_t* stamp) {
    BurstParams* burst = nullptr;
    const auto status = cfg_.raw ? daqiri::get_rx_burst(&burst, port_id_, qid_)
                                 : daqiri::get_rx_burst(&burst, connection_, server);
    if (timed && burst) *stamp = now_ns();
    RxBurst result(burst, FreeRx{cfg_.roce});
    if (status == Status::NOT_READY || (!cfg_.raw && !cfg_.roce && status == Status::NULL_PTR))
      return result;
    check(status, "poll RX");
    if (!burst) throw std::runtime_error("DAQIRI returned an empty burst pointer");
    if (cfg_.roce) {
      check(burst->transport_hdr.status, "RoCE completion");
      auto& posted = completion_is_send(burst) ? tx_posted_ : rx_posted_;
      posted -= packet_count(burst);
      if (posted < 0) throw std::runtime_error("unexpected RoCE completion count");
    }
    return result;
  }

  bool completion_is_send(BurstParams* burst) const {
    return cfg_.roce && daqiri::rdma_get_opcode(burst) == RDMAOpCode::SEND;
  }

  int packet_count(BurstParams* burst) const {
    return cfg_.roce ? static_cast<int>(burst->transport_hdr.num_pkts)
                     : static_cast<int>(daqiri::get_num_packets(burst));
  }

  bool matches_frame_length(uint32_t length) const {
    // A short UDP payload can arrive with minimum Ethernet frame padding.
    return length == static_cast<uint32_t>(cfg_.bytes) ||
           (cfg_.message_size && cfg_.bytes < ETH_ZLEN && length == ETH_ZLEN);
  }

  void set_probe_id(uint64_t id) {
    if (cfg_.opaque_payload) return;
    const uint64_t wire = htobe64(id);
    const int offset = cfg_.raw && !cfg_.named_sender ? kRawHeader : 0;
    std::memcpy(packet_.data() + offset, &wire, sizeof(wire));
  }

  bool matches_receive_metadata(BurstParams* burst, int index) const {
    return matches_frame_length(daqiri::get_packet_length(burst, index)) &&
           (!cfg_.opaque_payload || !rx_flow_id_ ||
            daqiri::get_packet_flow_id(burst, index) == rx_flow_id_);
  }

  bool matches_probe(BurstParams* burst, int index, uint64_t expected) {
    if (cfg_.opaque_payload) return matches_receive_metadata(burst, index);
    const int bytes = daqiri::get_packet_length(burst, index);
    if (cfg_.tcp) {
      auto* data = static_cast<const char*>(daqiri::get_segment_packet_ptr(burst, 0, index));
      int available = bytes;
      bool matches = false;
      while (available) {
        auto id = tcp_record_.consume(data, available, cfg_.bytes);
        matches = matches || (id && *id == expected);
      }
      return matches;
    }
    if (!matches_frame_length(bytes)) return false;
    uint64_t result = 0;
    auto* out = reinterpret_cast<char*>(&result);
    int offset = cfg_.raw ? kRawHeader : 0;
    int remaining = sizeof(result);
    for (size_t i = 0; i < rx_.size() && remaining; ++i) {
      const int length = daqiri::get_segment_packet_length(burst, i, index);
      if (offset >= length) {
        offset -= length;
        continue;
      }
      const int count = std::min(remaining, length - offset);
      auto* data = static_cast<char*>(daqiri::get_segment_packet_ptr(burst, i, index));
      std::memcpy(out, data + offset, count);
      out += count;
      remaining -= count;
      offset = 0;
    }
    return !remaining && be64toh(result) == expected;
  }

  void free_unsent(BurstParams* burst) const {
    if (cfg_.roce)
      daqiri::free_tx_burst(burst);
    else
      daqiri::free_all_packets_and_burst_tx(burst);
  }

  BurstParams* prepare_burst(int count, bool receive, bool probe, daqiri::TxTimestampId* id) {
    const auto& segments = receive ? rx_ : tx_;
    auto* burst = daqiri::create_tx_burst_params();
    if (!burst) return nullptr;
    try {
      if (cfg_.roce) {
        const auto op = receive ? RDMAOpCode::RECEIVE : RDMAOpCode::SEND;
        check(daqiri::rdma_set_header(burst, op, connection_, server, count, next_wr_,
                                      segments[0].name),
              "set RoCE header");
        // WQE IDs are unique within a connection, including multi-WQE bursts.
        next_wr_ += count;
      } else {
        daqiri::set_header(burst, port_id_, qid_, count, static_cast<int>(segments.size()));
        if (!cfg_.raw) daqiri::set_connection_id(burst, connection_);
      }
    } catch (...) {
      daqiri::free_tx_metadata(burst);
      throw;
    }
    if (!daqiri::is_tx_burst_available(burst)) {
      daqiri::free_tx_metadata(burst);
      return nullptr;
    }
    const auto allocation = daqiri::get_tx_packet_burst(burst);
    if (allocation != Status::SUCCESS) {
      daqiri::free_tx_metadata(burst);
      if (allocation != Status::NO_FREE_PACKET_BUFFERS &&
          allocation != Status::NO_FREE_BURST_BUFFERS && allocation != Status::NOT_READY)
        check(allocation, "allocate burst");
      return nullptr;
    }
    try {
      int offset = 0;
      std::array<int, daqiri::MAX_NUM_SEGS> lengths{};
      size_t index = 0;
      for (const auto& segment : segments) {
        const int bytes = std::min(cfg_.tx_buffer_bytes() - offset, segment.capacity);
        lengths[index++] = bytes;
        offset += bytes;
      }
      Status status = Status::INVALID_PARAMETER;
      if (cfg_.roce) {
        for (int i = 0; i < count; ++i)
          check(daqiri::set_packet_lengths(burst, i, {lengths[0]}), "set RoCE message length");
        status = Status::SUCCESS;
      } else
        switch (segments.size()) {
          case 1:
            status = daqiri::set_all_packet_lengths(burst, {lengths[0]});
            break;
          case 2:
            status = daqiri::set_all_packet_lengths(burst, {lengths[0], lengths[1]});
            break;
          case 3:
            status = daqiri::set_all_packet_lengths(burst, {lengths[0], lengths[1], lengths[2]});
            break;
          case 4:
            status = daqiri::set_all_packet_lengths(
                burst, {lengths[0], lengths[1], lengths[2], lengths[3]});
            break;
        }
      check(status, "set packet lengths");
      const int packets_to_initialize = receive || cfg_.prepared_device_frames ? 0 : count;
      for (int i = 0; i < packets_to_initialize; ++i) {
        int offset = 0;
        for (size_t j = 0; j < segments.size() && lengths[j]; ++j) {
          if (segments[j].device) {
            offset += lengths[j];
            continue;
          }
          auto* dest = daqiri::get_segment_packet_ptr(burst, j, i);
          // NIC-backed cyclic or pooled storage retains its template between
          // uses. The socket backend allocates fresh heap buffers per burst.
          if (probe || (!cfg_.raw && !cfg_.roce) || initialized_.insert(dest).second) {
            std::memcpy(dest, packet_.data() + offset, lengths[j]);
          }
          offset += lengths[j];
        }
      }
      if (id && daqiri::request_packet_tx_timestamp(burst, 0, id) != Status::SUCCESS) {
        throw TimestampUnavailable("TX CQE timestamp request failed on " + name_);
      }
    } catch (...) {
      free_unsent(burst);
      throw;
    }
    return burst;
  }

  bool post_burst(BurstParams* burst, int count, bool receive, uint64_t* stamp,
                  daqiri::TxTimestampId* id, uint64_t* errors) {
    if (stamp) *stamp = now_ns();
    const auto status =
        sender_id_ ? daqiri::send_tx_burst(sender_id_, qid_, burst) : daqiri::send_tx_burst(burst);
    if (status == Status::SUCCESS) {
      if (cfg_.roce && !receive) tx_posted_ += count;
      return true;
    }
    if (status == Status::NO_SPACE_AVAILABLE) {
      if (cfg_.roce && (receive ? cfg_.rx_direct : cfg_.tx_direct)) free_unsent(burst);
      if (id && *id) {
        daqiri::release_tx_timestamp(*id);
        *id = daqiri::INVALID_TX_TIMESTAMP_ID;
      }
      if (errors) ++*errors;
      return false;
    }
    if (cfg_.raw ||
        (cfg_.roce && (status == Status::NOT_READY || (receive ? cfg_.rx_direct : cfg_.tx_direct))))
      free_unsent(burst);
    check(status, sender_id_ ? "submit burst through named sender" : "submit burst");
    return false;
  }

  bool submit(int count, bool receive, bool probe, uint64_t* stamp, daqiri::TxTimestampId* id,
              uint64_t* errors = nullptr) {
    auto* burst = prepare_burst(count, receive, probe, id);
    return burst && post_burst(burst, count, receive, stamp, id, errors);
  }

  int qid_, cpu_, rx_app_cpu_;
  uint16_t server_port_, port_id_ = 0;
  daqiri::FlowId rx_flow_id_;
  const Settings& cfg_;
  std::string name_;
  std::optional<daqiri::RawUdpEndpointConfig> sender_config_;
  daqiri::EndpointId sender_id_ = daqiri::INVALID_ENDPOINT_ID;
  uintptr_t connection_ = 0;
  bool bulk_receive_ = true;
  std::vector<char> packet_;
  std::vector<Segment> tx_, rx_;
  std::unordered_set<void*> initialized_;
  daqiri::perftest::TcpRecord tcp_record_;
  int tx_posted_ = 0, rx_posted_ = 0;
  uint64_t next_wr_ = 1;
};

struct Phase {
  uint64_t start = 0;
  uint64_t duration = 0;
};

struct BandwidthResult {
  Counts tx, rx, rx_drain;
};

// App threads live across warmup, measurement and draining. Direct queues keep
// their first application owner even when throughput direction changes.
class Bandwidth {
 public:
  Bandwidth(const Settings& cfg, std::vector<Lane>& lanes)
      : cfg_(cfg), lanes_(lanes), results_(lanes.size()) {
    workers_.reserve(lanes.size() * (cfg.tcp && cfg.bidirectional ? 2 : 1));
    try {
      for (size_t i = 0; i < lanes.size(); ++i) {
        const bool tx = has_tx(lanes[i]);
        const bool rx = has_rx(lanes[i]);
        // A blocking TCP send must leave a reader running to release local
        // backpressure and make progress on the peer's writes.
        if (cfg.tcp && tx && rx) {
          workers_.emplace_back([this, i] { run_lane(i, true, false); });
          workers_.emplace_back([this, i] { run_lane(i, false, true); });
        } else {
          workers_.emplace_back([this, i, tx, rx] { run_lane(i, tx, rx); });
        }
      }
    } catch (...) {
      stop();
      throw;
    }
  }

  ~Bandwidth() {
    stop();
  }

  void ready() {
    wait_for_workers();
  }

  Phase run(uint64_t duration, bool measure) {
    const Phase phase{now_ns() + ns(100ms), duration};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      phase_ = phase;
      measure_ = measure;
      done_ = 0;
      ++generation_;
    }
    condition_.notify_all();
    wait_for_workers();
    return phase;
  }

  YAML::Node results() const {
    YAML::Node rows(YAML::NodeType::Sequence);
    for (size_t i = 0; i < lanes_.size(); ++i) {
      const auto& lane = lanes_[i];
      const auto& result = results_[i];
      if (has_tx(lane)) rows.push_back(row(lane, true, result.tx, {}));
      if (has_rx(lane)) rows.push_back(row(lane, false, result.rx, result.rx_drain));
    }
    return rows;
  }

 private:
  bool has_tx(const Lane& lane) const {
    return !lane.server || cfg_.bidirectional;
  }
  bool has_rx(const Lane& lane) const {
    return lane.server || cfg_.bidirectional;
  }

  static YAML::Node row(const Lane& lane, bool tx, Counts main, Counts drain) {
    YAML::Node row;
    row["direction"] = lane.server == tx ? "server->client" : "client->server";
    row["queue"] = lane.queue;
    row["kind"] = tx ? "submitted" : "received";
    row["packets"] = main.packets;
    row["bytes"] = main.bytes;
    row["errors"] = main.errors + drain.errors;
    row["drain_packets"] = drain.packets;
    row["drain_bytes"] = drain.bytes;
    return row;
  }

  void run_lane(size_t index, bool tx, bool rx) {
    auto& lane = lanes_[index];
    try {
      lane.bind_thread(cfg_.tcp && rx && cfg_.bidirectional);
      if (rx) lane.prime();
      unsigned generation = 0;
      while (true) {
        Phase phase;
        bool measure;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          ++done_;
          condition_.notify_all();
          condition_.wait(lock, [&] { return stopping_ || generation_ != generation; });
          if (stopping_) return;
          generation = generation_;
          phase = phase_;
          measure = measure_;
        }
        auto result = measure_lane(lane, phase, tx, rx);
        if (measure && tx) results_[index].tx = result.tx;
        if (measure && rx) {
          results_[index].rx = result.rx;
          results_[index].rx_drain = result.rx_drain;
        }
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!error_) error_ = std::current_exception();
      stopping_ = true;
      condition_.notify_all();
    }
  }

  BandwidthResult measure_lane(Lane& lane, Phase phase, bool tx, bool rx) const {
    BandwidthResult result;
    Counts before;
    while (!stopping_ && now_ns() < phase.start) {
      if (rx || cfg_.roce) lane.poll_bandwidth(before, false);
      if (rx) lane.refill();
      std::this_thread::yield();
    }
    const uint64_t end = phase.start + phase.duration;
    while (!stopping_ && now_ns() < end) {
      bool active = false;
      if (rx || cfg_.roce) active = lane.poll_bandwidth(result.rx, rx);
      if (rx) lane.refill();
      if (tx) active = lane.send_bandwidth(result.tx) || active;
      if (!active && !cfg_.roce) std::this_thread::yield();
    }
    while (!stopping_ && (rx || cfg_.roce) && now_ns() < end + ns(kDrainTime)) {
      const bool active = lane.poll_bandwidth(result.rx_drain, rx);
      if (rx) lane.refill();
      if (!active) std::this_thread::yield();
    }
    return result;
  }

  void wait_for_workers() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] { return error_ || done_ == workers_.size(); });
    if (error_) std::rethrow_exception(error_);
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  const Settings& cfg_;
  std::vector<Lane>& lanes_;
  std::vector<BandwidthResult> results_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<std::thread> workers_;
  size_t done_ = 0;
  unsigned generation_ = 0;
  std::atomic<bool> stopping_{false};
  bool measure_ = false;
  Phase phase_{};
  std::exception_ptr error_;
};

class TimestampRequest {
 public:
  ~TimestampRequest() {
    if (id) daqiri::release_tx_timestamp(id);
  }

  uint64_t read(uint64_t deadline) const {
    do {
      uint64_t time = 0;
      const auto status = daqiri::get_packet_tx_timestamp(id, &time);
      if (status == Status::SUCCESS && time) return time;
      if (status != Status::NOT_READY) break;
      std::this_thread::yield();
    } while (now_ns() < deadline);
    throw TimestampUnavailable("TX CQE timestamp unavailable");
  }

  daqiri::TxTimestampId id = daqiri::INVALID_TX_TIMESTAMP_ID;
};

struct Probe {
  uint64_t sequence, software;
  std::optional<int64_t> hardware;
};

int64_t signed_difference(uint64_t end, uint64_t start) {
  return end >= start ? static_cast<int64_t>(end - start) : -static_cast<int64_t>(start - end);
}

YAML::Node latency_result(const Lane& lane, int timeouts, const std::vector<Probe>& probes) {
  YAML::Node result;
  result["direction"] = lane.server ? "server->client" : "client->server";
  result["queue"] = lane.queue;
  result["timeouts"] = timeouts;
  YAML::Node rows(YAML::NodeType::Sequence);
  for (const auto& probe : probes) {
    YAML::Node row(YAML::NodeType::Sequence);
    row.SetStyle(YAML::EmitterStyle::Flow);
    row.push_back(probe.sequence);
    row.push_back(probe.software);
    if (probe.hardware)
      row.push_back(*probe.hardware);
    else
      row.push_back(YAML::Node(YAML::NodeType::Null));
    rows.push_back(row);
  }
  result["probes"] = rows;
  return result;
}

YAML::Node latency_phase(const Settings& cfg, Lane& lane, bool origin, const Control& control) {
  lane.bind_thread();
  std::vector<Probe> probes;
  if (origin) probes.reserve(cfg.samples);
  const uint64_t iterations = static_cast<uint64_t>(cfg.warmup_samples) + cfg.samples;
  if (cfg.direct_roce_rtt) {
    auto ping_pong = lane.prepare_rdma_ping_pong();
    control.barrier(5);
    for (uint64_t seq = 0; seq < iterations; ++seq) {
      const auto latency = ping_pong.probe(origin, seq, seq + 1 < iterations);
      if (origin && seq >= static_cast<uint64_t>(cfg.warmup_samples))
        probes.push_back({seq - cfg.warmup_samples, latency, std::nullopt});
    }
    control.barrier(7);
    return origin ? latency_result(lane, 0, probes) : YAML::Node{};
  }
  lane.prime();
  control.barrier(5);
  uint64_t nonce;
  const bool prepared_echo = !origin && lane.prepares_echo();
  if (origin) {
    std::random_device random;
    nonce = (static_cast<uint64_t>(random()) << 32) | random();
    control.send({nonce, 0, 0});
  } else {
    nonce = control.receive()[0];
  }
  int timeouts = 0;
  int consecutive_misses = 0;
  for (uint64_t seq = 0; seq < iterations; ++seq) {
    const bool measured = seq >= static_cast<uint64_t>(cfg.warmup_samples);
    const uint64_t probe_id = seq ^ nonce;
    if (!origin) {
      if (prepared_echo) {
        const bool received = lane.echo_prepared_probe(probe_id, seq, control);
        control.send({seq, received, 0});
        continue;
      }
      control.send({seq, 0, 0});
      const auto input =
          lane.receive_probe(probe_id, now_ns() + cfg.timeout, cfg.hardware && !cfg.hardware_rtt);
      uint64_t unused = 0;
      if (input.software && cfg.rtt &&
          !lane.send_probe(probe_id, now_ns() + cfg.timeout, unused, nullptr)) {
        throw std::runtime_error("failed to send latency echo");
      }
      control.send({seq, input.software, input.hardware});
      continue;
    }

    const auto ready = control.receive();
    if (ready[0] != seq) throw std::runtime_error("peer probe sequence mismatch");
    TimestampRequest request;
    uint64_t start = 0;
    const bool sent = lane.send_probe(probe_id, now_ns() + cfg.timeout, start,
                                      cfg.hardware ? &request.id : nullptr);
    ReceiveTime echo{};
    if (sent && cfg.rtt) {
      echo = lane.receive_probe(probe_id, now_ns() + cfg.timeout, cfg.hardware && cfg.hardware_rtt);
    }
    const auto rx = control.receive(cfg.timeout);
    if (rx[0] != seq) throw std::runtime_error("peer RX sequence mismatch");
    const auto finish = cfg.rtt ? echo.software : rx[1];
    if (!sent || !finish || !rx[1]) {
      if (cfg.opaque_payload) throw std::runtime_error("opaque latency probe timed out");
      if (measured) ++timeouts;
      if (++consecutive_misses == 3) throw std::runtime_error("three consecutive probe timeouts");
      continue;
    }
    consecutive_misses = 0;
    if (finish < start) throw std::runtime_error("software latency clocks are inconsistent");
    std::optional<int64_t> hardware;
    if (cfg.hardware) {
      const uint64_t receive_timestamp = cfg.hardware_rtt ? echo.hardware : rx[2];
      const uint64_t transmit_timestamp = request.read(now_ns() + cfg.timeout);
      const uint64_t span = receive_timestamp >= transmit_timestamp
                                ? receive_timestamp - transmit_timestamp
                                : transmit_timestamp - receive_timestamp;
      if (span > cfg.timeout)
        throw TimestampUnavailable("hardware timestamp interval exceeds the probe timeout");
      hardware = signed_difference(receive_timestamp, transmit_timestamp);
    }
    if (measured) probes.push_back({seq - cfg.warmup_samples, finish - start, hardware});
  }
  return origin ? latency_result(lane, timeouts, probes) : YAML::Node{};
}

YAML::Node latency_role(const Settings& cfg, std::vector<Lane>& lanes, bool server,
                        const Control& control) {
  YAML::Node rows(YAML::NodeType::Sequence);
  const int phases = cfg.bidirectional ? 2 : 1;
  for (int phase = 0; phase < phases; ++phase) {
    const bool origin = server == (phase == 1);
    for (auto& lane : lanes) {
      if (lane.server != server) continue;
      auto result = latency_phase(cfg, lane, origin, control);
      if (origin) rows.push_back(result);
    }
  }
  return rows;
}

YAML::Node latency(const Settings& cfg, std::vector<Lane>& lanes, const Control& control) {
  if (cfg.role != "local") return latency_role(cfg, lanes, cfg.role == "server", control);
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
    throw std::runtime_error("creating local benchmark control socket");
  }
  Fd server_fd{sockets[0]}, client_fd{sockets[1]};
  const Control server_control(server_fd.value, cfg.timeout * 2);
  const Control client_control(client_fd.value, cfg.timeout * 2);
  std::exception_ptr errors[2];
  YAML::Node server_rows, client_rows;
  auto run = [&](bool server, const Control& peer, YAML::Node& rows) {
    try {
      rows = latency_role(cfg, lanes, server, peer);
    } catch (const std::exception& error) {
      errors[server] = std::current_exception();
      std::cerr << (server ? "server" : "client") << " latency: " << error.what() << '\n';
      peer.abort();
    }
  };
  std::thread server(run, true, std::cref(server_control), std::ref(server_rows));
  run(false, client_control, client_rows);
  server.join();
  // The peer can see a closed control channel as a consequence of a timestamp
  // error. Preserve the original error so the launcher can rerun the case.
  for (const auto& error : errors) {
    if (!error) continue;
    try {
      std::rethrow_exception(error);
    } catch (const TimestampUnavailable&) {
      throw;
    } catch (...) {
    }
  }
  for (const auto& error : errors)
    if (error) std::rethrow_exception(error);
  for (const auto& row : server_rows) client_rows.push_back(row);
  return client_rows;
}

struct EngineSession {
  ~EngineSession() {
    daqiri::shutdown();
  }
};

YAML::Node benchmark(const char* input, const YAML::Node& document, const Settings& cfg,
                     const Control& control) {
  std::vector<Lane> lanes;
  for (const auto& lane : document["perftest"]["lanes"]) lanes.emplace_back(cfg, lane);
  // TCP clients connect while initializing. Start the remote listener first.
  if (cfg.role == "client" && control.receive()[0] != 1)
    throw std::runtime_error("peer did not initialize");
  EngineSession engine;
  daqiri::DeviceMemoryPrefixes prefixes;
  for (const auto& lane : lanes) lane.add_device_frame_prefix(prefixes);
  const auto status = daqiri::daqiri_init_from_yaml_file(input, {}, prefixes);
  if (status != Status::SUCCESS && cfg.hardware) {
    throw TimestampUnavailable("DAQIRI initialization failed with hardware timestamps enabled");
  }
  check(status, "DAQIRI initialization");
  if (cfg.role == "server") control.send({1, 0, 0});
  for (const bool server : {false, true}) {
    for (auto& lane : lanes) {
      if (lane.server == server) lane.connect();
    }
  }
  control.barrier(2);
  YAML::Node result;
  result["version"] = daqiri::version_string();
  result["role"] = cfg.role;
  if (cfg.latency) {
    result["latency"] = latency(cfg, lanes, control);
  } else {
    Bandwidth workers(cfg, lanes);
    workers.ready();
    control.barrier(3);
    if (cfg.warmup) workers.run(cfg.warmup, false);
    control.barrier(4);
    const auto window = workers.run(cfg.duration, true);
    control.barrier(6, ns(kDrainTime));
    result["window"]["start_ns"] = window.start;
    result["window"]["end_ns"] = window.start + window.duration;
    result["window"]["elapsed_ns"] = window.duration;
    result["bandwidth"] = workers.results();
  }
  if (cfg.roce) daqiri::print_stats();
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: daqiri_perftest_worker CONFIG RESULT CONTROL_FD\n";
    return 2;
  }
  const int fd = std::stoi(argv[3]);
  std::signal(SIGPIPE, SIG_IGN);
  try {
    const auto document = YAML::LoadFile(argv[1]);
    const Settings cfg(document["perftest"]);
    const Control control(fd, cfg.timeout * 2);
    // Stop-and-wait RoCE needs a completion for every probe.
    if (cfg.latency && cfg.roce) setenv("DAQIRI_RDMA_SEND_SIGNAL_EVERY", "1", 1);
    const auto result = benchmark(argv[1], document, cfg, control);
    std::ofstream output(argv[2]);
    output << result << '\n';
    output.close();
    if (!output) throw std::runtime_error("could not write benchmark result");
    return 0;
  } catch (const TimestampUnavailable& error) {
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    std::cerr << "WARNING: " << error.what() << '\n';
    return kTimestampUnavailable;
  } catch (const std::exception& error) {
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    std::cerr << "daqiri_perftest worker: " << error.what() << '\n';
    return 1;
  }
}
