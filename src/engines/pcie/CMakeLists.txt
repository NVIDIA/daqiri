# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

message(STATUS "PROJECT_NAME: ${ENGINE_TARGET}")

target_include_directories(${ENGINE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(${ENGINE_TARGET} PRIVATE daqiri_pcie_engine.cpp pcie_provider.cpp)
target_link_libraries(${ENGINE_TARGET} PRIVATE CUDA::cuda_driver CUDA::cudart)
