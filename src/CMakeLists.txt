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

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 80 90)
  set(DAQIRI_CORE_USING_DEFAULT_CUDA_ARCHITECTURES ON)
endif()

enable_language(CUDA)

set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --diag-suppress 1217")
if(DAQIRI_CORE_USING_DEFAULT_CUDA_ARCHITECTURES AND
   CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
  list(APPEND CMAKE_CUDA_ARCHITECTURES 121)
endif()

add_compile_definitions(ALLOW_EXPERIMENTAL_API)
add_compile_definitions(DOCA_ALLOW_EXPERIMENTAL_API)
option(DAQIRI_REORDER_GPU_PROFILE "Enable CUDA event timing for DPDK reorder kernels" OFF)
if(DAQIRI_REORDER_GPU_PROFILE)
  add_compile_definitions(DAQIRI_REORDER_GPU_PROFILE=1)
endif()
option(DAQIRI_ENABLE_GDS "Enable cuFile-backed burst file writes from CUDA device memory" OFF)
option(DAQIRI_ENABLE_OTEL_METRICS "Enable OpenTelemetry metrics instrumentation" OFF)
option(DAQIRI_ENABLE_S3 "Enable AWS SDK-backed raw packet writes to S3" OFF)
set(DOCA_PATH /opt/mellanox/doca)

if(DAQIRI_ENABLE_OTEL_METRICS)
  add_compile_definitions(DAQIRI_ENABLE_OTEL_METRICS=1)
  find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS api)
else()
  add_compile_definitions(DAQIRI_ENABLE_OTEL_METRICS=0)
endif()

set(DAQIRI_USING_VENDORED_YAML_CPP OFF)
if(NOT TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp/CMakeLists.txt")
    message(STATUS "Using vendored yaml-cpp from third_party/yaml-cpp")
    set(DAQIRI_USING_VENDORED_YAML_CPP ON)
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

# DPDK is needed by the common library (rte rings/mbufs in engine.h, common.cpp, engine.cpp).
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
  common_gds.cpp
  common_s3.cpp
  dpdk_log.cpp
  kernels.cu
  engine.cpp
  logging.cpp
)
if(DAQIRI_ENABLE_OTEL_METRICS)
  target_sources(daqiri_common PRIVATE metrics.cpp)
endif()
target_include_directories(daqiri_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<BUILD_INTERFACE:${DAQIRI_SPDLOG_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
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
if(DAQIRI_ENABLE_OTEL_METRICS)
  target_link_libraries(daqiri_common PRIVATE opentelemetry-cpp::api)
endif()
if(DAQIRI_ENABLE_GDS)
  find_path(DAQIRI_CUFILE_INCLUDE_DIR
            NAMES cufile.h
            HINTS
              ${CUDAToolkit_INCLUDE_DIRS}
              ${CUDAToolkit_TARGET_DIR}/include)
  if(NOT DAQIRI_CUFILE_INCLUDE_DIR)
    message(FATAL_ERROR "DAQIRI_ENABLE_GDS=ON requires cufile.h")
  endif()

  if(TARGET CUDA::cuFile)
    set(DAQIRI_CUFILE_TARGET CUDA::cuFile)
  else()
    find_library(DAQIRI_CUFILE_LIBRARY
                 NAMES cufile
                 HINTS
                   ${CUDAToolkit_LIBRARY_DIR}
                   ${CUDAToolkit_TARGET_DIR}/lib64
                   ${CUDAToolkit_TARGET_DIR}/lib)
    if(NOT DAQIRI_CUFILE_LIBRARY)
      message(FATAL_ERROR "DAQIRI_ENABLE_GDS=ON requires libcufile")
    endif()
    set(DAQIRI_CUFILE_TARGET ${DAQIRI_CUFILE_LIBRARY})
  endif()

  target_compile_definitions(daqiri_common PUBLIC DAQIRI_ENABLE_GDS=1)
  target_include_directories(daqiri_common PRIVATE ${DAQIRI_CUFILE_INCLUDE_DIR})
  target_link_libraries(daqiri_common PRIVATE ${DAQIRI_CUFILE_TARGET})
else()
  target_compile_definitions(daqiri_common PUBLIC DAQIRI_ENABLE_GDS=0)
endif()
if(DAQIRI_ENABLE_S3)
  find_package(AWSSDK REQUIRED COMPONENTS s3)
  target_compile_definitions(daqiri_common PUBLIC DAQIRI_ENABLE_S3=1)
  target_include_directories(daqiri_common PRIVATE ${AWSSDK_INCLUDE_DIRS})
  target_link_libraries(daqiri_common PRIVATE ${AWSSDK_LINK_LIBRARIES})
else()
  target_compile_definitions(daqiri_common PUBLIC DAQIRI_ENABLE_S3=0)
endif()
set_target_properties(daqiri_common PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    OUTPUT_NAME "daqiri"
    EXPORT_NAME "daqiri"
    INSTALL_RPATH "$ORIGIN"
)
install(
    TARGETS daqiri_common
    EXPORT daqiriTargets
    COMPONENT daqiri-cpp
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/daqiri
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT daqiri-cpp
    FILES_MATCHING
      PATTERN "*.h"
      PATTERN "*.hpp"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spdlog/include/spdlog/spdlog.h")
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spdlog/include/spdlog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT daqiri-cpp
  )
endif()
if(DAQIRI_USING_VENDORED_YAML_CPP)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp/include/yaml-cpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT daqiri-cpp
    FILES_MATCHING
      PATTERN "*.h"
  )
endif()

# Engine libraries.
#
# DAQIRI_ENGINE selects which optional NIC engines are compiled in. Linux
# sockets (UDP/TCP) are always available, so the socket engine is always built
# and is NOT a DAQIRI_ENGINE value. Valid values:
#   dpdk    - raw Ethernet via kernel bypass
#   ibverbs - RDMA/RoCE via libibverbs (also enables the socket RoCE path)
if(NOT DEFINED DAQIRI_ENGINE)
    set(DAQIRI_ENGINE "dpdk ibverbs" CACHE STRING "Optional engine implementations to build: dpdk, ibverbs (Linux sockets are always built in)")
endif()
set_property(CACHE DAQIRI_ENGINE PROPERTY STRINGS "dpdk" "ibverbs")
separate_arguments(DAQIRI_ENGINE_LIST UNIX_COMMAND ${DAQIRI_ENGINE})

# Translate user-facing engine names to internal engine directories/targets.
# "ibverbs" is the user-facing name for the RDMA engine, implemented under
# src/engines/rdma (the daqiri_rdma target / DAQIRI_ENGINE_RDMA define).
set(DAQIRI_ENGINE_INTERNAL_LIST "")
foreach(ENGINE IN LISTS DAQIRI_ENGINE_LIST)
  if(ENGINE STREQUAL "dpdk")
    list(APPEND DAQIRI_ENGINE_INTERNAL_LIST "dpdk")
  elseif(ENGINE STREQUAL "ibverbs")
    # "ibverbs" builds both internal engines that use libibverbs: "rdma"
    # (RoCE/InfiniBand, used by socket roce:// endpoints) and "ibverbs" (the
    # pure-DevX MPRQ raw-Ethernet engine selected by stream_type: raw +
    # engine: ibverbs).
    list(APPEND DAQIRI_ENGINE_INTERNAL_LIST "rdma")
    list(APPEND DAQIRI_ENGINE_INTERNAL_LIST "ibverbs")
  else()
    message(FATAL_ERROR
      "Invalid DAQIRI_ENGINE value '${ENGINE}'. Valid values: dpdk, ibverbs. "
      "Linux sockets are always built in (there is no 'socket' value); "
      "use 'ibverbs' for RDMA/RoCE.")
  endif()
endforeach()

# The socket engine (Linux UDP/TCP, plus a RoCE path that delegates to the
# ibverbs/rdma engine when present) is always built.
list(APPEND DAQIRI_ENGINE_INTERNAL_LIST "socket")
list(REMOVE_DUPLICATES DAQIRI_ENGINE_INTERNAL_LIST)

# Socket links the rdma engine for its RoCE path, so build rdma first when it
# is present (its target must exist before the socket subdirectory is added).
list(FIND DAQIRI_ENGINE_INTERNAL_LIST "rdma" DAQIRI_HAS_RDMA_IDX)
if(NOT DAQIRI_HAS_RDMA_IDX EQUAL -1)
  list(REMOVE_ITEM DAQIRI_ENGINE_INTERNAL_LIST "rdma")
  list(INSERT DAQIRI_ENGINE_INTERNAL_LIST 0 "rdma")
endif()

# Define targets for each engine
foreach(ENGINE IN LISTS DAQIRI_ENGINE_INTERNAL_LIST)
    string(TOUPPER ${ENGINE} ENGINE_UPPER)
    string(TOLOWER ${ENGINE} ENGINE_LOWER)

    message(STATUS "Adding engine ${ENGINE_UPPER} to the configuration")
    set(ENGINE_TARGET daqiri_${ENGINE_LOWER})
    add_library(${ENGINE_TARGET})
    add_subdirectory(engines/${ENGINE_LOWER})
    target_compile_definitions(${ENGINE_TARGET} PUBLIC "DAQIRI_ENGINE_${ENGINE_UPPER}=1")
    target_include_directories(${ENGINE_TARGET}
        PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/..
          ${CMAKE_CURRENT_SOURCE_DIR}/../include
          ${DAQIRI_SPDLOG_INCLUDE_DIR}
    )
    target_link_libraries(${ENGINE_TARGET} PRIVATE CUDA::cudart ${DAQIRI_YAML_TARGET})
    set_target_properties(${ENGINE_TARGET} PROPERTIES
        OUTPUT_NAME "daqiri_${ENGINE_LOWER}"
        EXPORT_NAME "${ENGINE_LOWER}"
        INSTALL_RPATH "$ORIGIN"
    )
    install(TARGETS ${ENGINE_TARGET}
        EXPORT daqiriTargets
        COMPONENT daqiri-cpp
    )

    target_compile_definitions(daqiri_common PUBLIC "DAQIRI_ENGINE_${ENGINE_UPPER}=1")
    target_link_libraries(daqiri_common PRIVATE ${ENGINE_TARGET})
endforeach()
