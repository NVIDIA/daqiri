# SPDX-FileCopyrightText: Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

target_sources(${ENGINE_TARGET} PRIVATE daqiri_gpunetio_ethernet_engine.cpp daqiri_gpunetio_ethernet_kernels.cu)

# DOCA 3.6 is the oldest supported release. DOCA installs its pkg-config files under
# ${DOCA_PATH}/lib/<multiarch>/pkgconfig, which FindPkgConfig searches once the DOCA prefix
# is in CMAKE_PREFIX_PATH.
set(DAQIRI_GPUNETIO_MIN_DOCA_VERSION 3.6)
if(NOT PKG_CONFIG_FOUND)
  message(FATAL_ERROR "pkg-config is required to locate DOCA for the gpunetio_ethernet engine")
endif()
list(APPEND CMAKE_PREFIX_PATH "${DOCA_PATH}")
pkg_check_modules(DOCA_GPUNETIO REQUIRED
    doca-gpunetio>=${DAQIRI_GPUNETIO_MIN_DOCA_VERSION}
    doca-eth>=${DAQIRI_GPUNETIO_MIN_DOCA_VERSION}
    doca-flow>=${DAQIRI_GPUNETIO_MIN_DOCA_VERSION}
    doca-common>=${DAQIRI_GPUNETIO_MIN_DOCA_VERSION})
message(STATUS "Found DOCA GPUNetIO ${DOCA_GPUNETIO_doca-gpunetio_VERSION}")

# The engine header keeps DOCA types opaque, so DOCA stays a private dependency
target_include_directories(${ENGINE_TARGET} PRIVATE ${DOCA_GPUNETIO_INCLUDE_DIRS})
target_compile_options(${ENGINE_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${DOCA_GPUNETIO_CFLAGS_OTHER}>)
target_link_directories(${ENGINE_TARGET} PUBLIC ${DOCA_GPUNETIO_LIBRARY_DIRS})
target_link_libraries(${ENGINE_TARGET} PRIVATE ${DOCA_GPUNETIO_LIBRARIES})

# Static-link flags for daqiri.pc. They are plain flags rather than Requires.private entries,
# so that pkg-config consumers of a shared build do not need DOCA's pkg-config files.
list(JOIN DOCA_GPUNETIO_LDFLAGS " " _daqiri_doca_ldflags)
set_property(GLOBAL PROPERTY DAQIRI_GPUNETIO_PC_LIBS_PRIVATE "${_daqiri_doca_ldflags}")
