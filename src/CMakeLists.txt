# SPDX-FileCopyrightText: Copyright (c) 2025-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
project(daqiri_core)

find_package(PkgConfig QUIET)
find_package(CUDAToolkit REQUIRED)
find_package(yaml-cpp QUIET)

enable_language(CUDA)

set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --diag-suppress 1217")
set(CMAKE_CUDA_ARCHITECTURES "80;90")

add_compile_definitions(ALLOW_EXPERIMENTAL_API)
add_compile_definitions(DOCA_ALLOW_EXPERIMENTAL_API)
option(DAQIRI_REORDER_GPU_PROFILE "Enable CUDA event timing for DPDK reorder kernels" OFF)
if(DAQIRI_REORDER_GPU_PROFILE)
  add_compile_definitions(DAQIRI_REORDER_GPU_PROFILE=1)
endif()
set(DOCA_PATH /opt/mellanox/doca)

if(NOT TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp/CMakeLists.txt")
    message(STATUS "Using vendored yaml-cpp from third_party/yaml-cpp")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/third_party/yaml-cpp)
  else()
    message(FATAL_ERROR "yaml-cpp not found and third_party/yaml-cpp is missing")
  endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spdlog/include/spdlog/spdlog.h")
  message(STATUS "Using vendored spdlog headers from third_party/spdlog")
  set(DAQIRI_SPDLOG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spdlog/include")
else()
  find_path(DAQIRI_SPDLOG_INCLUDE_DIR
            NAMES spdlog/spdlog.h
            REQUIRED)
  if(DAQIRI_SPDLOG_INCLUDE_DIR)
    message(STATUS "Using system spdlog headers from ${DAQIRI_SPDLOG_INCLUDE_DIR}")
  else()
    message(FATAL_ERROR "spdlog headers not found")
  endif()
endif()

# DPDK is needed by the common library (rte rings/mbufs in manager.h, common.cpp, manager.cpp).
if(NOT PKG_CONFIG_FOUND)
  message(FATAL_ERROR "pkg-config is required to locate DPDK (libdpdk)")
endif()
pkg_check_modules(DPDK QUIET libdpdk)
if(NOT DPDK_FOUND)
  list(APPEND CMAKE_PREFIX_PATH "/opt/mellanox/dpdk/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu/")
  pkg_check_modules(DPDK REQUIRED libdpdk)
endif()

# Common library
add_library(daqiri_common
  common.cpp
  dpdk_log.cpp
  kernels.cu
  manager.cpp
  logging.cpp
)
target_include_directories(daqiri_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..> # so we can find it under #include "src/..."
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<BUILD_INTERFACE:${DAQIRI_SPDLOG_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
        ${DPDK_INCLUDE_DIRS}
)
add_library(daqiri::daqiri ALIAS daqiri_common)
target_link_directories(daqiri_common PUBLIC ${DPDK_LIBRARY_DIRS})
if(TARGET yaml-cpp::yaml-cpp)
  set(DAQIRI_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(DAQIRI_YAML_TARGET yaml-cpp)
endif()
target_link_libraries(daqiri_common
    PUBLIC
        CUDA::cudart
        CUDA::cuda_driver
        ${DAQIRI_YAML_TARGET}
        ${DPDK_LDFLAGS_OTHER}
        ${DPDK_LIBRARIES}
    PRIVATE
)
set_target_properties(daqiri_common PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    OUTPUT_NAME "daqiri"
    EXPORT_NAME "daqiri"
)
install(
    TARGETS daqiri_common
    EXPORT daqiriTargets
    COMPONENT daqiri-cpp
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION include/src
    FILES_MATCHING
      PATTERN "*.h"
      PATTERN "*.hpp"
)
install(
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/logging.hpp
    DESTINATION include/src
    COMPONENT daqiri-cpp
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spdlog/include/spdlog/spdlog.h")
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spdlog/include/spdlog
    DESTINATION include
    COMPONENT daqiri-cpp
  )
endif()

# Backend libraries
if(NOT DEFINED DAQIRI_MGR)
    # Initialize DAQIRI_MGR with a default if not provided
    # Default: raw + socket backends
    set(DAQIRI_MGR "dpdk socket" CACHE STRING "Manager type(s) list")
endif()
set_property(CACHE DAQIRI_MGR PROPERTY STRINGS "dpdk" "socket" "rdma")
separate_arguments(DAQIRI_MGR_LIST UNIX_COMMAND ${DAQIRI_MGR})

# Socket backend reuses RoCE transport from the RDMA implementation.
# Ensure rdma backend is built first whenever socket is requested.
list(FIND DAQIRI_MGR_LIST "socket" DAQIRI_HAS_SOCKET_IDX)
if(NOT DAQIRI_HAS_SOCKET_IDX EQUAL -1)
  list(FIND DAQIRI_MGR_LIST "rdma" DAQIRI_HAS_RDMA_IDX)
  if(DAQIRI_HAS_RDMA_IDX EQUAL -1)
    list(APPEND DAQIRI_MGR_LIST "rdma")
  endif()
  list(REMOVE_ITEM DAQIRI_MGR_LIST "rdma")
  list(INSERT DAQIRI_MGR_LIST 0 "rdma")
endif()
list(REMOVE_DUPLICATES DAQIRI_MGR_LIST)

# Define targets for each manager type
foreach(MGR IN LISTS DAQIRI_MGR_LIST)
    string(TOUPPER ${MGR} MGR_UPPER)
    string(TOLOWER ${MGR} MGR_LOWER)

    message(STATUS "Adding manager ${MGR_UPPER} to the configuration")
    set(MGR_TARGET daqiri_${MGR_LOWER})
    add_library(${MGR_TARGET})
    add_subdirectory(managers/${MGR_LOWER})
    target_compile_definitions(${MGR_TARGET} PUBLIC "DAQIRI_MGR_${MGR_UPPER}=1")
    target_include_directories(${MGR_TARGET}
        PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/..
          ${CMAKE_CURRENT_SOURCE_DIR}/../include
          ${DAQIRI_SPDLOG_INCLUDE_DIR}
    )
    target_link_libraries(${MGR_TARGET} PRIVATE CUDA::cudart ${DAQIRI_YAML_TARGET})
    set_target_properties(${MGR_TARGET} PROPERTIES
        OUTPUT_NAME "daqiri_${MGR_LOWER}"
        EXPORT_NAME "${MGR_LOWER}"
    )
    install(TARGETS ${MGR_TARGET}
        EXPORT daqiriTargets
        COMPONENT daqiri-cpp
    )

    target_compile_definitions(daqiri_common PUBLIC "DAQIRI_MGR_${MGR_UPPER}=1")
    target_link_libraries(daqiri_common PRIVATE ${MGR_TARGET})
endforeach()
