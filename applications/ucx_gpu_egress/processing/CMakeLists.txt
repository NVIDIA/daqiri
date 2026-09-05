# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_library(daqiri_ucx_scale_offset STATIC scale_offset.cu)
target_compile_features(daqiri_ucx_scale_offset PUBLIC cxx_std_17)
target_include_directories(daqiri_ucx_scale_offset PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daqiri_ucx_scale_offset PUBLIC CUDA::cudart)
set_target_properties(daqiri_ucx_scale_offset PROPERTIES
  CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
  POSITION_INDEPENDENT_CODE ON)

# Do not add --use_fast_math: explicit fmaf and round-to-nearest-even conversion
# are observable output semantics of this example.
if(BUILD_TESTING)
  add_executable(daqiri_test_ucx_scale_offset test_scale_offset.cu)
  target_compile_features(daqiri_test_ucx_scale_offset PRIVATE cxx_std_17)
  target_link_libraries(daqiri_test_ucx_scale_offset PRIVATE
    daqiri_ucx_scale_offset CUDA::cudart)
  set_target_properties(daqiri_test_ucx_scale_offset PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

  add_test(NAME daqiri_test_ucx_scale_offset
           COMMAND daqiri_test_ucx_scale_offset)
endif()
