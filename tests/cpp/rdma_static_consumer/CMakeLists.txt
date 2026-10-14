# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(daqiri_static_consumer LANGUAGES CXX)
find_package(daqiri CONFIG REQUIRED)
add_executable(consumer ../daqiri_init_validation_test.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE "-Wl,--start-group"
  daqiri::daqiri daqiri::ibverbs daqiri::rdma daqiri::socket "-Wl,--end-group")
