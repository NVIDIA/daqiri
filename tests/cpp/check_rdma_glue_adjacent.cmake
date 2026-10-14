# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

file(MAKE_DIRECTORY "${DAQIRI_TEST_DIR}")
configure_file("${DAQIRI_GLUE_TEST}" "${DAQIRI_TEST_DIR}/loader" COPYONLY)
configure_file("${DAQIRI_RAW_GLUE}" "${DAQIRI_TEST_DIR}/libdaqiri_raw_verbs_glue.so.1" COPYONLY)
configure_file("${DAQIRI_CM_GLUE}" "${DAQIRI_TEST_DIR}/libdaqiri_cm_verbs_glue.so.1" COPYONLY)
if(DAQIRI_CHANGE_DIRECTORY)
  file(MAKE_DIRECTORY "${DAQIRI_TEST_DIR}/elsewhere")
  # Preserve a relative argv[0], then change cwd inside the application before
  # either API loader runs. No glue libraries are present in the new cwd.
  set(loader "./loader" adjacent-chdir "${DAQIRI_TEST_DIR}/elsewhere")
else()
  set(loader "${DAQIRI_TEST_DIR}/loader" adjacent)
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=DAQIRI_RDMA_GLUE_DIR --unset=LD_LIBRARY_PATH
    ${loader}
  WORKING_DIRECTORY "${DAQIRI_TEST_DIR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Adjacent glue loading failed: ${output}\n${error}")
endif()
