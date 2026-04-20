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

message(STATUS "PROJECT_NAME: ${MGR_TARGET}")

target_include_directories(${MGR_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(${MGR_TARGET} PRIVATE daqiri_socket_mgr.cpp)

# manager.h includes DPDK ring/mbuf headers.
pkg_check_modules(DPDK QUIET libdpdk)
if(NOT DPDK_FOUND)
  list(APPEND CMAKE_PREFIX_PATH "/opt/mellanox/dpdk/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu/")
  pkg_check_modules(DPDK REQUIRED libdpdk)
endif()

target_link_directories(${MGR_TARGET} PRIVATE ${DPDK_LIBRARY_DIRS})
target_include_directories(${MGR_TARGET} PRIVATE ${DPDK_INCLUDE_DIRS})
target_compile_options(${MGR_TARGET} PRIVATE ${DPDK_CFLAGS_OTHER})

target_link_libraries(${MGR_TARGET}
  PRIVATE
    ${DPDK_LDFLAGS_OTHER}
    ${DPDK_LIBRARIES}
)

# RoCE-under-socket delegates to the RDMA manager implementation.
if(NOT TARGET daqiri_rdma)
  message(FATAL_ERROR "Socket backend requires daqiri_rdma target for RoCE delegation")
endif()
target_link_libraries(${MGR_TARGET} PRIVATE daqiri_rdma)
