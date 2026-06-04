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

#include "otel_prometheus.h"

#if defined(DAQIRI_GRAFANA_PROMETHEUS) && DAQIRI_GRAFANA_PROMETHEUS

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <opentelemetry/exporters/prometheus/exporter_factory.h>
#include <opentelemetry/exporters/prometheus/exporter_options.h>
#include <opentelemetry/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/provider.h>

namespace daqiri::bench::grafana {
namespace {

namespace metrics_api = opentelemetry::metrics;
namespace metrics_exporter = opentelemetry::exporter::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;

bool g_metrics_initialized = false;

}  // namespace

bool init_prometheus_metrics_from_env() {
  const char *endpoint = std::getenv("DAQIRI_OTEL_PROMETHEUS_ENDPOINT");
  if (endpoint == nullptr || std::string(endpoint).empty()) { return false; }

  metrics_exporter::PrometheusExporterOptions options;
  options.url = endpoint;
  options.without_otel_scope = true;

  auto reader = metrics_exporter::PrometheusExporterFactory::Create(options);
  auto provider = metrics_sdk::MeterProviderFactory::Create();
  auto *sdk_provider = static_cast<metrics_sdk::MeterProvider *>(provider.get());
  sdk_provider->AddMetricReader(std::move(reader));

  std::shared_ptr<metrics_api::MeterProvider> api_provider(std::move(provider));
  metrics_sdk::Provider::SetMeterProvider(api_provider);
  g_metrics_initialized = true;

  std::cout << "DAQIRI OpenTelemetry Prometheus metrics listening on " << endpoint << "\n";
  return true;
}

void shutdown_prometheus_metrics() {
  if (!g_metrics_initialized) { return; }
  std::shared_ptr<metrics_api::MeterProvider> none;
  metrics_sdk::Provider::SetMeterProvider(none);
  g_metrics_initialized = false;
}

}  // namespace daqiri::bench::grafana

#else

namespace daqiri::bench::grafana {

bool init_prometheus_metrics_from_env() {
  return false;
}

void shutdown_prometheus_metrics() {}

}  // namespace daqiri::bench::grafana

#endif
