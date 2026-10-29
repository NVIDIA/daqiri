// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <daqiri/daqiri.h>
#include <arpa/inet.h>
#include <endian.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {
namespace d = daqiri;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kBytes = 64;
constexpr uint32_t kOffset = kBytes;
constexpr uint32_t kDescriptorBytes = 24;

void check(d::Status status, const char* action) {
  if (status != d::Status::SUCCESS)
    throw std::runtime_error(std::string(action) + " status=" + std::to_string(int(status)));
}
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Control messages have an explicit wire layout; never send a native MR struct.
std::array<uint8_t, kDescriptorBytes> encode(const d::RdmaMemoryRegion& region) {
  std::array<uint8_t, kDescriptorBytes> wire{};
  const uint64_t address = htobe64(region.address), length = htobe64(region.length);
  const uint32_t key = htonl(region.rkey), access = htonl(region.access);
  std::memcpy(wire.data(), &address, 8);
  std::memcpy(wire.data() + 8, &length, 8);
  std::memcpy(wire.data() + 16, &key, 4);
  std::memcpy(wire.data() + 20, &access, 4);
  return wire;
}
d::RdmaMemoryRegion decode(const void* data) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t address, length;
  uint32_t key, access;
  std::memcpy(&address, bytes, 8);
  std::memcpy(&length, bytes + 8, 8);
  std::memcpy(&key, bytes + 16, 4);
  std::memcpy(&access, bytes + 20, 4);
  return {be64toh(address), be64toh(length), ntohl(key), ntohl(access)};
}

// One process owns one engine. On exceptions, shutdown tears down QPs before
// releasing their registered allocations and metadata pools. In-flight buffers
// must not be freed individually while the NIC can still access them.
class Session {
 public:
  Session(uintptr_t connection, bool server, bool direct_rx, bool direct_tx)
      : connection_(connection), server_(server), direct_rx_(direct_rx), direct_tx_(direct_tx) {}
  ~Session() {
    d::shutdown();
  }

  d::BurstParams* allocate(d::RDMAOpCode opcode, const char* region, uint32_t length) {
    auto* burst = d::create_tx_burst_params();
    require(burst != nullptr, "metadata allocation failed");
    owned_.insert(burst);
    check(d::rdma_set_header(burst, opcode, connection_, server_, 1, next_id_++, region), "header");
    check(d::get_tx_packet_burst(burst), "packet allocation");
    check(d::set_packet_lengths(burst, 0, {static_cast<int>(length)}), "packet length");
    return burst;
  }
  uint64_t submit(d::BurstParams* burst) {
    require(owned_.count(burst) != 0, "submission does not own its burst");
    const uint64_t id = burst->transport_hdr.wr_id;
    const bool direct =
        burst->transport_hdr.opcode == d::RDMAOpCode::RECEIVE ? direct_rx_ : direct_tx_;
    const auto status = d::send_tx_burst(burst);
    if (status == d::Status::SUCCESS || (status == d::Status::NO_SPACE_AVAILABLE && !direct))
      owned_.erase(burst);
    check(status, "submit");
    pending_.emplace(id, burst);
    return id;
  }
  d::BurstParams* wait(uint64_t id) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!ready_.count(id)) {
      require(Clock::now() < deadline, "completion timed out");
      d::BurstParams* done = nullptr;
      const auto status = d::get_rx_burst(&done, connection_, server_);
      if (status == d::Status::NOT_READY) {
        std::this_thread::yield();
        continue;
      }
      check(status, "poll completion");
      const auto found = pending_.find(done->transport_hdr.wr_id);
      require(found != pending_.end() && found->second == done, "unexpected completion");
      pending_.erase(found);
      owned_.insert(done);
      check(done->transport_hdr.status, "operation completion");
      ready_.emplace(done->transport_hdr.wr_id, done);
    }
    auto* result = ready_.at(id);
    ready_.erase(id);
    return result;
  }
  void release(d::BurstParams* burst) {
    require(owned_.erase(burst) == 1, "free does not own its burst");
    d::free_tx_burst(burst);
  }
  void finish() {
    require(pending_.empty() && ready_.empty(), "unconsumed completions at shutdown");
    while (!owned_.empty()) release(*owned_.begin());
  }

 private:
  uintptr_t connection_;
  bool server_, direct_rx_, direct_tx_;
  uint64_t next_id_ = 1;
  std::unordered_set<d::BurstParams*> owned_;
  std::unordered_map<uint64_t, d::BurstParams*> pending_, ready_;
};

void send_control(Session& session, const void* data, uint32_t length) {
  auto* message = session.allocate(d::RDMAOpCode::SEND, "control", length);
  std::memcpy(d::get_packet_ptr(message, 0), data, length);
  session.wait(session.submit(message));
  session.release(message);
}
void verify(const void* data, uint8_t value) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  require(std::all_of(bytes, bytes + kBytes, [value](uint8_t byte) { return byte == value; }),
          "received payload differs from the written pattern");
}

void server(Session& session, uintptr_t connection) {
  // Reserve a target slot from its pool without posting it on the receive queue.
  auto* target = session.allocate(d::RDMAOpCode::RECEIVE, "target", 2 * kBytes);
  auto* destination = static_cast<uint8_t*>(d::get_packet_ptr(target, 0));
  std::memset(destination, 0, 2 * kBytes);
  d::RdmaMemoryRegion region{};
  check(d::rdma_get_local_memory_region(connection, "target", &region), "export MR");
  region.address = reinterpret_cast<uintptr_t>(destination);
  region.length = 2 * kBytes;

  // This RECEIVE supplies a notification credit. The RDMA payload bypasses it.
  auto* notification = session.allocate(d::RDMAOpCode::RECEIVE, "control", kBytes);
  std::memset(d::get_packet_ptr(notification, 0), 0xd3, kBytes);
  const auto notification_id = session.submit(notification);
  const auto wire = encode(region);
  send_control(session, wire.data(), wire.size());

  auto* done = session.wait(notification_id);
  uint32_t offset = 0, written = 0;
  check(d::rdma_get_immediate(done, &offset, &written), "WRITE_IMM notification");
  require(offset == kOffset && written == kBytes, "unexpected write offset or length");
  require(d::get_packet_length(done, 0) == 0, "notification buffer reported payload bytes");
  verify(d::get_packet_ptr(done, 0), 0xd3);
  verify(destination, 0x31);
  verify(destination + offset, 0xa5);
  session.release(done);

  // The protocol ends after this acknowledgment; the client issues no more DMA.
  const uint32_t acknowledgment = htonl(kOffset);
  send_control(session, &acknowledgment, sizeof(acknowledgment));
  session.release(target);
  session.finish();
  std::cout << "PASS: server verified WRITE and WRITE_IMM, offset=" << offset
            << " bytes=" << written << " notification buffer unchanged\n";
}

void client(Session& session) {
  auto* message = session.allocate(d::RDMAOpCode::RECEIVE, "control", kDescriptorBytes);
  session.wait(session.submit(message));
  require(d::get_packet_length(message, 0) == kDescriptorBytes, "bad descriptor message size");
  const auto remote = decode(d::get_packet_ptr(message, 0));
  require(remote.length >= 2 * kBytes, "exported target is too small");
  session.release(message);

  // WRITE copies the local source into remote slot 0. Only the sender completes.
  auto* write = session.allocate(d::RDMAOpCode::RDMA_WRITE, "io", kBytes);
  std::memset(d::get_packet_ptr(write, 0), 0x31, kBytes);
  check(d::rdma_set_remote_memory(write, remote, 0), "WRITE target");
  session.wait(session.submit(write));
  session.release(write);

  // READ fetches slot 0 back into a local registered destination.
  auto* read = session.allocate(d::RDMAOpCode::RDMA_READ, "io", kBytes);
  std::memset(d::get_packet_ptr(read, 0), 0, kBytes);
  check(d::rdma_set_remote_memory(read, remote, 0), "READ source");
  session.wait(session.submit(read));
  verify(d::get_packet_ptr(read, 0), 0x31);
  session.release(read);

  // Supply a credit for the server's acknowledgment before notifying it.
  auto* ack = session.allocate(d::RDMAOpCode::RECEIVE, "control", sizeof(uint32_t));
  const auto ack_id = session.submit(ack);
  auto* notified = session.allocate(d::RDMAOpCode::RDMA_WRITE_IMM, "io", kBytes);
  std::memset(d::get_packet_ptr(notified, 0), 0xa5, kBytes);
  check(d::rdma_set_remote_memory(notified, remote, kOffset), "WRITE_IMM target");
  // Immediate data is a host-order application value, not an automatic address.
  check(d::rdma_set_immediate(notified, kOffset), "WRITE_IMM application offset");
  session.wait(session.submit(notified));
  session.release(notified);
  session.wait(ack_id);
  require(d::get_packet_length(ack, 0) == sizeof(uint32_t), "bad acknowledgment size");
  uint32_t acknowledged = 0;
  std::memcpy(&acknowledged, d::get_packet_ptr(ack, 0), sizeof(acknowledged));
  require(ntohl(acknowledged) == kOffset, "server did not acknowledge the target slot");
  session.release(ack);
  session.finish();
  std::cout << "PASS: client verified WRITE -> READ and WRITE_IMM acknowledgment\n";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: daqiri_example_rdma_read_write CONFIG.yaml --mode server|client\n";
    return 0;
  }
  if (argc != 4 || std::string(argv[2]) != "--mode" ||
      (std::string(argv[3]) != "server" && std::string(argv[3]) != "client"))
    return 2;
  bool initialized = false;
  try {
    const char* signal_every = std::getenv("DAQIRI_RDMA_SEND_SIGNAL_EVERY");
    require(!signal_every || std::string(signal_every) == "1",
            "example requires DAQIRI_RDMA_SEND_SIGNAL_EVERY=1 (or unset)");
    const bool is_server = std::string(argv[3]) == "server";
    d::NetworkConfig config;
    check(d::parse_network_config_from_yaml_file(argv[1], config), "parse config");
    const auto remote = std::find_if(config.ifs_.begin(), config.ifs_.end(), [](const auto& intf) {
      return intf.rdma_.mode_ == d::RDMAMode::SERVER;
    });
    require(remote != config.ifs_.end(), "config needs a server interface");
    const std::string server_address = remote->address_;
    const uint16_t port = remote->rdma_.port_;
    const auto role = is_server ? d::RDMAMode::SERVER : d::RDMAMode::CLIENT;
    config.ifs_.erase(std::remove_if(config.ifs_.begin(), config.ifs_.end(),
                                     [role](const auto& intf) { return intf.rdma_.mode_ != role; }),
                      config.ifs_.end());
    require(config.ifs_.size() == 1, "config needs exactly one interface per role");
    const auto& intf = config.ifs_.front();
    require(intf.rdma_.xmode_ == d::RDMATransportMode::RC && intf.rx_.queues_.size() == 1 &&
                intf.tx_.queues_.size() == 1,
            "example requires RC and one RX/TX queue pair");
    const bool direct_rx = intf.rx_.queues_.front().poll_mode_ == d::QueuePollMode::DIRECT;
    const bool direct_tx = intf.tx_.queues_.front().poll_mode_ == d::QueuePollMode::DIRECT;
    for (const auto& requirement : {std::make_pair("control", kBytes), std::make_pair("io", kBytes),
                                    std::make_pair("target", 2 * kBytes)}) {
      const auto mr = config.mrs_.find(requirement.first);
      require(mr != config.mrs_.end() && mr->second.buf_size_ >= requirement.second &&
                  (mr->second.kind_ == d::MemoryKind::HOST ||
                   mr->second.kind_ == d::MemoryKind::HOST_PINNED ||
                   mr->second.kind_ == d::MemoryKind::HUGE),
              "example requires CPU-accessible control/io/target regions of sufficient size");
    }
    require(config.mrs_.at("control").num_bufs_ >= 2, "control region needs at least two slots");
    check(d::daqiri_init(config), "initialize");
    initialized = true;
    uintptr_t connection = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!connection) {
      require(Clock::now() < deadline, "connection timed out");
      const auto status =
          is_server ? d::rdma_get_server_conn_id(server_address, port, &connection)
                    : d::rdma_connect_to_server(server_address, port, intf.address_, &connection);
      if (status != d::Status::SUCCESS) {
        require(status == d::Status::NOT_READY || status == d::Status::CONNECT_FAILURE,
                "cannot establish RC connection");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    Session session(connection, is_server, direct_rx, direct_tx);
    initialized = false;  // Session now owns shutdown, including exception paths.
    if (is_server)
      server(session, connection);
    else
      client(session);
    return 0;
  } catch (const std::exception& error) {
    if (initialized) d::shutdown();
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
