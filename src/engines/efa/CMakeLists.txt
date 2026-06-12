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

message(STATUS "PROJECT_NAME: ${ENGINE_TARGET}")

target_include_directories(${ENGINE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(${ENGINE_TARGET} PRIVATE daqiri_efa_engine.cpp)

# libfabric (OFI) with the AWS EFA provider. AWS ships a CUDA-enabled build via
# the aws-efa-installer under /opt/amazon/efa; fall back to that pkgconfig dir
# if the system libfabric isn't found. Default to >= 1.18 for solid CUDA
# FI_HMEM + dma-buf support; override DAQIRI_EFA_MIN_LIBFABRIC to compile
# against an older libfabric.
set(DAQIRI_EFA_MIN_LIBFABRIC "1.18" CACHE STRING "Minimum libfabric version for the EFA engine")
pkg_check_modules(LIBFABRIC libfabric>=${DAQIRI_EFA_MIN_LIBFABRIC})
if(NOT LIBFABRIC_FOUND)
  message(STATUS "Looking for libfabric in /opt/amazon/efa")
  set(ENV{PKG_CONFIG_PATH} "/opt/amazon/efa/lib/pkgconfig:/opt/amazon/efa/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  pkg_check_modules(LIBFABRIC REQUIRED libfabric>=${DAQIRI_EFA_MIN_LIBFABRIC})
endif()

# DPDK is needed for rings/mempools (engine.h pulls in rte_ring/rte_mbuf).
pkg_check_modules(DPDK QUIET libdpdk)
if(NOT DPDK_FOUND)
  message(STATUS "Looking for DPDK in alternative directories")
  set(ENV{PKG_CONFIG_PATH} /opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig/)
  pkg_check_modules(DPDK REQUIRED libdpdk)
endif()

target_link_directories(${ENGINE_TARGET} PRIVATE ${DPDK_LIBRARY_DIRS})
target_include_directories(${ENGINE_TARGET} PRIVATE ${DPDK_INCLUDE_DIRS})
target_compile_options(${ENGINE_TARGET} PRIVATE ${DPDK_CFLAGS_OTHER})

# libfabric: PUBLIC because daqiri_efa_engine.h includes <rdma/fabric.h>.
target_include_directories(${ENGINE_TARGET} PUBLIC ${LIBFABRIC_INCLUDE_DIRS})
target_link_directories(${ENGINE_TARGET} PUBLIC ${LIBFABRIC_LIBRARY_DIRS})

target_link_libraries(${ENGINE_TARGET}
    PUBLIC
        ${LIBFABRIC_LIBRARIES}
    PRIVATE
        CUDA::cudart
        ${DPDK_LDFLAGS_OTHER}
        ${DPDK_LIBRARIES}
)
