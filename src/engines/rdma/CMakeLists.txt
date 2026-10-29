# SPDX-FileCopyrightText: Copyright (c) 2025-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

message(STATUS "PROJECT_NAME: ${ENGINE_TARGET}")

target_include_directories(${ENGINE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(${ENGINE_TARGET} PRIVATE daqiri_rdma_engine.cpp rdma_direct.cpp)

pkg_check_modules(IBVERBS libibverbs)
pkg_check_modules(RDMACM librdmacm)
pkg_check_modules(RDMA_MLX5 libmlx5)
if(NOT RDMA_MLX5_FOUND)
    find_library(RDMA_MLX5_LIBRARIES NAMES mlx5)
    find_path(RDMA_MLX5_INCLUDE_DIRS NAMES infiniband/mlx5dv.h)
endif()
if(NOT RDMA_MLX5_LIBRARIES)
    message(FATAL_ERROR "RDMA accurate send requires libmlx5 (mlx5dv.h)")
endif()

if(NOT IBVERBS_FOUND)
    find_library(IBVERBS_LIBRARIES
        NAMES ibverbs
        PATHS /usr/lib /usr/lib64
    )

    find_path(IBVERBS_INCLUDE_DIRS
        NAMES infiniband/verbs.h
        PATHS /usr/include
    )
endif()

if(NOT RDMACM_FOUND)
    find_library(RDMACM_LIBRARIES
        NAMES rdmacm
        PATHS /usr/lib /usr/lib64
    )
endif()

if(NOT IBVERBS_LIBRARIES OR NOT RDMACM_LIBRARIES)
    message(FATAL_ERROR "Required RDMA libraries not found")
endif()


# The rdma engine no longer depends on DPDK: its rings/pools are the
# libdpdk-free daqiri::Ring / daqiri::ObjectPool.

# ibverbs/rdmacm: PUBLIC (daqiri_rdma_engine.h includes <infiniband/verbs.h> and <rdma/rdma_cma.h>)
target_include_directories(${ENGINE_TARGET} PUBLIC ${IBVERBS_INCLUDE_DIRS})
target_link_directories(${ENGINE_TARGET} PUBLIC ${IBVERBS_LIBRARY_DIRS})

target_sources(${ENGINE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rdma/cm_api.cpp)
if(DAQIRI_IBVERBS_DLOPEN)
    target_compile_definitions(${ENGINE_TARGET} PRIVATE DAQIRI_IBVERBS_DLOPEN=1)
    target_link_libraries(${ENGINE_TARGET} PRIVATE ${CMAKE_DL_LIBS})
    # Always a DSO, even when DAQIRI itself is built as static archives.
    add_library(daqiri_cm_verbs_glue SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../../rdma/cm_glue.cpp)
    target_include_directories(daqiri_cm_verbs_glue PRIVATE ${IBVERBS_INCLUDE_DIRS} ${RDMACM_INCLUDE_DIRS})
    target_link_directories(daqiri_cm_verbs_glue PRIVATE ${IBVERBS_LIBRARY_DIRS} ${RDMACM_LIBRARY_DIRS})
    target_include_directories(daqiri_cm_verbs_glue PRIVATE ${RDMA_MLX5_INCLUDE_DIRS})
    target_link_directories(daqiri_cm_verbs_glue PRIVATE ${RDMA_MLX5_LIBRARY_DIRS})
    target_link_libraries(daqiri_cm_verbs_glue PRIVATE ${IBVERBS_LIBRARIES} ${RDMACM_LIBRARIES} ${RDMA_MLX5_LIBRARIES})
    add_dependencies(${ENGINE_TARGET} daqiri_cm_verbs_glue)
    target_compile_features(daqiri_cm_verbs_glue PRIVATE cxx_std_17)
    set_target_properties(daqiri_cm_verbs_glue PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        SOVERSION 2 VERSION 2.0.0)
    install(TARGETS daqiri_cm_verbs_glue
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT daqiri-cpp)
else()
    target_compile_definitions(${ENGINE_TARGET} PRIVATE DAQIRI_IBVERBS_DLOPEN=0)
    target_sources(${ENGINE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rdma/cm_glue.cpp)
    target_include_directories(${ENGINE_TARGET} PRIVATE ${RDMA_MLX5_INCLUDE_DIRS})
    target_link_directories(${ENGINE_TARGET} PUBLIC ${RDMA_MLX5_LIBRARY_DIRS})
    target_link_libraries(${ENGINE_TARGET} PUBLIC ${IBVERBS_LIBRARIES} ${RDMACM_LIBRARIES} ${RDMA_MLX5_LIBRARIES})
endif()

if(BUILD_TESTING AND UNIX AND NOT APPLE)
    add_executable(daqiri_rdma_timed_send_test
        ${CMAKE_SOURCE_DIR}/tests/cpp/rdma_timed_send_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../rdma/cm_glue.cpp)
    target_compile_features(daqiri_rdma_timed_send_test PRIVATE cxx_std_17)
    target_include_directories(daqiri_rdma_timed_send_test PRIVATE ${CMAKE_SOURCE_DIR}
        ${IBVERBS_INCLUDE_DIRS} ${RDMACM_INCLUDE_DIRS} ${RDMA_MLX5_INCLUDE_DIRS})
    target_link_directories(daqiri_rdma_timed_send_test PRIVATE
        ${IBVERBS_LIBRARY_DIRS} ${RDMACM_LIBRARY_DIRS} ${RDMA_MLX5_LIBRARY_DIRS})
    target_link_libraries(daqiri_rdma_timed_send_test PRIVATE
        ${IBVERBS_LIBRARIES} ${RDMACM_LIBRARIES} ${RDMA_MLX5_LIBRARIES})
    target_link_options(daqiri_rdma_timed_send_test PRIVATE
        "LINKER:--wrap=ibv_qp_to_qp_ex" "LINKER:--wrap=mlx5dv_qp_ex_from_ibv_qp_ex")
    foreach(symbol IN ITEMS mlx5dv_is_supported mlx5dv_devx_general_cmd mlx5dv_create_qp
            ibv_query_ece ibv_set_ece rdma_get_remote_ece rdma_set_local_ece
            rdma_init_qp_attr ibv_modify_qp ibv_destroy_qp)
        target_link_options(daqiri_rdma_timed_send_test PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
    add_test(NAME daqiri_rdma_timed_send_test COMMAND daqiri_rdma_timed_send_test)
endif()
