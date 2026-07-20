/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Minimal subset of the mlx5 Programmer's Reference Manual (PRM) command-buffer
// struct definitions needed to create a Multi-Packet (striding) Receive Queue
// and its TIR via DevX. The "1 byte per bit" sentinel-struct layout matches the
// DEVX_SET/DEVX_GET macros in <infiniband/mlx5dv.h> (offsetof/sizeof yield bit
// offsets/sizes). Field layouts are copied verbatim from DPDK's proven
// drivers/common/mlx5/mlx5_prm.h so they match this NIC's firmware exactly.
//
// The structs MUST live in the global namespace and use the exact tag
// `mlx5_ifc_<typ>_bits`, because DEVX_SET(typ, ...) expands to an unqualified
// `struct mlx5_ifc_##typ##_bits`.

#pragma once

#include <cstdint>

// ---- command opcodes ----
enum {
  MLX5_CMD_OP_QUERY_HCA_CAP = 0x100,
  MLX5_CMD_OP_CREATE_CQ = 0x400,
  MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN = 0x816,
  MLX5_CMD_OP_CREATE_TIR = 0x900,
  MLX5_CMD_OP_CREATE_RQ = 0x908,
  MLX5_CMD_OP_MODIFY_RQ = 0x909,
  MLX5_CMD_OP_CREATE_GENERAL_OBJECT = 0xa00,
  MLX5_CMD_OP_QUERY_GENERAL_OBJECT = 0xa02,
};

// Flex-parser (parse-graph) constants for arbitrary-offset flow matching.
enum {
  MLX5_GENERAL_OBJ_TYPE_FLEX_PARSE_GRAPH = 0x0022,
  MLX5_GRAPH_NODE_LEN_FIXED = 0x0,
  MLX5_GRAPH_ARC_NODE_MAC = 0x2,  // anchor at L2; compare value = ethertype
  MLX5_GRAPH_ARC_NODE_UDP = 0x5,  // anchor after UDP; compare value = udp dst port
  MLX5_GRAPH_SAMPLE_OFFSET_FIXED = 0x0,
};

// QUERY_HCA_CAP op_mod: general device caps, current values.
enum {
  MLX5_HCA_CAP_OPMOD_GENERAL_CUR = (0x0 << 1) | 0x1,
  MLX5_HCA_CAP_OPMOD_SHAMPO_CUR = (0x1d << 1) | 0x1,
};

// ---- HCA capability probe (wait_on_time for accurate TX send scheduling) ----
struct mlx5_ifc_query_hca_cap_in_bits {
  uint8_t opcode[0x10];
  uint8_t reserved_at_10[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t reserved_at_40[0x40];
};

// Minimal cmd_hca_cap: only the fields we read. device_frequency_khz and
// wait_on_time are placed at their exact PRM bit offsets via sized reserved
// prefixes (DEVX_GET uses offsetof, so the intervening fields need not be named
// -- only the cumulative offset must match DPDK's mlx5_ifc_cmd_hca_cap_bits).
struct mlx5_ifc_cmd_hca_cap_min_bits {
  uint8_t reserved_at_0[0xba];
  uint8_t shampo[0x1];  // general-cap bit advertising the SHAMPO cap block
  uint8_t reserved_at_bb[0x365];
  uint8_t general_obj_types[0x40];  // at bit 0x420; bit 0x22 = FLEX_PARSE_GRAPH
  uint8_t reserved_at_460[0x80];
  uint8_t device_frequency_khz[0x20];  // at bit 0x4e0
  uint8_t reserved_at_500[0x18b];
  uint8_t wait_on_data[0x1];  // at bit 0x68b
  uint8_t wait_on_time[0x1];  // at bit 0x68c
  // Pad to the full HCA-cap blob size (0x8000 bits) so the QUERY_HCA_CAP output
  // buffer is large enough for the firmware to write the whole capability.
  uint8_t reserved_at_68d[0x7973];
};

struct mlx5_ifc_query_hca_cap_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x40];
  struct mlx5_ifc_cmd_hca_cap_min_bits capability;
};

// SHAMPO is a separate QUERY_HCA_CAP capability block (cap type 0x1d). Only
// the fields required for HEADER_SPLIT_DATA_MERGE are named here.
struct mlx5_ifc_shampo_cap_min_bits {
  uint8_t reserved_at_0[0x3];
  uint8_t shampo_log_max_reservation_size[0x5];
  uint8_t reserved_at_8[0x3];
  uint8_t shampo_log_min_reservation_size[0x5];
  uint8_t shampo_min_mss_size[0x10];
  uint8_t shampo_header_split[0x1];
  uint8_t shampo_header_split_data_merge[0x1];
  uint8_t reserved_at_22[0x1];
  uint8_t shampo_log_max_headers_entry_size[0x5];
  uint8_t reserved_at_28[0x7fd8];
};

struct mlx5_ifc_query_shampo_cap_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x40];
  struct mlx5_ifc_shampo_cap_min_bits capability;
};
enum {
  MLX5_CQE_SIZE_64B = 0x0,
  MLX5_CQE_SIZE_128B = 0x1,
};

// ---- WQ / RQ / TIR enums ----
enum {
  MLX5_WQ_TYPE_CYCLIC = 0x1,
  MLX5_WQ_TYPE_CYCLIC_STRIDING_RQ = 0x3,
};
enum {
  MLX5_RQC_STATE_RST = 0x0,
  MLX5_RQC_STATE_RDY = 0x1,
};
enum {
  MLX5_TIRC_DISP_TYPE_DIRECT = 0x0,
};
enum {
  MLX5_RX_HASH_FN_NONE = 0x0,
};
enum {
  MLX5_MODIFY_RQ_RST2RDY = 0x1,
};
enum {
  MLX5_RQC_TIMESTAMP_FORMAT_FREE_RUNNING = 0x0,
  MLX5_RQC_TIMESTAMP_FORMAT_DEFAULT = 0x1,
  MLX5_RQC_TIMESTAMP_FORMAT_REAL_TIME = 0x2,
};

// Firmware biases for the striding parameters (the field stores log - min).
static constexpr uint32_t MLX5_MIN_SINGLE_WQE_LOG_NUM_STRIDES = 9u;
static constexpr uint32_t MLX5_MIN_SINGLE_STRIDE_LOG_NUM_BYTES = 6u;

struct mlx5_ifc_wq_bits {
  uint8_t wq_type[0x4];
  uint8_t wq_signature[0x1];
  uint8_t end_padding_mode[0x2];
  uint8_t cd_slave[0x1];
  uint8_t reserved_at_8[0x18];
  uint8_t hds_skip_first_sge[0x1];
  uint8_t log2_hds_buf_size[0x3];
  uint8_t reserved_at_24[0x7];
  uint8_t page_offset[0x5];
  uint8_t lwm[0x10];
  uint8_t reserved_at_40[0x8];
  uint8_t pd[0x18];
  uint8_t reserved_at_60[0x8];
  uint8_t uar_page[0x18];
  uint8_t dbr_addr[0x40];
  uint8_t hw_counter[0x20];
  uint8_t sw_counter[0x20];
  uint8_t reserved_at_100[0xc];
  uint8_t log_wq_stride[0x4];
  uint8_t reserved_at_110[0x3];
  uint8_t log_wq_pg_sz[0x5];
  uint8_t reserved_at_118[0x3];
  uint8_t log_wq_sz[0x5];
  uint8_t dbr_umem_valid[0x1];
  uint8_t wq_umem_valid[0x1];
  uint8_t reserved_at_122[0x1];
  uint8_t log_hairpin_num_packets[0x5];
  uint8_t reserved_at_128[0x3];
  uint8_t log_hairpin_data_sz[0x5];
  uint8_t reserved_at_130[0x4];
  uint8_t single_wqe_log_num_of_strides[0x4];
  uint8_t two_byte_shift_en[0x1];
  uint8_t reserved_at_139[0x4];
  uint8_t single_stride_log_num_of_bytes[0x3];
  uint8_t dbr_umem_id[0x20];
  uint8_t wq_umem_id[0x20];
  uint8_t wq_umem_offset[0x40];
  uint8_t headers_mkey[0x20];
  uint8_t shampo_enable[0x1];
  uint8_t reserved_at_1e1[0x1];
  uint8_t shampo_mode[0x2];
  uint8_t reserved_at_1e4[0x1];
  uint8_t log_reservation_size[0x3];
  uint8_t reserved_at_1e8[0x5];
  uint8_t log_max_num_of_packets_per_reservation[0x3];
  uint8_t reserved_at_1f0[0x6];
  uint8_t log_headers_entry_size[0x2];
  uint8_t reserved_at_1f8[0x4];
  uint8_t log_headers_buffer_entry_num[0x4];
  uint8_t reserved_at_200[0x400];
};

struct mlx5_ifc_rqc_bits {
  uint8_t rlky[0x1];
  uint8_t delay_drop_en[0x1];
  uint8_t scatter_fcs[0x1];
  uint8_t vsd[0x1];
  uint8_t mem_rq_type[0x4];
  uint8_t state[0x4];
  uint8_t reserved_at_c[0x1];
  uint8_t flush_in_error_en[0x1];
  uint8_t hairpin[0x1];
  uint8_t reserved_at_f[0x6];
  uint8_t hairpin_data_buffer_type[0x3];
  uint8_t reserved_at_a8[0x2];
  uint8_t ts_format[0x02];
  uint8_t reserved_at_1c[0x4];
  uint8_t reserved_at_20[0x8];
  uint8_t user_index[0x18];
  uint8_t reserved_at_40[0x8];
  uint8_t cqn[0x18];
  uint8_t counter_set_id[0x8];
  uint8_t reserved_at_68[0x18];
  uint8_t reserved_at_80[0x8];
  uint8_t rmpn[0x18];
  uint8_t reserved_at_a0[0x8];
  uint8_t hairpin_peer_sq[0x18];
  uint8_t reserved_at_c0[0x10];
  uint8_t hairpin_peer_vhca[0x10];
  uint8_t reserved_at_e0[0x46];
  uint8_t shampo_no_match_alignment_granularity[0x2];
  uint8_t reserved_at_128[0x6];
  uint8_t shampo_match_criteria_type[0x2];
  uint8_t reservation_timeout[0x10];
  uint8_t reserved_at_140[0x40];
  struct mlx5_ifc_wq_bits wq;
};

struct mlx5_ifc_create_rq_in_bits {
  uint8_t opcode[0x10];
  uint8_t uid[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t reserved_at_40[0xc0];
  struct mlx5_ifc_rqc_bits ctx;
};

struct mlx5_ifc_create_rq_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x8];
  uint8_t rqn[0x18];
  uint8_t reserved_at_60[0x20];
};

struct mlx5_ifc_modify_rq_in_bits {
  uint8_t opcode[0x10];
  uint8_t uid[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t rq_state[0x4];
  uint8_t reserved_at_44[0x4];
  uint8_t rqn[0x18];
  uint8_t reserved_at_60[0x20];
  uint8_t modify_bitmask[0x40];
  uint8_t reserved_at_c0[0x40];
  struct mlx5_ifc_rqc_bits ctx;
};

struct mlx5_ifc_modify_rq_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x40];
};

struct mlx5_ifc_rx_hash_field_select_bits {
  uint8_t l3_prot_type[0x1];
  uint8_t l4_prot_type[0x1];
  uint8_t selected_fields[0x1e];
};

struct mlx5_ifc_tirc_bits {
  uint8_t reserved_at_0[0x20];
  uint8_t disp_type[0x4];
  uint8_t reserved_at_24[0x1c];
  uint8_t reserved_at_40[0x40];
  uint8_t reserved_at_80[0x4];
  uint8_t lro_timeout_period_usecs[0x10];
  uint8_t lro_enable_mask[0x4];
  uint8_t lro_max_msg_sz[0x8];
  uint8_t reserved_at_a0[0x40];
  uint8_t reserved_at_e0[0x8];
  uint8_t inline_rqn[0x18];
  uint8_t rx_hash_symmetric[0x1];
  uint8_t reserved_at_101[0x1];
  uint8_t tunneled_offload_en[0x1];
  uint8_t reserved_at_103[0x5];
  uint8_t indirect_table[0x18];
  uint8_t rx_hash_fn[0x4];
  uint8_t reserved_at_124[0x2];
  uint8_t self_lb_block[0x2];
  uint8_t transport_domain[0x18];
  uint8_t rx_hash_toeplitz_key[10][0x20];
  struct mlx5_ifc_rx_hash_field_select_bits rx_hash_field_selector_outer;
  struct mlx5_ifc_rx_hash_field_select_bits rx_hash_field_selector_inner;
  uint8_t reserved_at_2c0[0x4c0];
};

struct mlx5_ifc_create_tir_in_bits {
  uint8_t opcode[0x10];
  uint8_t uid[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t reserved_at_40[0xc0];
  struct mlx5_ifc_tirc_bits ctx;
};

struct mlx5_ifc_create_tir_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x8];
  uint8_t tirn[0x18];
  uint8_t reserved_at_60[0x20];
};

struct mlx5_ifc_cqc_bits {
  uint8_t status[0x4];
  uint8_t as_notify[0x1];
  uint8_t initiator_src_dct[0x1];
  uint8_t dbr_umem_valid[0x1];
  uint8_t ext_element[0x1];
  uint8_t cqe_sz[0x3];
  uint8_t cc[0x1];
  uint8_t reserved_at_c[0x1];
  uint8_t scqe_break_moderation_en[0x1];
  uint8_t oi[0x1];
  uint8_t cq_period_mode[0x2];
  uint8_t cqe_comp_en[0x1];
  uint8_t mini_cqe_res_format[0x2];
  uint8_t st[0x4];
  uint8_t always_armed_cq[0x1];
  uint8_t ext_element_type[0x3];
  uint8_t reserved_at_1c[0x2];
  uint8_t cqe_comp_layout[0x2];
  uint8_t dbr_umem_id[0x20];
  uint8_t reserved_at_40[0x14];
  uint8_t page_offset[0x6];
  uint8_t reserved_at_5a[0x2];
  uint8_t mini_cqe_res_format_ext[0x2];
  uint8_t cq_timestamp_format[0x2];
  uint8_t reserved_at_60[0x3];
  uint8_t log_cq_size[0x5];
  uint8_t uar_page[0x18];
  uint8_t reserved_at_80[0x4];
  uint8_t cq_period[0xc];
  uint8_t cq_max_count[0x10];
  uint8_t reserved_at_a0[0x18];
  uint8_t c_eqn[0x8];
  uint8_t reserved_at_c0[0x3];
  uint8_t log_page_size[0x5];
  uint8_t reserved_at_c8[0x18];
  uint8_t reserved_at_e0[0x20];
  uint8_t reserved_at_100[0x8];
  uint8_t last_notified_index[0x18];
  uint8_t reserved_at_120[0x8];
  uint8_t last_solicit_index[0x18];
  uint8_t reserved_at_140[0x8];
  uint8_t consumer_counter[0x18];
  uint8_t reserved_at_160[0x8];
  uint8_t producer_counter[0x18];
  uint8_t local_partition_id[0xc];
  uint8_t process_id[0x14];
  uint8_t reserved_at_1A0[0x20];
  uint8_t dbr_addr[0x40];
};

struct mlx5_ifc_create_cq_in_bits {
  uint8_t opcode[0x10];
  uint8_t uid[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t reserved_at_40[0x40];
  struct mlx5_ifc_cqc_bits cq_context;
  uint8_t cq_umem_offset[0x40];
  uint8_t cq_umem_id[0x20];
  uint8_t cq_umem_valid[0x1];
  uint8_t reserved_at_2e1[0x1f];
  uint8_t reserved_at_300[0x580];
};

struct mlx5_ifc_create_cq_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x8];
  uint8_t cqn[0x18];
  uint8_t reserved_at_60[0x20];
};

struct mlx5_ifc_alloc_transport_domain_in_bits {
  uint8_t opcode[0x10];
  uint8_t reserved_at_10[0x10];
  uint8_t reserved_at_20[0x10];
  uint8_t op_mod[0x10];
  uint8_t reserved_at_40[0x40];
};

struct mlx5_ifc_alloc_transport_domain_out_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t reserved_at_40[0x8];
  uint8_t transport_domain[0x18];
  uint8_t reserved_at_60[0x20];
};

// One mlx5 receive scatter entry. The RQ WQE for a striding RQ is a single such
// segment. All fields big-endian on the wire.
struct mlx5_wqe_data_seg_min {
  uint32_t byte_count;
  uint32_t lkey;
  uint64_t addr;
};

// ---- flow steering match parameters (mlx5dv_dr matchers/rules) ----
// Layer 2-4 outer-header match set. mlx5dv_dr interprets the 64-byte match
// buffer as this struct when match_criteria_enable selects outer_headers
// (bit 0). We DEVX_SET only the fields we steer on (ethertype, ip_protocol,
// udp/tcp ports, src/dst IPv4); the rest stay zero (wildcard in the mask).
struct mlx5_ifc_ipv4_layout_bits {
  uint8_t reserved_at_0[0x60];
  uint8_t ipv4[0x20];
};

struct mlx5_ifc_ipv6_layout_bits {
  uint8_t ipv6[16 * 0x8];
};

union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits {
  struct mlx5_ifc_ipv6_layout_bits ipv6_layout;
  struct mlx5_ifc_ipv4_layout_bits ipv4_layout;
  uint8_t reserved_at_0[0x80];
};

struct mlx5_ifc_fte_match_set_lyr_2_4_bits {
  uint8_t smac_47_16[0x20];
  uint8_t smac_15_0[0x10];
  uint8_t ethertype[0x10];
  uint8_t dmac_47_16[0x20];
  uint8_t dmac_15_0[0x10];
  uint8_t first_prio[0x3];
  uint8_t first_cfi[0x1];
  uint8_t first_vid[0xc];
  uint8_t ip_protocol[0x8];
  uint8_t ip_dscp[0x6];
  uint8_t ip_ecn[0x2];
  uint8_t cvlan_tag[0x1];
  uint8_t svlan_tag[0x1];
  uint8_t frag[0x1];
  uint8_t ip_version[0x4];
  uint8_t tcp_flags[0x9];
  uint8_t tcp_sport[0x10];
  uint8_t tcp_dport[0x10];
  uint8_t reserved_at_c0[0x10];
  uint8_t ipv4_ihl[0x4];
  uint8_t l3_ok[0x1];
  uint8_t l4_ok[0x1];
  uint8_t ipv4_checksum_ok[0x1];
  uint8_t l4_checksum_ok[0x1];
  uint8_t ip_ttl_hoplimit[0x8];
  uint8_t udp_sport[0x10];
  uint8_t udp_dport[0x10];
  union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits src_ipv4_src_ipv6;
  union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits dst_ipv4_dst_ipv6;
};

enum {
  MLX5_DR_MATCH_CRITERIA_OUTER = 1 << 0,  // match_criteria_enable: outer_headers
  MLX5_DR_MATCH_CRITERIA_MISC4 = 1 << 5,  // match_criteria_enable: misc_parameters_4
  MLX5_ETHERTYPE_IPV4 = 0x0800,
  MLX5_ETHERTYPE_ECPRI = 0xAEFE,  // eCPRI over Ethernet
  MLX5_IP_PROTOCOL_UDP = 0x11,
};

// misc_parameters_4 carries the flex-parser sample registers: each enabled
// sample matches by (prog_sample_field_id_N == sample_field_id) on
// prog_sample_field_value_N. One fte_match_set is 0x200 bits (64 bytes).
struct mlx5_ifc_fte_match_set_misc4_bits {
  uint8_t prog_sample_field_value_0[0x20];
  uint8_t prog_sample_field_id_0[0x20];
  uint8_t prog_sample_field_value_1[0x20];
  uint8_t prog_sample_field_id_1[0x20];
  uint8_t prog_sample_field_value_2[0x20];
  uint8_t prog_sample_field_id_2[0x20];
  uint8_t prog_sample_field_value_3[0x20];
  uint8_t prog_sample_field_id_3[0x20];
  uint8_t reserved_at_100[0x100];
};

// Full flow-steering match parameter. Each section is 0x200 bits (64 bytes);
// misc_parameters_4 lands at byte offset 320. We only name the sections we
// touch (outer_headers, misc_parameters_4); the rest are opaque padding so the
// fixed offsets line up for DEVX_ADDR_OF.
struct mlx5_ifc_fte_match_param_bits {
  struct mlx5_ifc_fte_match_set_lyr_2_4_bits outer_headers;
  uint8_t misc_parameters[0x200];
  uint8_t inner_headers[0x200];
  uint8_t misc_parameters_2[0x200];
  uint8_t misc_parameters_3[0x200];
  struct mlx5_ifc_fte_match_set_misc4_bits misc_parameters_4;
  uint8_t misc_parameters_5[0x200];
  uint8_t reserved_at_e00[0x200];
};

// ---- accurate send scheduling (wait-on-time WAIT WQE) ----
// A WAIT WQE = ctrl seg + wait seg; with WAIT_TIME op_mod the NIC holds the
// following send until its real-time clock reaches the wait segment's value.
enum {
  MLX5_OPCODE_WAIT = 0x0f,
  MLX5_OPC_MOD_WAIT_TIME = 2,
  MLX5_WAIT_COND_CYCLIC_SMALLER = 5,
  MLX5_OPCODE_ENHANCED_MPSW = 0x29,
  MLX5_EMPW_MAX_DSEG = 58,
};

// Wait segment (16 bytes of control + an 8-byte value + 8-byte mask = 32 B,
// i.e. 2 DS). All fields big-endian on the wire.
struct mlx5_wqe_wseg {
  uint32_t operation;  // condition (e.g. CYCLIC_SMALLER)
  uint32_t lkey;       // 0 for wait-on-time
  uint32_t va_high;    // 0 for wait-on-time
  uint32_t va_low;     // 0 for wait-on-time
  uint64_t value;      // timestamp to wait for (real-time: ns%1e9 | (ns/1e9)<<32)
  uint64_t mask;       // comparison mask
};

// ---- flex parser (parse-graph node) for arbitrary-offset flow matching ----
struct mlx5_ifc_general_obj_in_cmd_hdr_bits {
  uint8_t opcode[0x10];
  uint8_t reserved_at_10[0x20];
  uint8_t obj_type[0x10];
  uint8_t obj_id[0x20];
  uint8_t reserved_at_60[0x20];
};

struct mlx5_ifc_general_obj_out_cmd_hdr_bits {
  uint8_t status[0x8];
  uint8_t reserved_at_8[0x18];
  uint8_t syndrome[0x20];
  uint8_t obj_id[0x20];
  uint8_t reserved_at_60[0x20];
};

struct mlx5_ifc_parse_graph_flow_match_sample_bits {
  uint8_t flow_match_sample_en[0x1];
  uint8_t reserved_at_1[0x3];
  uint8_t flow_match_sample_offset_mode[0x4];
  uint8_t reserved_at_5[0x8];
  uint8_t flow_match_sample_field_offset[0x10];
  uint8_t reserved_at_32[0x4];
  uint8_t flow_match_sample_field_offset_shift[0x4];
  uint8_t flow_match_sample_field_base_offset[0x8];
  uint8_t reserved_at_48[0xd];
  uint8_t flow_match_sample_tunnel_mode[0x3];
  uint8_t flow_match_sample_field_offset_mask[0x20];
  uint8_t flow_match_sample_field_id[0x20];
};

struct mlx5_ifc_parse_graph_arc_bits {
  uint8_t start_inner_tunnel[0x1];
  uint8_t reserved_at_1[0x7];
  uint8_t arc_parse_graph_node[0x8];
  uint8_t compare_condition_value[0x10];
  uint8_t parse_graph_node_handle[0x20];
  uint8_t reserved_at_40[0x40];
};

struct mlx5_ifc_parse_graph_flex_bits {
  uint8_t modify_field_select[0x40];
  uint8_t reserved_at_64[0x20];
  uint8_t header_length_base_value[0x10];
  uint8_t reserved_at_112[0x4];
  uint8_t header_length_field_shift[0x4];
  uint8_t reserved_at_120[0x4];
  uint8_t header_length_mode[0x4];
  uint8_t header_length_field_offset[0x10];
  uint8_t next_header_field_offset[0x10];
  uint8_t reserved_at_160[0x12];
  uint8_t head_anchor_id[0x6];
  uint8_t reserved_at_178[0x1];
  uint8_t header_length_field_offset_mode[0x1];
  uint8_t reserved_at_17a[0x1];
  uint8_t next_header_field_size[0x5];
  uint8_t header_length_field_mask[0x20];
  uint8_t reserved_at_224[0x20];
  struct mlx5_ifc_parse_graph_flow_match_sample_bits sample_table[0x8];
  struct mlx5_ifc_parse_graph_arc_bits input_arc[0x8];
  struct mlx5_ifc_parse_graph_arc_bits output_arc[0x8];
};

struct mlx5_ifc_create_flex_parser_in_bits {
  struct mlx5_ifc_general_obj_in_cmd_hdr_bits hdr;
  struct mlx5_ifc_parse_graph_flex_bits flex;
};

struct mlx5_ifc_create_flex_parser_out_bits {
  struct mlx5_ifc_general_obj_out_cmd_hdr_bits hdr;
  struct mlx5_ifc_parse_graph_flex_bits flex;
};
