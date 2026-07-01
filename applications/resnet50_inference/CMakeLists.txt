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

# Mirror the examples/ CUDA-arch auto-detect so Spark (sm_120/121), IGX (sm_89),
# and A100/H100 (sm_80/90) all build without manual flags.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 80 90)
  set(DAQIRI_APP_USING_DEFAULT_CUDA_ARCHITECTURES ON)
endif()

project(daqiri_resnet50_inference LANGUAGES CXX CUDA)

find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)

if(DAQIRI_APP_USING_DEFAULT_CUDA_ARCHITECTURES AND
   CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
  list(APPEND CMAKE_CUDA_ARCHITECTURES 121)
endif()
set(DAQIRI_APP_CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

# --- TensorRT discovery (cross-platform). NGC PyTorch (BASE_IMAGE=torch) ships
# TensorRT with headers under /usr/include and libs under
# /usr/lib/{x86_64,aarch64}-linux-gnu. The CMake-find machinery for TRT is
# inconsistent across versions, so locate the header + libs manually. If TRT is
# absent we warn and skip the target rather than failing the whole build.
find_path(TRT_INCLUDE_DIR NvInfer.h
  HINTS /usr/include /usr/include/x86_64-linux-gnu /usr/include/aarch64-linux-gnu)
find_library(TRT_NVINFER_LIB nvinfer
  HINTS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib)
find_library(TRT_NVONNXPARSER_LIB nvonnxparser
  HINTS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib)
if(NOT TRT_INCLUDE_DIR OR NOT TRT_NVINFER_LIB OR NOT TRT_NVONNXPARSER_LIB)
  message(WARNING
    "TensorRT not found (NvInfer.h / libnvinfer / libnvonnxparser); skipping "
    "resnet50_inference application. Build the container with BASE_IMAGE=torch "
    "to get TensorRT. INCLUDE=${TRT_INCLUDE_DIR} NVINFER=${TRT_NVINFER_LIB} "
    "NVONNX=${TRT_NVONNXPARSER_LIB}")
  return()
endif()
message(STATUS "TensorRT include: ${TRT_INCLUDE_DIR}")
message(STATUS "TensorRT libs: ${TRT_NVINFER_LIB} ${TRT_NVONNXPARSER_LIB}")

# yaml-cpp target: set by the top-level/src CMake when built in-tree; mirror the
# examples/ resolution block for a standalone application build.
if(NOT DAQIRI_YAML_TARGET)
  find_package(yaml-cpp QUIET)
  if(TARGET yaml-cpp::yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp::yaml-cpp)
  elseif(TARGET yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp)
  else()
    message(FATAL_ERROR "yaml-cpp not found; install yaml-cpp-dev")
  endif()
endif()

set(DAQIRI_EXAMPLES_DIR "${CMAKE_SOURCE_DIR}/examples")

# Reuse the bench scaffolding: config parsers, thread affinity, UDP header
# builders, wait_for_stop, and the ReorderPipeline. raw_bench_common.cpp's
# rx_count_worker references GpuWorkload + ReorderPipeline, so we also compile
# bench_workload.cu + bench_pipeline.cu and link the math libs (same as the
# socket bench).
add_executable(daqiri_resnet50_inference
  main.cpp
  app_config.cpp
  pcap_replayer.cpp
  feature_sink.cpp
  inference_pipeline.cu
  trt_runner.cu
  ${DAQIRI_EXAMPLES_DIR}/raw_bench_common.cpp
  ${DAQIRI_EXAMPLES_DIR}/raw_bench_common_cuda.cu
  ${DAQIRI_EXAMPLES_DIR}/bench_workload.cu
  ${DAQIRI_EXAMPLES_DIR}/bench_pipeline.cu)

target_compile_features(daqiri_resnet50_inference PRIVATE cxx_std_17)
target_include_directories(daqiri_resnet50_inference PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${DAQIRI_EXAMPLES_DIR}
  ${CMAKE_SOURCE_DIR}
  ${TRT_INCLUDE_DIR})
target_link_libraries(daqiri_resnet50_inference PRIVATE
  daqiri::daqiri
  ${DAQIRI_YAML_TARGET}
  CUDA::cudart
  CUDA::cufft
  CUDA::cublas
  Threads::Threads
  ${TRT_NVINFER_LIB}
  ${TRT_NVONNXPARSER_LIB})
set_target_properties(daqiri_resnet50_inference PROPERTIES
  CUDA_ARCHITECTURES "${DAQIRI_APP_CUDA_ARCHITECTURES}"
  BUILD_RPATH "$ORIGIN/../../src;$ORIGIN/../../src/third_party/yaml-cpp")

set(DAQIRI_RESNET_CONFIGS
  configs/resnet50_wire_loopback.yaml
  configs/resnet50_bench_spark.yaml)
foreach(cfg IN LISTS DAQIRI_RESNET_CONFIGS)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${cfg}
                 ${CMAKE_CURRENT_BINARY_DIR}/${cfg} COPYONLY)
endforeach()

install(TARGETS daqiri_resnet50_inference RUNTIME DESTINATION bin)
install(FILES ${DAQIRI_RESNET_CONFIGS} DESTINATION bin/resnet50_inference)
