# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_executable(daqiri_test_gvsp
  gvsp_frame_assembler_test.cpp
  ../../examples/gvsp/ethernet_udp_view.cpp
  ../../examples/gvsp/gvsp_packet.cpp
  ../../examples/gvsp/frame_assembler.cpp
)
target_compile_features(daqiri_test_gvsp PRIVATE cxx_std_17)
target_include_directories(daqiri_test_gvsp PRIVATE ../../examples/gvsp)
add_test(NAME daqiri_test_gvsp COMMAND daqiri_test_gvsp)
