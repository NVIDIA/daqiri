# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_executable(daqiri_init_validation_test daqiri_init_validation_test.cpp)
target_compile_features(daqiri_init_validation_test PRIVATE cxx_std_17)
target_include_directories(daqiri_init_validation_test PRIVATE ${PROJECT_SOURCE_DIR})

if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
  set(DAQIRI_TEST_STATIC_LIBS daqiri::daqiri)
  foreach(engine IN ITEMS dpdk socket rdma ibverbs)
    if(TARGET daqiri_${engine})
      list(APPEND DAQIRI_TEST_STATIC_LIBS daqiri_${engine})
    endif()
  endforeach()
  target_link_libraries(daqiri_init_validation_test PRIVATE
    "-Wl,--start-group"
    ${DAQIRI_TEST_STATIC_LIBS}
    "-Wl,--end-group"
    ${DAQIRI_YAML_TARGET})
else()
  target_link_libraries(daqiri_init_validation_test PRIVATE daqiri::daqiri
                        ${DAQIRI_YAML_TARGET})
endif()

add_test(NAME daqiri_init_validation_test COMMAND daqiri_init_validation_test)
