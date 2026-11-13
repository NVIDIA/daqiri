/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2016 6WIND S.A.
 * Copyright 2016 Mellanox Technologies, Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
// PRM layouts from DPDK v24.11 drivers/common/mlx5/mlx5_prm.h.
#pragma once
#include "../src/engines/ibverbs/mlx5_prm_min.h"
struct mlx5_ifc_tisc_bits {
  uint8_t strict_lag_tx_port_affinity[0x1];
  uint8_t reserved_at_1[0x3];
  uint8_t lag_tx_port_affinity[0x04];
  uint8_t reserved_at_8[0x4];
  uint8_t prio[0x4];
  uint8_t reserved_at_10[0x10];
  uint8_t reserved_at_20[0x100];
  uint8_t reserved_at_120[0x8];
  uint8_t transport_domain[0x18];
  uint8_t reserved_at_140[0x8];
  uint8_t underlay_qpn[0x18];
  uint8_t reserved_at_160[0x3a0];
};
struct mlx5_ifc_create_tis_in_bits {
  uint8_t opcode[0x10];
  uint8_t uid[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t reserved_at_40[0xc0];
  struct mlx5_ifc_tisc_bits ctx;
};
struct mlx5_ifc_create_tis_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x8];
  uint8_t tisn[0x18];
  uint8_t reserved_at_60[0x20];
};
struct mlx5_ifc_sqc_bits {
  uint8_t rlky[0x1];
  uint8_t cd_master[0x1];
  uint8_t fre[0x1];
  uint8_t flush_in_error_en[0x1];
  uint8_t allow_multi_pkt_send_wqe[0x1];
  uint8_t min_wqe_inline_mode[0x3];
  uint8_t state[0x4];
  uint8_t reg_umr[0x1];
  uint8_t allow_swp[0x1];
  uint8_t hairpin[0x1];
  uint8_t non_wire[0x1];
  uint8_t static_sq_wq[0x1];
  uint8_t reserved_at_11[0x4];
  uint8_t hairpin_wq_buffer_type[0x3];
  uint8_t reserved_at_18[0x2];
  uint8_t ts_format[0x02];
  uint8_t reserved_at_1c[0x4];
  uint8_t reserved_at_20[0x8];
  uint8_t user_index[0x18];
  uint8_t reserved_at_40[0x8];
  uint8_t cqn[0x18];
  uint8_t reserved_at_60[0x8];
  uint8_t hairpin_peer_rq[0x18];
  uint8_t reserved_at_80[0x10];
  uint8_t hairpin_peer_vhca[0x10];
  uint8_t reserved_at_a0[0x50];
  uint8_t packet_pacing_rate_limit_index[0x10];
  uint8_t tis_lst_sz[0x10];
  uint8_t reserved_at_110[0x10];
  uint8_t reserved_at_120[0x40];
  uint8_t reserved_at_160[0x8];
  uint8_t tis_num_0[0x18];
  struct mlx5_ifc_wq_bits wq;
};
struct mlx5_ifc_create_sq_in_bits {
  uint8_t opcode[0x10];
  uint8_t uid[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t reserved_at_40[0xc0];
  struct mlx5_ifc_sqc_bits ctx;
};
struct mlx5_ifc_create_sq_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x8];
  uint8_t sqn[0x18];
  uint8_t reserved_at_60[0x20];
};
struct mlx5_ifc_modify_sq_in_bits {
  uint8_t opcode[0x10];
  uint8_t uid[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t sq_state[0x4];
  uint8_t reserved_at_44[0x4];
  uint8_t sqn[0x18];
  uint8_t reserved_at_60[0x20];
  uint8_t modify_bitmask[0x40];
  uint8_t reserved_at_c0[0x40];
  struct mlx5_ifc_sqc_bits ctx;
};
struct mlx5_ifc_modify_sq_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x40];
};
