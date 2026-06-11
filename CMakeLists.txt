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

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 80 90)
  set(DAQIRI_USING_DEFAULT_CUDA_ARCHITECTURES ON)
endif()

project(daqiri VERSION 0.1.0 LANGUAGES C CXX CUDA)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(CUDAToolkit REQUIRED)

if(DAQIRI_USING_DEFAULT_CUDA_ARCHITECTURES AND
   CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
  list(APPEND CMAKE_CUDA_ARCHITECTURES 121)
endif()

option(DAQIRI_BUILD_PYTHON "Build Python bindings" OFF)
option(DAQIRI_BUILD_EXAMPLES "Build standalone examples" ON)
option(DAQIRI_ENABLE_OTEL_METRICS "Enable OpenTelemetry metrics instrumentation" OFF)
set(DAQIRI_MGR "dpdk socket" CACHE STRING "Manager backend list")

add_subdirectory(src)

if(TARGET yaml-cpp)
  install(
    TARGETS yaml-cpp
    EXPORT daqiriTargets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()

if(DAQIRI_BUILD_PYTHON)
  add_subdirectory(python)
endif()

if(DAQIRI_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

install(
  EXPORT daqiriTargets
  NAMESPACE daqiri::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/daqiri
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/daqiriConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/daqiriConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/daqiri
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/daqiriConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(DAQIRI_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(DAQIRI_PKGCONFIG_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
  set(DAQIRI_PC_PREFIX_FROM_PCFILEDIR "..")
  string(REPLACE "/" ";" DAQIRI_PC_LIBDIR_COMPONENTS "${CMAKE_INSTALL_LIBDIR}")
  foreach(component IN LISTS DAQIRI_PC_LIBDIR_COMPONENTS)
    if(NOT component STREQUAL "")
      string(APPEND DAQIRI_PC_PREFIX_FROM_PCFILEDIR "/..")
    endif()
  endforeach()
  set(DAQIRI_PC_PREFIX "\${pcfiledir}/${DAQIRI_PC_PREFIX_FROM_PCFILEDIR}")
  set(DAQIRI_PKGCONFIG_LIBDIR "\${pcfiledir}/..")
endif()

if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
  set(DAQIRI_PKGCONFIG_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
  set(DAQIRI_PKGCONFIG_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

set(DAQIRI_PC_REQUIRES "")
set(DAQIRI_PC_REQUIRES_PRIVATE "libdpdk")
set(DAQIRI_PC_LIBS "-lyaml-cpp")
if(DAQIRI_ENABLE_OTEL_METRICS)
  string(APPEND DAQIRI_PC_LIBS " -lopentelemetry_api")
endif()
separate_arguments(DAQIRI_PC_MGR_LIST UNIX_COMMAND "${DAQIRI_MGR}")
list(FIND DAQIRI_PC_MGR_LIST "socket" DAQIRI_PC_HAS_SOCKET_IDX)
list(FIND DAQIRI_PC_MGR_LIST "rdma" DAQIRI_PC_HAS_RDMA_IDX)
if(NOT DAQIRI_PC_HAS_SOCKET_IDX EQUAL -1 OR NOT DAQIRI_PC_HAS_RDMA_IDX EQUAL -1)
  string(APPEND DAQIRI_PC_REQUIRES_PRIVATE " libibverbs librdmacm")
endif()

set(DAQIRI_PC_REQUIRES_LINE "")
if(DAQIRI_PC_REQUIRES)
  set(DAQIRI_PC_REQUIRES_LINE "Requires: ${DAQIRI_PC_REQUIRES}")
endif()

set(DAQIRI_PC_REQUIRES_PRIVATE_LINE "")
if(DAQIRI_PC_REQUIRES_PRIVATE)
  set(DAQIRI_PC_REQUIRES_PRIVATE_LINE "Requires.private: ${DAQIRI_PC_REQUIRES_PRIVATE}")
endif()

set(DAQIRI_PC_EXTRA_CFLAGS "")
foreach(include_dir IN LISTS CUDAToolkit_INCLUDE_DIRS)
  if(include_dir)
    string(APPEND DAQIRI_PC_EXTRA_CFLAGS " -I${include_dir}")
  endif()
endforeach()

set(DAQIRI_PC_LIBS_PRIVATE "-lcudart -lcuda")
if(CUDAToolkit_LIBRARY_DIR)
  string(PREPEND DAQIRI_PC_LIBS_PRIVATE "-L${CUDAToolkit_LIBRARY_DIR} ")
endif()
if(DAQIRI_ENABLE_S3)
  string(APPEND DAQIRI_PC_LIBS_PRIVATE " -laws-cpp-sdk-s3 -laws-cpp-sdk-core")
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/daqiri.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/daqiri.pc
  @ONLY
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/daqiriConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/daqiriConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/daqiri
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/daqiri.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  COMPONENT daqiri-cpp
)
