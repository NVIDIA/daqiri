cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 80 90)
  set(DAQIRI_EXAMPLES_USING_DEFAULT_CUDA_ARCHITECTURES ON)
endif()

project(daqiri_bench_examples LANGUAGES CXX CUDA)

find_package(CUDAToolkit REQUIRED)

if(DAQIRI_EXAMPLES_USING_DEFAULT_CUDA_ARCHITECTURES AND
   CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
  list(APPEND CMAKE_CUDA_ARCHITECTURES 121)
endif()

set(DAQIRI_EXAMPLES_CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

if(NOT DAQIRI_YAML_TARGET)
  # Standalone build (not added by the top-level CMakeLists): mirror the
  # src/CMakeLists.txt resolution so a system yaml-cpp or the vendored
  # submodule both work.
  find_package(yaml-cpp QUIET)
  if(TARGET yaml-cpp::yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp::yaml-cpp)
  elseif(TARGET yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp)
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/yaml-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/third_party/yaml-cpp)
    set(DAQIRI_YAML_TARGET yaml-cpp)
  else()
    message(FATAL_ERROR
      "yaml-cpp not found; install yaml-cpp-dev or run "
      "`git submodule update --init third_party/yaml-cpp`")
  endif()
endif()

set(DAQIRI_BENCH_CONFIGS
  daqiri_bench_raw_tx_rx.yaml
  daqiri_bench_raw_tx_rx_4q.yaml
  daqiri_bench_raw_tx_rx_pacing.yaml
  daqiri_bench_raw_tx_rx_hds.yaml
  daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml
  daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml
  daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml
  daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml
  daqiri_bench_raw_rx_reorder_seq_ppb.yaml
  daqiri_bench_raw_rx_reorder_seq_batch.yaml
  daqiri_bench_raw_rx_multi_q.yaml
  daqiri_bench_raw_sw_loopback.yaml
  daqiri_example_gds_write_sw_loopback.yaml
  daqiri_example_gds_write_tx_rx.yaml
  daqiri_example_pcap_writer_sw_loopback.yaml
  daqiri_example_pcap_writer_tx_rx.yaml
  daqiri_bench_rdma_tx_rx.yaml
  daqiri_bench_socket_udp_tx_rx.yaml
  daqiri_bench_socket_tcp_tx_rx.yaml
)

set(DAQIRI_PYTHON_EXAMPLES
  daqiri_bench_raw_tx_rx.py
)

function(link_daqiri_bench target)
  target_compile_features(${target} PRIVATE cxx_std_17)
  if(NOT BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
    set(DAQIRI_EXAMPLE_STATIC_LIBS daqiri::daqiri)
    foreach(engine IN ITEMS dpdk socket rdma)
      if(TARGET daqiri_${engine})
        list(APPEND DAQIRI_EXAMPLE_STATIC_LIBS daqiri_${engine})
      endif()
    endforeach()
    # Static common/engine archives reference each other; group them so GNU ld rescans.
    target_link_libraries(${target} PRIVATE
      "-Wl,--start-group"
      ${DAQIRI_EXAMPLE_STATIC_LIBS}
      "-Wl,--end-group"
      ${DAQIRI_YAML_TARGET}
    )
  else()
    target_link_libraries(${target} PRIVATE daqiri::daqiri ${DAQIRI_YAML_TARGET})
  endif()
endfunction()

function(add_daqiri_raw_bench target source)
  add_executable(${target} ${source} raw_bench_common.cpp raw_bench_common_cuda.cu)
  link_daqiri_bench(${target})
  target_link_libraries(${target} PRIVATE CUDA::cudart)
  set_target_properties(${target} PROPERTIES
    CUDA_ARCHITECTURES "${DAQIRI_EXAMPLES_CUDA_ARCHITECTURES}"
    BUILD_RPATH "$ORIGIN/../src;$ORIGIN/../src/third_party/yaml-cpp"
  )
endfunction()

add_daqiri_raw_bench(daqiri_bench_raw_hds raw_hds_bench.cpp)
add_daqiri_raw_bench(daqiri_bench_raw_gpudirect raw_gpudirect_bench.cpp)
if(DAQIRI_ENABLE_OTEL_METRICS)
  find_package(opentelemetry-cpp CONFIG QUIET COMPONENTS sdk exporters_prometheus)
  if(TARGET opentelemetry-cpp::prometheus_exporter)
    target_sources(daqiri_bench_raw_gpudirect PRIVATE grafana/otel_prometheus.cpp)
    target_compile_definitions(daqiri_bench_raw_gpudirect PRIVATE DAQIRI_GRAFANA_PROMETHEUS=1)
    target_link_libraries(daqiri_bench_raw_gpudirect PRIVATE
      opentelemetry-cpp::sdk
      opentelemetry-cpp::prometheus_exporter
    )
  else()
    message(STATUS "OpenTelemetry Prometheus exporter not found; Grafana live metrics example disabled")
  endif()
endif()
add_daqiri_raw_bench(daqiri_bench_raw_reorder_seq raw_reorder_seq_bench.cpp)
add_daqiri_raw_bench(daqiri_bench_raw_reorder_quantize raw_reorder_quantize_bench.cpp)
add_daqiri_raw_bench(daqiri_example_gds_write gds_write_example.cpp)
add_daqiri_raw_bench(daqiri_example_pcap_writer pcap_writer_example.cpp)

add_executable(daqiri_bench_rdma rdma_bench.cpp raw_bench_common.cpp)
link_daqiri_bench(daqiri_bench_rdma)
target_link_libraries(daqiri_bench_rdma PRIVATE CUDA::cudart)
set_target_properties(daqiri_bench_rdma PROPERTIES
  BUILD_RPATH "$ORIGIN/../src;$ORIGIN/../src/third_party/yaml-cpp"
)

add_executable(daqiri_bench_socket socket_bench.cpp raw_bench_common.cpp)
link_daqiri_bench(daqiri_bench_socket)
target_link_libraries(daqiri_bench_socket PRIVATE CUDA::cudart)
set_target_properties(daqiri_bench_socket PROPERTIES
  BUILD_RPATH "$ORIGIN/../src;$ORIGIN/../src/third_party/yaml-cpp"
)

# Microbenchmark for the daqiri::Ring / daqiri::ObjectPool primitives. Always
# built (the primitives are header-only); when the dpdk engine is present it also
# compiles the rte_ring / rte_mempool comparison arm.
add_executable(daqiri_pool_ring_bench pool_ring_bench.cpp)
target_compile_features(daqiri_pool_ring_bench PRIVATE cxx_std_17)
target_include_directories(daqiri_pool_ring_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(Threads REQUIRED)
target_link_libraries(daqiri_pool_ring_bench PRIVATE Threads::Threads)
if(TARGET daqiri_dpdk)
  find_package(PkgConfig)
  pkg_check_modules(BENCH_DPDK QUIET libdpdk)
  if(NOT BENCH_DPDK_FOUND)
    list(APPEND CMAKE_PREFIX_PATH "/opt/mellanox/dpdk/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu/")
    pkg_check_modules(BENCH_DPDK QUIET libdpdk)
  endif()
  if(BENCH_DPDK_FOUND)
    target_compile_definitions(daqiri_pool_ring_bench PRIVATE BENCH_HAVE_DPDK=1)
    target_include_directories(daqiri_pool_ring_bench PRIVATE ${BENCH_DPDK_INCLUDE_DIRS})
    target_link_directories(daqiri_pool_ring_bench PRIVATE ${BENCH_DPDK_LIBRARY_DIRS})
    target_link_libraries(daqiri_pool_ring_bench PRIVATE ${BENCH_DPDK_LDFLAGS_OTHER} ${BENCH_DPDK_LIBRARIES})
  endif()
endif()

foreach(cfg IN LISTS DAQIRI_BENCH_CONFIGS)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${cfg} ${CMAKE_CURRENT_BINARY_DIR}/${cfg} COPYONLY)
endforeach()

foreach(example IN LISTS DAQIRI_PYTHON_EXAMPLES)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${example} ${CMAKE_CURRENT_BINARY_DIR}/${example} COPYONLY)
endforeach()

install(TARGETS
        daqiri_bench_raw_hds
        daqiri_bench_raw_gpudirect
        daqiri_bench_raw_reorder_seq
        daqiri_bench_raw_reorder_quantize
        daqiri_example_gds_write
        daqiri_example_pcap_writer
        daqiri_bench_rdma
        daqiri_bench_socket
        daqiri_pool_ring_bench
        RUNTIME DESTINATION bin)
install(FILES ${DAQIRI_BENCH_CONFIGS}
        DESTINATION bin)
install(FILES ${DAQIRI_PYTHON_EXAMPLES}
        DESTINATION bin
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ
                    WORLD_EXECUTE WORLD_READ)
