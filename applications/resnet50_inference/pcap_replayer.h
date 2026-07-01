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

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "app_config.h"

namespace daqiri::apps::resnet {

using PcapFrame = std::vector<uint8_t>;  // one full ETH/IP/UDP frame

// Minimal standard-pcap reader (little-endian global + record headers,
// linktype 1 / Ethernet). Loads every frame into memory for replay; the
// per-image data prep keeps pcaps small (a few hundred images).
class PcapReplayer {
 public:
  bool load(const std::string& path);
  const std::vector<PcapFrame>& frames() const {
    return frames_;
  }
  bool empty() const {
    return frames_.empty();
  }

 private:
  std::vector<PcapFrame> frames_;
};

// Build synthetic frames for the benchmark (no dataset): images_per_batch
// images worth of frames, each carrying a per-image sequence number (0 ..
// packets_per_image-1) at the payload start and arbitrary pixel bytes. The RX
// pipeline reassembles + infers identically to the dataset path; only the pixel
// content (and thus the feature values) is meaningless, which does not affect
// throughput.
std::vector<PcapFrame> build_synthetic_frames(const AppConfig& cfg);

// Replay `frames` into the DAQIRI TX path forever (or once if loop==false),
// patching each frame's destination MAC to `eth_dst_addr` at send time so one
// pcap works on any host. Mirrors the bench tx_worker burst/send sequence.
// Sets `tx_done` true on return (the whole dataset has been sent once, in
// replay-once mode) so the RX worker knows to drain the ring and stop.
void pcap_tx_worker(const AppConfig& cfg, const std::vector<PcapFrame>& frames,
                    const std::string& eth_dst_addr, bool loop, std::atomic<bool>& tx_done,
                    std::atomic<bool>& stop);

}  // namespace daqiri::apps::resnet
