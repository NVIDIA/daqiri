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

#pragma once

#include <memory>
#include <stdint.h>
#include <string>

#ifndef DAQIRI_ENABLE_OTEL_METRICS
#define DAQIRI_ENABLE_OTEL_METRICS 0
#endif

namespace daqiri::metrics {

#if DAQIRI_ENABLE_OTEL_METRICS

struct CounterSet;

std::shared_ptr<CounterSet> get_or_create_queue(const std::string& backend,
                                                const std::string& interface_name,
                                                uint16_t port_id,
                                                const std::string& queue_id);

void add_rx(const std::shared_ptr<CounterSet>& counters, uint64_t packets, uint64_t bytes);
void add_tx(const std::shared_ptr<CounterSet>& counters, uint64_t packets, uint64_t bytes);
void add_dropped(const std::shared_ptr<CounterSet>& counters,
                 const std::string& reason,
                 uint64_t packets);
void set_rx_packets(const std::shared_ptr<CounterSet>& counters, uint64_t packets);
void set_tx_packets(const std::shared_ptr<CounterSet>& counters, uint64_t packets);
void set_rx_bytes(const std::shared_ptr<CounterSet>& counters, uint64_t bytes);
void set_tx_bytes(const std::shared_ptr<CounterSet>& counters, uint64_t bytes);
void set_dropped(const std::shared_ptr<CounterSet>& counters,
                 const std::string& reason,
                 uint64_t packets);
void shutdown();

#else

struct CounterSet {};

inline std::shared_ptr<CounterSet> get_or_create_queue(const std::string&,
                                                       const std::string&,
                                                       uint16_t,
                                                       const std::string&) {
  return nullptr;
}

inline void add_rx(const std::shared_ptr<CounterSet>&, uint64_t, uint64_t) {}
inline void add_tx(const std::shared_ptr<CounterSet>&, uint64_t, uint64_t) {}
inline void add_dropped(const std::shared_ptr<CounterSet>&, const std::string&, uint64_t) {}
inline void set_rx_packets(const std::shared_ptr<CounterSet>&, uint64_t) {}
inline void set_tx_packets(const std::shared_ptr<CounterSet>&, uint64_t) {}
inline void set_rx_bytes(const std::shared_ptr<CounterSet>&, uint64_t) {}
inline void set_tx_bytes(const std::shared_ptr<CounterSet>&, uint64_t) {}
inline void set_dropped(const std::shared_ptr<CounterSet>&, const std::string&, uint64_t) {}
inline void shutdown() {}

#endif

}  // namespace daqiri::metrics
