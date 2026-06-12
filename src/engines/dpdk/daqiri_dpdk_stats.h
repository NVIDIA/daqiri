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

#include <daqiri/daqiri.h>
#include <rte_ethdev.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <array>
#include "src/metrics.h"

namespace daqiri {

class DpdkStats {
 public:
    DpdkStats() = default;
    void Run();
    ~DpdkStats() {
      // Free allocated memory for xstats
      for (auto& [port_id, port_stats] : xstats_) {
        if (port_stats.xstats) {
          free(port_stats.xstats);
        }
        if (port_stats.old_xstats) {
          free(port_stats.old_xstats);
        }
      }
    }

    // Method to stop the stats thread
    void Stop() {
      force_quit_.store(true);
    }

    void Init(const NetworkConfig &cfg);
    void Shutdown();

 private:
    // Structure to hold xstats data for a port
    struct PortXStats {
      int len;                           // Number of xstats
      struct rte_eth_xstat* xstats;      // Current xstats values
      struct rte_eth_xstat* old_xstats;  // Previous xstats values
      std::unordered_map<std::string, int> name_to_idx;  // Map from stat name to index
      int rx_missed_idx;                   // Index for rx_missed_idx counter
      int rx_mbuf_allocation_errors_idx;   // Index for rx_mbuf_allocation_errors_idx counter

      // Map from queue ID to rx_q*_errors index
      static constexpr int MAX_QUEUE_COUNT = 128;        // Maximum number of queues supported
      std::unordered_map<int, int> rx_queue_errors_idx;  // Indices for rx_q*_errors counters

      struct QueueXStats {
        int rx_packets_idx = -1;
        int tx_packets_idx = -1;
        int rx_bytes_idx = -1;
        int tx_bytes_idx = -1;
        int rx_errors_idx = -1;
      };
      std::unordered_map<int, QueueXStats> queue_xstats;

      PortXStats() : len(0), xstats(nullptr), old_xstats(nullptr),
                    rx_missed_idx(-1), rx_mbuf_allocation_errors_idx(-1) {
        // No need to initialize the map, it starts empty
      }
    };

    NetworkConfig cfg_;
    bool init_ = false;
    int core_;
    std::unordered_map<int, PortXStats> xstats_;  // Map from port_id to xstats
    std::atomic<bool> force_quit_ = false;
    static constexpr int POLLING_INTERVAL_MS = 500;

    // Map to store memory region names for each port/queue combination
    // Key: (port_id << 16) | queue_id, Value: comma-separated list of memory region names
    std::unordered_map<uint32_t, std::string> port_queue_memory_regions_;
    std::unordered_map<int, std::shared_ptr<metrics::CounterSet>> port_metrics_;
    std::unordered_map<uint32_t, std::shared_ptr<metrics::CounterSet>> queue_metrics_;
};

}  // namespace daqiri
