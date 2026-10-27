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

add_executable(daqiri_socket_rx_queue_test socket_rx_queue_test.cpp)
target_compile_features(daqiri_socket_rx_queue_test PRIVATE cxx_std_17)
target_include_directories(daqiri_socket_rx_queue_test PRIVATE ${PROJECT_SOURCE_DIR})
if(BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
  # daqiri_common and the engine DSOs reference each other. Keep daqiri_common
  # loaded while this internal test links directly to the socket engine.
  target_link_options(daqiri_socket_rx_queue_test PRIVATE "LINKER:--no-as-needed")
endif()

if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
  target_link_libraries(daqiri_socket_rx_queue_test PRIVATE
    "-Wl,--start-group"
    ${DAQIRI_TEST_STATIC_LIBS}
    "-Wl,--end-group"
    ${DAQIRI_YAML_TARGET})
else()
  target_link_libraries(daqiri_socket_rx_queue_test PRIVATE daqiri_socket daqiri::daqiri
                        ${DAQIRI_YAML_TARGET})
endif()

add_test(NAME daqiri_socket_rx_queue_test COMMAND daqiri_socket_rx_queue_test)

if(TARGET daqiri_rdma)
  add_executable(daqiri_rdma_polling_test rdma_polling_test.cpp)
  target_compile_features(daqiri_rdma_polling_test PRIVATE cxx_std_17)
  target_include_directories(daqiri_rdma_polling_test PRIVATE ${PROJECT_SOURCE_DIR})
  if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
    target_link_libraries(daqiri_rdma_polling_test PRIVATE
      "-Wl,--start-group" ${DAQIRI_TEST_STATIC_LIBS} "-Wl,--end-group" ${DAQIRI_YAML_TARGET})
  else()
    if(UNIX AND NOT APPLE)
      target_link_options(daqiri_rdma_polling_test PRIVATE "LINKER:--no-as-needed")
    endif()
    target_link_libraries(daqiri_rdma_polling_test PRIVATE daqiri_rdma daqiri::daqiri ${DAQIRI_YAML_TARGET})
  endif()
  add_test(NAME daqiri_rdma_polling_test COMMAND daqiri_rdma_polling_test)
endif()

if(TARGET daqiri_ibverbs)
  add_executable(daqiri_ibverbs_tx_segments_test ibverbs_tx_segments_test.cpp)
  target_compile_features(daqiri_ibverbs_tx_segments_test PRIVATE cxx_std_17)
  target_include_directories(daqiri_ibverbs_tx_segments_test PRIVATE ${PROJECT_SOURCE_DIR})
  if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
    target_link_libraries(daqiri_ibverbs_tx_segments_test PRIVATE
      "-Wl,--start-group" ${DAQIRI_TEST_STATIC_LIBS} "-Wl,--end-group" ${DAQIRI_YAML_TARGET})
  else()
    if(UNIX AND NOT APPLE)
      target_link_options(daqiri_ibverbs_tx_segments_test PRIVATE "LINKER:--no-as-needed")
    endif()
    target_link_libraries(daqiri_ibverbs_tx_segments_test PRIVATE
      daqiri_ibverbs daqiri::daqiri ${DAQIRI_YAML_TARGET})
  endif()
  add_test(NAME daqiri_ibverbs_tx_segments_test COMMAND daqiri_ibverbs_tx_segments_test)
endif()

if(DAQIRI_IBVERBS_DLOPEN AND TARGET daqiri_raw_verbs_glue)
  find_package(Threads REQUIRED)
  add_executable(daqiri_rdma_glue_test rdma_glue_test.cpp
    ${PROJECT_SOURCE_DIR}/src/rdma/raw_api.cpp
    ${PROJECT_SOURCE_DIR}/src/rdma/cm_api.cpp)
  target_compile_features(daqiri_rdma_glue_test PRIVATE cxx_std_17)
  target_include_directories(daqiri_rdma_glue_test PRIVATE ${PROJECT_SOURCE_DIR}
    $<TARGET_PROPERTY:daqiri_ibverbs,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:daqiri_rdma,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(daqiri_rdma_glue_test PRIVATE DAQIRI_IBVERBS_DLOPEN=1)
  target_link_libraries(daqiri_rdma_glue_test PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  add_dependencies(daqiri_rdma_glue_test daqiri_raw_verbs_glue daqiri_cm_verbs_glue)
  add_test(NAME daqiri_rdma_glue_load COMMAND daqiri_rdma_glue_test load)
  set_tests_properties(daqiri_rdma_glue_load PROPERTIES
    ENVIRONMENT "DAQIRI_RDMA_GLUE_DIR=${CMAKE_BINARY_DIR}/lib")
  # Copy the executable and glue into an isolated deployment directory, with no
  # environment override or loader search path pointing back into the build.
  add_test(NAME daqiri_rdma_glue_adjacent
    COMMAND ${CMAKE_COMMAND}
      -DDAQIRI_GLUE_TEST=$<TARGET_FILE:daqiri_rdma_glue_test>
      -DDAQIRI_RAW_GLUE=$<TARGET_FILE:daqiri_raw_verbs_glue>
      -DDAQIRI_CM_GLUE=$<TARGET_FILE:daqiri_cm_verbs_glue>
      -DDAQIRI_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/adjacent-glue
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_rdma_glue_adjacent.cmake)
  add_test(NAME daqiri_rdma_glue_adjacent_chdir
    COMMAND ${CMAKE_COMMAND}
      -DDAQIRI_GLUE_TEST=$<TARGET_FILE:daqiri_rdma_glue_test>
      -DDAQIRI_RAW_GLUE=$<TARGET_FILE:daqiri_raw_verbs_glue>
      -DDAQIRI_CM_GLUE=$<TARGET_FILE:daqiri_cm_verbs_glue>
      -DDAQIRI_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/adjacent-glue-chdir
      -DDAQIRI_CHANGE_DIRECTORY=ON
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_rdma_glue_adjacent.cmake)
  add_test(NAME daqiri_rdma_glue_missing COMMAND daqiri_rdma_glue_test dlopen)
  set_tests_properties(daqiri_rdma_glue_missing PROPERTIES
    ENVIRONMENT "DAQIRI_RDMA_GLUE_DIR=${CMAKE_CURRENT_BINARY_DIR}/missing-glue")

  foreach(fixture IN ITEMS incompatible missing-symbol)
    foreach(kind IN ITEMS raw cm)
      set(fixture_target daqiri_${kind}_glue_${fixture})
      add_library(${fixture_target} SHARED rdma_glue_fixture.cpp)
      target_compile_features(${fixture_target} PRIVATE cxx_std_17)
      target_include_directories(${fixture_target} PRIVATE ${PROJECT_SOURCE_DIR}
        $<TARGET_PROPERTY:daqiri_ibverbs,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:daqiri_rdma,INTERFACE_INCLUDE_DIRECTORIES>)
      if(fixture STREQUAL "missing-symbol")
        target_compile_definitions(${fixture_target} PRIVATE DAQIRI_TEST_GLUE_MISSING_SYMBOL=1)
      endif()
      set_target_properties(${fixture_target} PROPERTIES
        OUTPUT_NAME daqiri_${kind}_verbs_glue SOVERSION 1
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${fixture}")
      add_dependencies(daqiri_rdma_glue_test ${fixture_target})
    endforeach()
    if(fixture STREQUAL "incompatible")
      set(expected_error "Incompatible")
    else()
      set(expected_error "dlsym")
    endif()
    add_test(NAME daqiri_rdma_glue_${fixture}
      COMMAND daqiri_rdma_glue_test ${expected_error})
    set_tests_properties(daqiri_rdma_glue_${fixture} PROPERTIES
      ENVIRONMENT "DAQIRI_RDMA_GLUE_DIR=${CMAKE_CURRENT_BINARY_DIR}/${fixture}")
  endforeach()

  add_executable(daqiri_rdma_init_missing_glue_test rdma_init_missing_glue_test.cpp)
  target_compile_features(daqiri_rdma_init_missing_glue_test PRIVATE cxx_std_17)
  target_include_directories(daqiri_rdma_init_missing_glue_test PRIVATE ${PROJECT_SOURCE_DIR})
  if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
    target_link_libraries(daqiri_rdma_init_missing_glue_test PRIVATE
      "-Wl,--start-group" ${DAQIRI_TEST_STATIC_LIBS} "-Wl,--end-group" ${DAQIRI_YAML_TARGET})
  else()
    target_link_options(daqiri_rdma_init_missing_glue_test PRIVATE "LINKER:--no-as-needed")
    target_link_libraries(daqiri_rdma_init_missing_glue_test PRIVATE
      daqiri::daqiri daqiri_ibverbs daqiri_rdma ${DAQIRI_YAML_TARGET})
  endif()
  add_test(NAME daqiri_rdma_init_missing_glue COMMAND daqiri_rdma_init_missing_glue_test)
  set_tests_properties(daqiri_rdma_init_missing_glue PROPERTIES
    ENVIRONMENT "DAQIRI_RDMA_GLUE_DIR=${CMAKE_CURRENT_BINARY_DIR}/missing-glue"
    PASS_REGULAR_EXPRESSION "Unable to load rdma runtime: dlopen")

  # Linux sockets must keep working without loading either RDMA glue library.
  add_test(NAME daqiri_socket_without_rdma_glue COMMAND daqiri_socket_rx_queue_test)
  set_tests_properties(daqiri_socket_without_rdma_glue PROPERTIES
    ENVIRONMENT "DAQIRI_RDMA_GLUE_DIR=${CMAKE_CURRENT_BINARY_DIR}/missing-glue")
endif()

if(DAQIRI_IBVERBS_DLOPEN AND TARGET daqiri_raw_verbs_glue AND NOT BUILD_SHARED_LIBS
   AND NOT TARGET daqiri_dpdk)
  find_program(DAQIRI_TEST_PKG_CONFIG pkg-config REQUIRED)
  find_program(DAQIRI_TEST_READELF readelf REQUIRED)
  add_test(NAME daqiri_rdma_static_install
    COMMAND ${CMAKE_COMMAND}
      -DDAQIRI_BUILD_DIR=${CMAKE_BINARY_DIR}
      -DDAQIRI_SOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DDAQIRI_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DDAQIRI_LIBDIR=${CMAKE_INSTALL_LIBDIR}
      -DDAQIRI_PKG_CONFIG=${DAQIRI_TEST_PKG_CONFIG}
      -DDAQIRI_READELF=${DAQIRI_TEST_READELF}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_rdma_static_install.cmake)
endif()
