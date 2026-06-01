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

#include "metrics.h"

#if DAQIRI_ENABLE_OTEL_METRICS

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/metrics/async_instruments.h>
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/observer_result.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/nostd/variant.h>

namespace daqiri::metrics {

namespace {

namespace otel = opentelemetry;

using IntObserver =
    otel::nostd::shared_ptr<otel::metrics::ObserverResultT<int64_t>>;

struct DropCounter {
  std::string reason;
  std::atomic<uint64_t> packets{0};
};

struct QueueLabels {
  std::string backend;
  std::string interface_name;
  std::string port_id;
  std::string queue_id;
};

enum class MetricKind { RX_PACKETS, TX_PACKETS, RX_BYTES, TX_BYTES, DROPPED_PACKETS };

std::string normalize_interface_name(const std::string& interface_name, uint16_t port_id) {
  if (!interface_name.empty()) { return interface_name; }
  return "port" + std::to_string(port_id);
}

std::string make_key(const std::string& backend,
                     const std::string& interface_name,
                     uint16_t port_id,
                     const std::string& queue_id) {
  return backend + "|" + interface_name + "|" + std::to_string(port_id) + "|" + queue_id;
}

otel::nostd::string_view view(const std::string& value) {
  return otel::nostd::string_view(value.data(), value.size());
}

template <size_t N>
void observe(IntObserver observer,
             uint64_t value,
             const std::array<std::pair<otel::nostd::string_view,
                                        otel::common::AttributeValue>, N>& attrs) {
  if (observer == nullptr) { return; }
  observer->Observe(static_cast<int64_t>(value), attrs);
}

}  // namespace

struct CounterSet {
  QueueLabels labels;
  std::atomic<uint64_t> rx_packets{0};
  std::atomic<uint64_t> tx_packets{0};
  std::atomic<uint64_t> rx_bytes{0};
  std::atomic<uint64_t> tx_bytes{0};
  std::mutex drops_mutex;
  std::unordered_map<std::string, std::shared_ptr<DropCounter>> drops;

  void observe_drops(IntObserver observer) {
    std::vector<std::shared_ptr<DropCounter>> snapshot;
    {
      std::lock_guard<std::mutex> guard(drops_mutex);
      snapshot.reserve(drops.size());
      for (const auto& [reason, drop] : drops) {
        (void)reason;
        snapshot.push_back(drop);
      }
    }

    for (const auto& drop : snapshot) {
      if (drop == nullptr) { continue; }
      std::array<std::pair<otel::nostd::string_view, otel::common::AttributeValue>, 5> attrs = {{
          {"daqiri.backend", view(labels.backend)},
          {"daqiri.interface.name", view(labels.interface_name)},
          {"daqiri.port.id", view(labels.port_id)},
          {"daqiri.queue.id", view(labels.queue_id)},
          {"daqiri.drop.reason", view(drop->reason)},
      }};
      observe(observer, drop->packets.load(std::memory_order_relaxed), attrs);
    }
  }

  std::shared_ptr<DropCounter> get_drop_counter(const std::string& reason) {
    std::lock_guard<std::mutex> guard(drops_mutex);
    auto it = drops.find(reason);
    if (it != drops.end()) { return it->second; }

    auto drop = std::make_shared<DropCounter>();
    drop->reason = reason;
    drops.emplace(reason, drop);
    return drop;
  }
};

namespace {

class Registry {
 public:
  std::shared_ptr<CounterSet> get_or_create_queue(const std::string& backend,
                                                  const std::string& interface_name,
                                                  uint16_t port_id,
                                                  const std::string& queue_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    initialize_locked();

    const auto normalized_name = normalize_interface_name(interface_name, port_id);
    const auto key = make_key(backend, normalized_name, port_id, queue_id);
    auto it = by_key_.find(key);
    if (it != by_key_.end()) { return it->second; }

    auto counters = std::make_shared<CounterSet>();
    counters->labels.backend = backend;
    counters->labels.interface_name = normalized_name;
    counters->labels.port_id = std::to_string(port_id);
    counters->labels.queue_id = queue_id;

    by_key_.emplace(key, counters);
    counters_.push_back(counters);
    return counters;
  }

  void shutdown() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (rx_packets_) { rx_packets_->RemoveCallback(&Registry::observe_rx_packets, this); }
    if (tx_packets_) { tx_packets_->RemoveCallback(&Registry::observe_tx_packets, this); }
    if (rx_bytes_) { rx_bytes_->RemoveCallback(&Registry::observe_rx_bytes, this); }
    if (tx_bytes_) { tx_bytes_->RemoveCallback(&Registry::observe_tx_bytes, this); }
    if (dropped_packets_) {
      dropped_packets_->RemoveCallback(&Registry::observe_dropped_packets, this);
    }

    rx_packets_.reset();
    tx_packets_.reset();
    rx_bytes_.reset();
    tx_bytes_.reset();
    dropped_packets_.reset();
    counters_.clear();
    by_key_.clear();
    initialized_ = false;
  }

 private:
  void initialize_locked() {
    if (initialized_) { return; }

    auto provider = otel::metrics::Provider::GetMeterProvider();
    auto meter = provider->GetMeter("daqiri", "0.1.0");
    rx_packets_ = meter->CreateInt64ObservableCounter(
        "daqiri.rx.packets", "Packets received by DAQIRI", "{packet}");
    tx_packets_ = meter->CreateInt64ObservableCounter(
        "daqiri.tx.packets", "Packets transmitted by DAQIRI", "{packet}");
    rx_bytes_ = meter->CreateInt64ObservableCounter(
        "daqiri.rx.bytes", "Bytes received by DAQIRI", "By");
    tx_bytes_ = meter->CreateInt64ObservableCounter(
        "daqiri.tx.bytes", "Bytes transmitted by DAQIRI", "By");
    dropped_packets_ = meter->CreateInt64ObservableCounter(
        "daqiri.dropped.packets", "Packets dropped by DAQIRI or the active backend", "{packet}");

    rx_packets_->AddCallback(&Registry::observe_rx_packets, this);
    tx_packets_->AddCallback(&Registry::observe_tx_packets, this);
    rx_bytes_->AddCallback(&Registry::observe_rx_bytes, this);
    tx_bytes_->AddCallback(&Registry::observe_tx_bytes, this);
    dropped_packets_->AddCallback(&Registry::observe_dropped_packets, this);
    initialized_ = true;
  }

  static void observe_rx_packets(otel::metrics::ObserverResult result, void* state) noexcept {
    static_cast<Registry*>(state)->observe_counter(result, MetricKind::RX_PACKETS);
  }

  static void observe_tx_packets(otel::metrics::ObserverResult result, void* state) noexcept {
    static_cast<Registry*>(state)->observe_counter(result, MetricKind::TX_PACKETS);
  }

  static void observe_rx_bytes(otel::metrics::ObserverResult result, void* state) noexcept {
    static_cast<Registry*>(state)->observe_counter(result, MetricKind::RX_BYTES);
  }

  static void observe_tx_bytes(otel::metrics::ObserverResult result, void* state) noexcept {
    static_cast<Registry*>(state)->observe_counter(result, MetricKind::TX_BYTES);
  }

  static void observe_dropped_packets(otel::metrics::ObserverResult result,
                                      void* state) noexcept {
    static_cast<Registry*>(state)->observe_counter(result, MetricKind::DROPPED_PACKETS);
  }

  std::vector<std::shared_ptr<CounterSet>> snapshot_counters() {
    std::lock_guard<std::mutex> guard(mutex_);
    return counters_;
  }

  void observe_counter(otel::metrics::ObserverResult result, MetricKind kind) noexcept {
    auto observer_ptr = otel::nostd::get_if<IntObserver>(&result);
    if (observer_ptr == nullptr || *observer_ptr == nullptr) { return; }

    try {
      const auto snapshot = snapshot_counters();
      for (const auto& counters : snapshot) {
        if (counters == nullptr) { continue; }

        if (kind == MetricKind::DROPPED_PACKETS) {
          counters->observe_drops(*observer_ptr);
          continue;
        }

        uint64_t value = 0;
        switch (kind) {
          case MetricKind::RX_PACKETS:
            value = counters->rx_packets.load(std::memory_order_relaxed);
            break;
          case MetricKind::TX_PACKETS:
            value = counters->tx_packets.load(std::memory_order_relaxed);
            break;
          case MetricKind::RX_BYTES:
            value = counters->rx_bytes.load(std::memory_order_relaxed);
            break;
          case MetricKind::TX_BYTES:
            value = counters->tx_bytes.load(std::memory_order_relaxed);
            break;
          case MetricKind::DROPPED_PACKETS:
            break;
        }

        std::array<std::pair<otel::nostd::string_view, otel::common::AttributeValue>, 4> attrs = {{
            {"daqiri.backend", view(counters->labels.backend)},
            {"daqiri.interface.name", view(counters->labels.interface_name)},
            {"daqiri.port.id", view(counters->labels.port_id)},
            {"daqiri.queue.id", view(counters->labels.queue_id)},
        }};
        observe(*observer_ptr, value, attrs);
      }
    } catch (...) {
      return;
    }
  }

  bool initialized_ = false;
  std::mutex mutex_;
  std::vector<std::shared_ptr<CounterSet>> counters_;
  std::unordered_map<std::string, std::shared_ptr<CounterSet>> by_key_;
  otel::nostd::shared_ptr<otel::metrics::ObservableInstrument> rx_packets_;
  otel::nostd::shared_ptr<otel::metrics::ObservableInstrument> tx_packets_;
  otel::nostd::shared_ptr<otel::metrics::ObservableInstrument> rx_bytes_;
  otel::nostd::shared_ptr<otel::metrics::ObservableInstrument> tx_bytes_;
  otel::nostd::shared_ptr<otel::metrics::ObservableInstrument> dropped_packets_;
};

Registry& registry() {
  static Registry registry;
  return registry;
}

}  // namespace

std::shared_ptr<CounterSet> get_or_create_queue(const std::string& backend,
                                                const std::string& interface_name,
                                                uint16_t port_id,
                                                const std::string& queue_id) {
  return registry().get_or_create_queue(backend, interface_name, port_id, queue_id);
}

void add_rx(const std::shared_ptr<CounterSet>& counters, uint64_t packets, uint64_t bytes) {
  if (counters == nullptr) { return; }
  counters->rx_packets.fetch_add(packets, std::memory_order_relaxed);
  counters->rx_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void add_tx(const std::shared_ptr<CounterSet>& counters, uint64_t packets, uint64_t bytes) {
  if (counters == nullptr) { return; }
  counters->tx_packets.fetch_add(packets, std::memory_order_relaxed);
  counters->tx_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void add_dropped(const std::shared_ptr<CounterSet>& counters,
                 const std::string& reason,
                 uint64_t packets) {
  if (counters == nullptr || packets == 0) { return; }
  counters->get_drop_counter(reason)->packets.fetch_add(packets, std::memory_order_relaxed);
}

void set_rx_packets(const std::shared_ptr<CounterSet>& counters, uint64_t packets) {
  if (counters == nullptr) { return; }
  counters->rx_packets.store(packets, std::memory_order_relaxed);
}

void set_tx_packets(const std::shared_ptr<CounterSet>& counters, uint64_t packets) {
  if (counters == nullptr) { return; }
  counters->tx_packets.store(packets, std::memory_order_relaxed);
}

void set_rx_bytes(const std::shared_ptr<CounterSet>& counters, uint64_t bytes) {
  if (counters == nullptr) { return; }
  counters->rx_bytes.store(bytes, std::memory_order_relaxed);
}

void set_tx_bytes(const std::shared_ptr<CounterSet>& counters, uint64_t bytes) {
  if (counters == nullptr) { return; }
  counters->tx_bytes.store(bytes, std::memory_order_relaxed);
}

void set_dropped(const std::shared_ptr<CounterSet>& counters,
                 const std::string& reason,
                 uint64_t packets) {
  if (counters == nullptr) { return; }
  counters->get_drop_counter(reason)->packets.store(packets, std::memory_order_relaxed);
}

void shutdown() {
  registry().shutdown();
}

}  // namespace daqiri::metrics

#endif  // DAQIRI_ENABLE_OTEL_METRICS
