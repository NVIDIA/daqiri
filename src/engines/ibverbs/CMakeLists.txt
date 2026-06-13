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

target_sources(${ENGINE_TARGET} PRIVATE daqiri_ibverbs_engine.cpp)

# Core verbs library.
pkg_check_modules(IBVERBS libibverbs)
if(NOT IBVERBS_FOUND)
    find_library(IBVERBS_LIBRARIES
        NAMES ibverbs
        PATHS /usr/lib /usr/lib64
    )
    find_path(IBVERBS_INCLUDE_DIRS
        NAMES infiniband/verbs.h
        PATHS /usr/include
    )
endif()

# mlx5 direct-verbs provider library for striding-RQ (MPRQ) attributes and DevX.
pkg_check_modules(MLX5 libmlx5)
if(NOT MLX5_FOUND)
    find_library(MLX5_LIBRARIES
        NAMES mlx5
        PATHS /usr/lib /usr/lib64
    )
    find_path(MLX5_INCLUDE_DIRS
        NAMES infiniband/mlx5dv.h
        PATHS /usr/include
    )
endif()

if(NOT IBVERBS_LIBRARIES)
    message(FATAL_ERROR "ibverbs engine requires libibverbs")
endif()
if(NOT MLX5_LIBRARIES)
    message(FATAL_ERROR "ibverbs engine requires libmlx5 (mlx5dv.h) for striding RQ / DevX")
endif()

# The ibverbs engine no longer depends on DPDK: its rings/pools are the
# libdpdk-free daqiri::Ring / daqiri::ObjectPool, and it drives the NIC through
# libibverbs/mlx5dv directly.

# verbs/mlx5dv: PUBLIC (daqiri_ibverbs_engine.h includes <infiniband/verbs.h> and <infiniband/mlx5dv.h>)
target_include_directories(${ENGINE_TARGET} PUBLIC ${IBVERBS_INCLUDE_DIRS} ${MLX5_INCLUDE_DIRS})
target_link_directories(${ENGINE_TARGET} PUBLIC ${IBVERBS_LIBRARY_DIRS} ${MLX5_LIBRARY_DIRS})

target_link_libraries(${ENGINE_TARGET}
    PUBLIC
        ${IBVERBS_LIBRARIES}
        ${MLX5_LIBRARIES}
    PRIVATE
        CUDA::cuda_driver
)
