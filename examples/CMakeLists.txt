cmake_minimum_required(VERSION 3.20)

project(daqiri_bench_examples LANGUAGES CXX)

if(TARGET yaml-cpp::yaml-cpp)
  set(DAQIRI_EXAMPLE_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(DAQIRI_EXAMPLE_YAML_TARGET yaml-cpp)
endif()

set(DAQIRI_BENCH_CONFIGS
  daqiri_bench_default_tx_rx.yaml
  daqiri_bench_default_tx_rx_doca_eth.yaml
  daqiri_bench_default_tx_rx_hds.yaml
  daqiri_bench_default_tx_rx_hds_doca_eth.yaml
  daqiri_bench_default_rx_multi_q.yaml
  daqiri_bench_default_rx_multi_q_doca_eth.yaml
  daqiri_bench_default_sw_loopback.yaml
  daqiri_bench_default_sw_loopback_doca_eth.yaml
  daqiri_bench_rdma_tx_rx.yaml
)

add_executable(daqiri_bench_default default_bench.cpp)
target_compile_features(daqiri_bench_default PRIVATE cxx_std_17)
target_link_libraries(daqiri_bench_default PRIVATE daqiri::daqiri ${DAQIRI_EXAMPLE_YAML_TARGET})

add_executable(daqiri_bench_rdma rdma_bench.cpp)
target_compile_features(daqiri_bench_rdma PRIVATE cxx_std_17)
target_link_libraries(daqiri_bench_rdma PRIVATE daqiri::daqiri ${DAQIRI_EXAMPLE_YAML_TARGET})

foreach(cfg IN LISTS DAQIRI_BENCH_CONFIGS)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${cfg} ${CMAKE_CURRENT_BINARY_DIR}/${cfg} COPYONLY)
endforeach()
