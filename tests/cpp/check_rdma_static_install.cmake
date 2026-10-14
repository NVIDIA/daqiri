# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

function(run_checked)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Command failed: ${ARGV}\n${output}\n${error}")
  endif()
endfunction()

set(prefix "${DAQIRI_BUILD_DIR}/tests/rdma-install")
set(consumer_build "${DAQIRI_BUILD_DIR}/tests/rdma-consumer")
run_checked("${CMAKE_COMMAND}" --install "${DAQIRI_BUILD_DIR}" --prefix "${prefix}")
# Load the installed DSOs, including their provider dependencies, before linking
# consumers. This verifies the deployed files rather than build-tree copies.
set(ENV{DAQIRI_RDMA_GLUE_DIR} "${prefix}/${DAQIRI_LIBDIR}")
run_checked("${DAQIRI_BUILD_DIR}/tests/cpp/daqiri_rdma_glue_test" load)
set(ENV{DAQIRI_RDMA_GLUE_DIR} "${prefix}/missing-glue")
run_checked("${CMAKE_COMMAND}" -S "${DAQIRI_SOURCE_DIR}/tests/cpp/rdma_static_consumer"
  -B "${consumer_build}" "-DCMAKE_PREFIX_PATH=${prefix}"
  "-DCMAKE_CXX_COMPILER=${DAQIRI_CXX_COMPILER}")
run_checked("${CMAKE_COMMAND}" --build "${consumer_build}")
run_checked("${consumer_build}/consumer")

set(ENV{PKG_CONFIG_PATH} "${prefix}/${DAQIRI_LIBDIR}/pkgconfig")
execute_process(COMMAND "${DAQIRI_PKG_CONFIG}" --static --cflags --libs daqiri
  RESULT_VARIABLE status OUTPUT_VARIABLE flags ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR flags MATCHES "-l(ibverbs|mlx5|rdmacm|rte_|dpdk)")
  message(FATAL_ERROR "Unexpected static pkg-config dependencies: ${flags}\n${error}")
endif()
separate_arguments(consumer_flags UNIX_COMMAND "${flags}")
run_checked("${DAQIRI_CXX_COMPILER}" -std=c++17
  "${DAQIRI_SOURCE_DIR}/tests/cpp/daqiri_init_validation_test.cpp"
  -o "${consumer_build}/pkg-consumer" ${consumer_flags})
run_checked("${consumer_build}/pkg-consumer")

foreach(executable IN ITEMS consumer pkg-consumer)
  execute_process(COMMAND "${DAQIRI_READELF}" -d "${consumer_build}/${executable}"
    RESULT_VARIABLE status OUTPUT_VARIABLE dynamic ERROR_VARIABLE error)
  if(NOT status EQUAL 0 OR dynamic MATCHES "lib(ibverbs|mlx5|rdmacm)")
    message(FATAL_ERROR "Unexpected RDMA ELF dependency in ${executable}: ${dynamic}\n${error}")
  endif()
endforeach()
