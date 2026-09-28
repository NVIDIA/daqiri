# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)

include(GNUInstallDirs)

add_executable(daqiri_config_validate config_validate.cpp)
target_compile_features(daqiri_config_validate PRIVATE cxx_std_17)
target_include_directories(daqiri_config_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(NOT DAQIRI_YAML_TARGET)
  find_package(yaml-cpp QUIET)
  if(TARGET yaml-cpp::yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp::yaml-cpp)
  elseif(TARGET yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp)
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/third_party/yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp)
  else()
    message(FATAL_ERROR
      "yaml-cpp not found; install yaml-cpp-dev or run "
      "`git submodule update --init third_party/yaml-cpp`")
  endif()
endif()

if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
  set(DAQIRI_TOOL_STATIC_LIBS daqiri::daqiri)
  foreach(engine IN ITEMS dpdk socket rdma ibverbs)
    if(TARGET daqiri_${engine})
      list(APPEND DAQIRI_TOOL_STATIC_LIBS daqiri_${engine})
    endif()
  endforeach()
  target_link_libraries(daqiri_config_validate PRIVATE
    "-Wl,--start-group"
    ${DAQIRI_TOOL_STATIC_LIBS}
    "-Wl,--end-group"
    ${DAQIRI_YAML_TARGET})
else()
  target_link_libraries(daqiri_config_validate PRIVATE daqiri::daqiri ${DAQIRI_YAML_TARGET})
endif()

set_target_properties(daqiri_config_validate PROPERTIES
  BUILD_RPATH "$ORIGIN/../src;$ORIGIN/../src/third_party/yaml-cpp"
  INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

install(TARGETS daqiri_config_validate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
