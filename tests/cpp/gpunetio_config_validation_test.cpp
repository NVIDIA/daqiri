/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

// Checks, without a GPU or NIC, that the shared validation rejects the features the gpunetio
// engine does not support, and that the engine refuses caller-owned memory regions before it
// opens a device.

#include <cstdio>
#include <string>
#include <vector>

#include <daqiri/daqiri.h>

#include "src/engine.h"

namespace {

const char* const kBaseConfig = R"yaml(%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: "raw"
    engine: "gpunetio"
    master_core: 0
    log_level: "info"
    memory_regions:
    - name: "DATA_TX"
      kind: "device"
      affinity: 0
      num_bufs: 1024
      buf_size: 2048
    - name: "DATA_RX"
      kind: "device"
      affinity: 0
      num_bufs: 1024
      buf_size: 2048
    interfaces:
    - name: "port0"
      address: "0000:00:00.0"
      tx:
        queues:
        - name: "tx_q_0"
          id: 0
          cpu_core: 1
          batch_size: 256
          memory_regions:
          - "DATA_TX"
      rx:
        queues:
        - name: "rx_q_0"
          id: 0
          cpu_core: 2
          batch_size: 256
          memory_regions:
          - "DATA_RX"
        flows:
        - name: "flow_0"
          id: 1
          action:
            type: queue
            id: 0
          match:
            udp_src: 4096
            udp_dst: 4096
)yaml";

struct UnsupportedCase {
  const char* name;
  const char* from;
  const char* to;
};

const std::vector<UnsupportedCase> kUnsupportedCases = {
    {"header-data split", "          - \"DATA_RX\"\n",
     "          - \"DATA_RX\"\n          - \"DATA_TX\"\n"},
    {"host memory", "    - name: \"DATA_TX\"\n      kind: \"device\"\n",
     "    - name: \"DATA_TX\"\n      kind: \"host\"\n"},
    {"software loopback", "    log_level: \"info\"\n",
     "    log_level: \"info\"\n    loopback: \"sw\"\n"},
    {"TX offload", "          - \"DATA_TX\"\n",
     "          - \"DATA_TX\"\n          offloads:\n          - \"tx_eth_src\"\n"},
    {"packet pacing", "          - \"DATA_TX\"\n",
     "          - \"DATA_TX\"\n          pacing_mbps: 1000\n"},
    {"accurate send", "      tx:\n", "      tx:\n        accurate_send: true\n"},
    {"hardware timestamps", "      rx:\n", "      rx:\n        hardware_timestamps: true\n"},
    {"dynamic RX flows", "      rx:\n", "      rx:\n        dynamic_flow_capacity: 16\n"},
    {"eCPRI flow", "            udp_src: 4096\n            udp_dst: 4096\n",
     "            ecpri: {}\n"},
    // The region of an RX queue is its receive ring
    {"shared RX region", "          - \"DATA_RX\"\n", "          - \"DATA_TX\"\n"},
    // A TX queue hands out the packet slots of its whole region
    {"shared TX region", "          - \"DATA_TX\"\n      rx:\n",
     "          - \"DATA_TX\"\n        - name: \"tx_q_1\"\n          id: 1\n          cpu_core: 3\n"
     "          batch_size: 256\n          memory_regions:\n          - \"DATA_TX\"\n      rx:\n"},
    {"ring smaller than a batch", "      num_bufs: 1024\n      buf_size: 2048\n    interfaces:",
     "      num_bufs: 128\n      buf_size: 2048\n    interfaces:"},
};

// Replaces the first occurrence of from, which must exist
bool patch(std::string* yaml, const std::string& from, const std::string& to) {
  const size_t pos = yaml->find(from);
  if (pos == std::string::npos) {
    return false;
  }
  yaml->replace(pos, from.size(), to);
  return true;
}

bool parse(const std::string& yaml, daqiri::NetworkConfig* config) {
  return daqiri::parse_network_config_from_yaml_string(yaml, *config) == daqiri::Status::SUCCESS;
}

}  // namespace

int main() {
  int failures = 0;
  const auto expect = [&failures](bool condition, const std::string& what) {
    if (!condition) {
      std::fprintf(stderr, "FAILED: %s\n", what.c_str());
      failures++;
    }
  };

  daqiri::NetworkConfig base;
  expect(parse(kBaseConfig, &base), "base config parses");
  expect(base.common_.engine_type == daqiri::EngineType::GPUNETIO,
         "base config resolves to the gpunetio engine");
  expect(daqiri::validate_network_config(base), "base config is valid");

  expect(base.ifs_[0].tx_.queues_[0].gpunetio_tx_kernel_ == daqiri::GpunetioTxKernel::PERSISTENT,
         "TX queues default to the persistent kernel");

  for (const auto& unsupported : kUnsupportedCases) {
    std::string yaml = kBaseConfig;
    if (!patch(&yaml, unsupported.from, unsupported.to)) {
      expect(false, std::string(unsupported.name) + ": marker not found in the base config");
      continue;
    }
    daqiri::NetworkConfig config;
    expect(parse(yaml, &config), std::string(unsupported.name) + ": config parses");
    expect(!daqiri::validate_network_config(config),
           std::string(unsupported.name) + ": config is rejected");
  }

  // TX kernel option
  const std::string tx_kernel_from = "          - \"DATA_TX\"\n";
  std::string per_burst = kBaseConfig;
  expect(patch(&per_burst, tx_kernel_from,
               tx_kernel_from + "          gpunetio:\n            tx_kernel: \"per_burst\"\n"),
         "tx_kernel marker");
  daqiri::NetworkConfig per_burst_config;
  expect(parse(per_burst, &per_burst_config) &&
             per_burst_config.ifs_[0].tx_.queues_[0].gpunetio_tx_kernel_ ==
                 daqiri::GpunetioTxKernel::PER_BURST,
         "tx_kernel per_burst parses");
  std::string bad_kernel = kBaseConfig;
  expect(patch(&bad_kernel, tx_kernel_from,
               tx_kernel_from + "          gpunetio:\n            tx_kernel: \"sometimes\"\n"),
         "bad tx_kernel marker");
  daqiri::NetworkConfig bad_kernel_config;
  expect(!parse(bad_kernel, &bad_kernel_config), "an invalid tx_kernel is rejected");

  daqiri::MemoryRegionRequirements requirements;
  expect(
      daqiri::get_memory_region_requirements(base, requirements) == daqiri::Status::NOT_SUPPORTED,
      "memory region requirements are not supported");

  // Rejected before the engine is created, so no device is needed
  char buffer[64];
  const daqiri::MemoryRegionBindings bindings = {{"DATA_RX", {buffer, sizeof(buffer)}}};
  daqiri::NetworkConfig init_config = base;
  expect(daqiri::daqiri_init(init_config, bindings) == daqiri::Status::NOT_SUPPORTED,
         "caller-owned memory regions are not supported");

  return failures == 0 ? 0 : 1;
}
