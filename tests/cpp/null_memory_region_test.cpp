// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0

#include <daqiri/daqiri.h>

#include <iostream>
#include <stdexcept>
#include <string>

#include "src/engine.h"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::string yaml(const std::string& scatter, const std::string& ignored = "",
                 const std::string& kind = "\"null\"") {
  return R"yaml(daqiri:
  cfg:
    version: 1
    stream_type: raw
    engine: ibverbs
    master_core: 0
    memory_regions:
      - name: DISCARD
        kind: )yaml" +
         kind + "\n" + ignored + R"yaml(      - name: DATA
        kind: host
        affinity: 0
        num_bufs: 256
        buf_size: 4096
    interfaces:
      - name: test
        address: "0000:01:00.0"
        rx:
          queues:
            - name: RX
              id: 0
              cpu_core: 1
              batch_size: 1
              memory_regions: )yaml" +
         scatter + "\n";
}

daqiri::NetworkConfig parse(const std::string& text) {
  daqiri::NetworkConfig config{};
  require(daqiri::parse_network_config_from_yaml_string(text, config) == daqiri::Status::SUCCESS,
          "could not parse null region");
  return config;
}

void run() {
  using namespace daqiri;
  // Ignore even values of the wrong type, without requiring any allocation fields.
  auto config = parse(yaml("[{name: DISCARD, length: 42}, DATA]",
                           "        buf_size: invalid\n        num_bufs: -1\n"
                           "        affinity: [invalid]\n        access: invalid\n"));
  require(validate_network_config(config), "valid leading null rejected");
  const auto& discard = config.mrs_.at("DISCARD");
  require(discard.kind_ == MemoryKind::NULL_MR && discard.buf_size_ == 0 &&
              discard.num_bufs_ == 0 && discard.access_ == 0 && discard.affinity_ == 0,
          "null region allocation fields were not ignored");
  auto& queue = config.ifs_[0].rx_.queues_[0].common_;
  require(queue.mr_lengths_ == std::vector<uint32_t>({42, 0}), "scatter length lost");
  MemoryRegionRequirements requirements;
  require(get_memory_region_requirements(config, requirements) == Status::SUCCESS,
          "requirements failed");
  const auto& req = requirements.at("DISCARD");
  require(req.kind == MemoryKind::NULL_MR && req.capacity == 0 && req.num_bufs == 0 &&
              req.slot_size == 0 && req.alignment == 0,
          "null region requires backing memory");
  require(validate_network_config(parse(yaml("[DATA, DISCARD]"))), "tail discard rejected");
  require(validate_network_config(parse(yaml("[DISCARD]", "", "null"))),
          "unquoted null or discard-only queue rejected");
  require(validate_network_config(parse(yaml("[{name: DISCARD, length: 42}, DATA, DISCARD]"))),
          "same null region at different scatter positions rejected");

  queue.mr_lengths_.clear();
  require(!validate_network_config(config), "unbounded leading null accepted");
  queue.mr_lengths_ = {42};
  require(!validate_network_config(config), "mismatched length count accepted");
  queue.mr_lengths_ = {42, 100};
  require(!validate_network_config(config), "length on real memory accepted");
  queue.mr_lengths_ = {42, 0};
  TxQueueConfig tx{};
  tx.common_ = queue;
  config.ifs_[0].tx_.queues_ = {tx};
  require(!validate_network_config(config), "null TX accepted");
  config.ifs_[0].tx_.queues_.clear();
  config.common_.engine_type = EngineType::DPDK;
  require(!validate_network_config(config), "null DPDK accepted");
  config.common_.engine_type = EngineType::SOCKET;
  config.common_.stream_type = StreamType::SOCKET;
  require(!validate_network_config(config), "null socket accepted");

  for (const auto* entry : {"{name: DISCARD, length: 0}", "{name: DISCARD, length: -1}",
                            "{name: DISCARD, length: 4294967296}",
                            "{name: DISCARD, length: 42, typo: true}", "{name: DISCARD}"}) {
    NetworkConfig invalid{};
    require(parse_network_config_from_yaml_string(yaml(std::string("[") + entry + ", DATA]"),
                                                  invalid) != Status::SUCCESS,
            "malformed scatter entry accepted");
  }
}
}  // namespace

int main() {
  try {
    run();
    std::cout << "PASS: null memory region parsing, sizing, and validation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
