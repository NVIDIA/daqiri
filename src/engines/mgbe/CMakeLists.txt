# SPDX-License-Identifier: Apache-2.0
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The MGBE engine requires Linux")
endif()
target_sources(${ENGINE_TARGET} PRIVATE daqiri_mgbe_engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../drivers/mgbe/src/mgbe_pmd.cpp)
target_include_directories(${ENGINE_TARGET} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../drivers/mgbe/include)
