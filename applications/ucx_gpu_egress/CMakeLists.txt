# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_subdirectory(processing)
find_package(CUDAToolkit REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(UCX REQUIRED IMPORTED_TARGET ucx>=1.20.0)

add_library(daqiri_ucx_gpu_transport STATIC
  protocol.cpp
  external_batch_policy.cpp
  cuda_image.cu
  ucx_transport.cpp)
target_compile_features(daqiri_ucx_gpu_transport PUBLIC cxx_std_17)
target_include_directories(daqiri_ucx_gpu_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daqiri_ucx_gpu_transport
  PUBLIC CUDA::cudart PkgConfig::UCX Threads::Threads)
set_target_properties(daqiri_ucx_gpu_transport PROPERTIES
  CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
  POSITION_INDEPENDENT_CODE ON)

add_library(daqiri_ucx_raw_assembly STATIC
  raw/dqri_header.cpp
  raw/fragment_placement.cu
  raw/image_assembler.cpp)
target_compile_features(daqiri_ucx_raw_assembly PUBLIC cxx_std_17)
target_include_directories(daqiri_ucx_raw_assembly PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daqiri_ucx_raw_assembly PUBLIC CUDA::cudart)
set_target_properties(daqiri_ucx_raw_assembly PROPERTIES
  CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
  POSITION_INDEPENDENT_CODE ON)

add_executable(daqiri_ucx_gpu_transport_bench ucx_gpu_transport_bench.cpp)
target_compile_features(daqiri_ucx_gpu_transport_bench PRIVATE cxx_std_17)
target_link_libraries(daqiri_ucx_gpu_transport_bench PRIVATE daqiri_ucx_gpu_transport)

add_executable(daqiri_ucx_raw_source raw_tx_bench.cpp)
target_compile_features(daqiri_ucx_raw_source PRIVATE cxx_std_17)
target_include_directories(daqiri_ucx_raw_source PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daqiri_ucx_raw_source
  PRIVATE daqiri::daqiri daqiri_ucx_raw_assembly ${DAQIRI_YAML_TARGET}
          CUDA::cudart Threads::Threads)

add_executable(daqiri_ucx_raw_pipeline
  raw_pipeline_bench.cu)
target_compile_features(daqiri_ucx_raw_pipeline PRIVATE cxx_std_17)
target_include_directories(daqiri_ucx_raw_pipeline
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daqiri_ucx_raw_pipeline
  PRIVATE daqiri::daqiri daqiri_ucx_gpu_transport daqiri_ucx_raw_assembly
          daqiri_ucx_scale_offset ${DAQIRI_YAML_TARGET} CUDA::cudart Threads::Threads)
set_target_properties(daqiri_ucx_raw_pipeline PROPERTIES
  CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

if(BUILD_TESTING)
  add_executable(daqiri_test_ucx_gpu_protocol protocol.cpp protocol_test.cpp)
  target_compile_features(daqiri_test_ucx_gpu_protocol PRIVATE cxx_std_17)

  add_executable(daqiri_test_ucx_external_batch_policy
    external_batch_policy.cpp
    external_batch_policy_test.cpp)
  target_compile_features(daqiri_test_ucx_external_batch_policy PRIVATE cxx_std_17)

  add_executable(daqiri_test_ucx_spsc_queue spsc_queue_test.cpp)
  target_compile_features(daqiri_test_ucx_spsc_queue PRIVATE cxx_std_17)
  target_link_libraries(daqiri_test_ucx_spsc_queue PRIVATE Threads::Threads)

  add_executable(daqiri_test_ucx_raw_header
    raw/dqri_header.cpp
    raw/tests/dqri_header_test.cpp)
  target_compile_features(daqiri_test_ucx_raw_header PRIVATE cxx_std_17)
  target_include_directories(daqiri_test_ucx_raw_header PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(daqiri_test_ucx_fragment_placement
    raw/dqri_header.cpp
    raw/fragment_placement.cu
    raw/tests/fragment_placement_test.cu)
  target_compile_features(daqiri_test_ucx_fragment_placement PRIVATE cxx_std_17)
  target_include_directories(daqiri_test_ucx_fragment_placement
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(daqiri_test_ucx_fragment_placement PRIVATE CUDA::cudart)
  set_target_properties(daqiri_test_ucx_fragment_placement PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

  add_executable(daqiri_test_ucx_image_assembler
    raw/tests/image_assembler_test.cpp)
  target_compile_features(daqiri_test_ucx_image_assembler PRIVATE cxx_std_17)
  target_include_directories(daqiri_test_ucx_image_assembler
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(daqiri_test_ucx_image_assembler
    PRIVATE daqiri_ucx_raw_assembly)

  add_test(NAME daqiri_test_ucx_gpu_protocol COMMAND daqiri_test_ucx_gpu_protocol)
  add_test(NAME daqiri_test_ucx_external_batch_policy
           COMMAND daqiri_test_ucx_external_batch_policy)
  add_test(NAME daqiri_test_ucx_spsc_queue COMMAND daqiri_test_ucx_spsc_queue)
  add_test(NAME daqiri_test_ucx_raw_header COMMAND daqiri_test_ucx_raw_header)
  add_test(NAME daqiri_test_ucx_fragment_placement COMMAND daqiri_test_ucx_fragment_placement)
  add_test(NAME daqiri_test_ucx_image_assembler COMMAND daqiri_test_ucx_image_assembler)
endif()

install(TARGETS daqiri_ucx_gpu_transport_bench daqiri_ucx_raw_source
                daqiri_ucx_raw_pipeline
        RUNTIME DESTINATION bin)

set(DAQIRI_UCX_GPU_EGRESS_CONFIGS
  configs/raw_source.yaml
  configs/raw_processor.yaml)
foreach(cfg IN LISTS DAQIRI_UCX_GPU_EGRESS_CONFIGS)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${cfg}
                 ${CMAKE_CURRENT_BINARY_DIR}/${cfg} COPYONLY)
endforeach()
install(FILES ${DAQIRI_UCX_GPU_EGRESS_CONFIGS} DESTINATION bin/ucx_gpu_egress)
