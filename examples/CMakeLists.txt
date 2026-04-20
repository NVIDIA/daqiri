cmake_minimum_required(VERSION 3.20)

project(daqiri_bench_examples LANGUAGES CXX)

if(TARGET yaml-cpp::yaml-cpp)
  set(DAQIRI_EXAMPLE_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(DAQIRI_EXAMPLE_YAML_TARGET yaml-cpp)
endif()

set(DAQIRI_BENCH_CONFIGS
  daqiri_bench_raw_tx_rx.yaml
  daqiri_bench_raw_tx_rx_hds.yaml
  daqiri_bench_raw_rx_multi_q.yaml
  daqiri_bench_raw_sw_loopback.yaml
  daqiri_bench_rdma_tx_rx.yaml
  daqiri_bench_socket_udp_tx_rx.yaml
  daqiri_bench_socket_tcp_tx_rx.yaml
)

add_executable(daqiri_bench_raw raw_bench.cpp)
target_compile_features(daqiri_bench_raw PRIVATE cxx_std_17)
target_link_libraries(daqiri_bench_raw PRIVATE daqiri::daqiri ${DAQIRI_EXAMPLE_YAML_TARGET})

add_executable(daqiri_bench_rdma rdma_bench.cpp)
target_compile_features(daqiri_bench_rdma PRIVATE cxx_std_17)
target_link_libraries(daqiri_bench_rdma PRIVATE daqiri::daqiri ${DAQIRI_EXAMPLE_YAML_TARGET})

add_executable(daqiri_bench_socket socket_bench.cpp)
target_compile_features(daqiri_bench_socket PRIVATE cxx_std_17)
target_link_libraries(daqiri_bench_socket PRIVATE daqiri::daqiri ${DAQIRI_EXAMPLE_YAML_TARGET})

foreach(cfg IN LISTS DAQIRI_BENCH_CONFIGS)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${cfg} ${CMAKE_CURRENT_BINARY_DIR}/${cfg} COPYONLY)
endforeach()
