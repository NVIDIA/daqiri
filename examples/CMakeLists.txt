cmake_minimum_required(VERSION 3.20)

project(daqiri_bench_examples LANGUAGES CXX)

find_package(CUDAToolkit REQUIRED)

if(TARGET yaml-cpp::yaml-cpp)
  set(DAQIRI_EXAMPLE_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(DAQIRI_EXAMPLE_YAML_TARGET yaml-cpp)
endif()

set(DAQIRI_BENCH_CONFIGS
  daqiri_bench_raw_tx_rx.yaml
  daqiri_bench_raw_tx_rx_4q.yaml
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
    foreach(mgr IN ITEMS dpdk socket rdma)
      if(TARGET daqiri_${mgr})
        list(APPEND DAQIRI_EXAMPLE_STATIC_LIBS daqiri_${mgr})
      endif()
    endforeach()
    # Static common/backend archives reference each other; group them so GNU ld rescans.
    target_link_libraries(${target} PRIVATE
      "-Wl,--start-group"
      ${DAQIRI_EXAMPLE_STATIC_LIBS}
      "-Wl,--end-group"
      ${DAQIRI_EXAMPLE_YAML_TARGET}
    )
  else()
    target_link_libraries(${target} PRIVATE daqiri::daqiri ${DAQIRI_EXAMPLE_YAML_TARGET})
  endif()
endfunction()

function(add_daqiri_raw_bench target source)
  add_executable(${target} ${source} raw_bench_common.cpp)
  link_daqiri_bench(${target})
  target_link_libraries(${target} PRIVATE CUDA::cudart)
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "$ORIGIN/../src;$ORIGIN/../src/third_party/yaml-cpp"
  )
endfunction()

add_daqiri_raw_bench(daqiri_bench_raw_hds raw_hds_bench.cpp)
add_daqiri_raw_bench(daqiri_bench_raw_gpudirect raw_gpudirect_bench.cpp)
add_daqiri_raw_bench(daqiri_bench_raw_reorder_seq raw_reorder_seq_bench.cpp)
add_daqiri_raw_bench(daqiri_bench_raw_reorder_quantize raw_reorder_quantize_bench.cpp)
add_daqiri_raw_bench(daqiri_example_gds_write gds_write_example.cpp)
add_daqiri_raw_bench(daqiri_example_pcap_writer pcap_writer_example.cpp)

add_executable(daqiri_bench_rdma rdma_bench.cpp)
link_daqiri_bench(daqiri_bench_rdma)
set_target_properties(daqiri_bench_rdma PROPERTIES
  BUILD_RPATH "$ORIGIN/../src;$ORIGIN/../src/third_party/yaml-cpp"
)

add_executable(daqiri_bench_socket socket_bench.cpp)
link_daqiri_bench(daqiri_bench_socket)

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
        RUNTIME DESTINATION bin)
install(FILES ${DAQIRI_BENCH_CONFIGS}
        DESTINATION bin)
install(FILES ${DAQIRI_PYTHON_EXAMPLES}
        DESTINATION bin
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ
                    WORLD_EXECUTE WORLD_READ)
