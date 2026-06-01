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

namespace daqiri::bench::grafana {

#if defined(DAQIRI_GRAFANA_PROMETHEUS) && DAQIRI_GRAFANA_PROMETHEUS

bool init_prometheus_metrics_from_env();
void shutdown_prometheus_metrics();

#else

inline bool init_prometheus_metrics_from_env() {
  return false;
}

inline void shutdown_prometheus_metrics() {}

#endif

}  // namespace daqiri::bench::grafana
