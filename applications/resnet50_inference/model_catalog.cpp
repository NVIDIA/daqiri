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

#include "model_catalog.h"

#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace daqiri::apps::resnet {

ModelProfile load_model_profile(const std::string& catalog_path, const std::string& name) {
  const YAML::Node catalog = YAML::LoadFile(catalog_path);
  const YAML::Node entry = catalog[name];
  if (!entry) {
    throw std::runtime_error("model '" + name + "' is not in " + catalog_path);
  }

  ModelProfile profile;
  profile.name = name;
  profile.architecture = entry["architecture"].as<std::string>();
  profile.onnx_path = entry["onnx_path"].as<std::string>();
  profile.engine_path = entry["engine_path"].as<std::string>();
  profile.feature_dim = entry["feature_dim"].as<int>();
  profile.input_type = entry["input_type"].as<std::string>();
  if (profile.input_type != "fp16" && profile.input_type != "int8") {
    throw std::runtime_error("model '" + name + "' has unsupported input_type '" +
                             profile.input_type + "'");
  }
  return profile;
}

}  // namespace daqiri::apps::resnet
