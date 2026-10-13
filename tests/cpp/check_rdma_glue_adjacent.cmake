# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

file(MAKE_DIRECTORY "${DAQIRI_TEST_DIR}")
configure_file("${DAQIRI_GLUE_TEST}" "${DAQIRI_TEST_DIR}/loader" COPYONLY)
configure_file("${DAQIRI_RAW_GLUE}" "${DAQIRI_TEST_DIR}/libdaqiri_raw_verbs_glue.so.1" COPYONLY)
configure_file("${DAQIRI_CM_GLUE}" "${DAQIRI_TEST_DIR}/libdaqiri_cm_verbs_glue.so.1" COPYONLY)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=DAQIRI_RDMA_GLUE_DIR --unset=LD_LIBRARY_PATH
    "${DAQIRI_TEST_DIR}/loader" adjacent
  WORKING_DIRECTORY "${DAQIRI_TEST_DIR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Adjacent glue loading failed: ${output}\n${error}")
endif()
