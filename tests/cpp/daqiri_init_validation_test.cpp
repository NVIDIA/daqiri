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

#include <string>

#include <daqiri/common.h>

int main() {
  const std::string config = R"yaml(%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: socket
    master_core: 0
    memory_regions:
      - name: DATA
        kind: host
        affinity: 0
        num_bufs: 1
        buf_size: 2048
    interfaces:
      - name: udp
        address: 127.0.0.1
        socket_config:
          mode: client
          remote_addr: udp://127.0.0.1:9
          max_payload_size: 2048
        rx:
          queues:
            - name: RX
              id: 0
              cpu_core: -1
              batch_size: 1
              memory_regions:
                - MISSING
)yaml";

  const daqiri::Status status = daqiri::daqiri_init_from_yaml_string(config);
  if (status == daqiri::Status::SUCCESS) {
    daqiri::shutdown();
    return 1;
  }
  return status == daqiri::Status::INTERNAL_ERROR ? 0 : 1;
}
