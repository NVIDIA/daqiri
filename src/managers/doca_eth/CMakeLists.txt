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

message(STATUS "PROJECT_NAME: ${MGR_TARGET}")

target_include_directories(${MGR_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(${MGR_TARGET} PRIVATE daqiri_doca_eth_mgr.cpp)

if(NOT TARGET daqiri_dpdk)
  message(FATAL_ERROR "doca_eth manager currently requires dpdk manager in DAQIRI_MGR list")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DOCA_ETH REQUIRED doca-eth)
pkg_check_modules(DOCA_FLOW REQUIRED doca-flow)

target_include_directories(${MGR_TARGET}
  PRIVATE
    ${DOCA_ETH_INCLUDE_DIRS}
    ${DOCA_FLOW_INCLUDE_DIRS}
)

target_link_directories(${MGR_TARGET}
  PRIVATE
    ${DOCA_ETH_LIBRARY_DIRS}
    ${DOCA_FLOW_LIBRARY_DIRS}
)

target_compile_options(${MGR_TARGET}
  PRIVATE
    ${DOCA_ETH_CFLAGS_OTHER}
    ${DOCA_FLOW_CFLAGS_OTHER}
)

target_link_libraries(${MGR_TARGET}
  PRIVATE
    daqiri_dpdk
    ${DOCA_ETH_LDFLAGS_OTHER}
    ${DOCA_FLOW_LDFLAGS_OTHER}
    ${DOCA_ETH_LIBRARIES}
    ${DOCA_FLOW_LIBRARIES}
)
