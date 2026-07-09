/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stable userspace/kernel/FPGA control-plane ABI for DAQIRI PCIe streams.
 *
 * This header is deliberately C compatible. All multi-byte fields exchanged
 * with the device are little-endian. The ioctl structures are native Linux
 * UAPI structures and therefore must be copied by the kernel rather than
 * consumed by FPGA logic directly.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__linux__)
#include <linux/ioctl.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DAQIRI_PCIE_ABI_MAGIC UINT32_C(0x43505144) /* "DQPC" on little-endian hosts */
#define DAQIRI_PCIE_ABI_VERSION_MAJOR UINT16_C(1)
#define DAQIRI_PCIE_ABI_VERSION_MINOR UINT16_C(0)

/* Production character-device discovery contract for a configured PCI BDF. */
#define DAQIRI_PCIE_SYSFS_CHAR_ATTRIBUTE "daqiri_pcie/char"
#define DAQIRI_PCIE_DEVNODE_PREFIX "/dev/daqiri-pcie-"

#define DAQIRI_PCIE_CAP_DMABUF_PCIE (UINT64_C(1) << 0)
/* Reserved for a possible legacy nvidia_p2p_* provider; not a DAQIRI engine. */
#define DAQIRI_PCIE_CAP_NV_P2P (UINT64_C(1) << 1)
#define DAQIRI_PCIE_CAP_DMA_FENCE (UINT64_C(1) << 2)
#define DAQIRI_PCIE_CAP_DEVICE_RESET (UINT64_C(1) << 3)

#define DAQIRI_PCIE_STATUS_FLAG_RUNNING (UINT32_C(1) << 0)
#define DAQIRI_PCIE_STATUS_FLAG_QUIESCED (UINT32_C(1) << 1)
#define DAQIRI_PCIE_STATUS_FLAG_FATAL (UINT32_C(1) << 2)

enum daqiri_pcie_direction {
  DAQIRI_PCIE_DIRECTION_RX = 0,
  DAQIRI_PCIE_DIRECTION_TX = 1,
};

enum daqiri_pcie_ring_id {
  DAQIRI_PCIE_RING_RX_AVAILABLE = 0,
  DAQIRI_PCIE_RING_RX_COMPLETION = 1,
  DAQIRI_PCIE_RING_TX_SUBMISSION = 2,
  DAQIRI_PCIE_RING_TX_COMPLETION = 3,
  DAQIRI_PCIE_RING_COUNT = 4,
};

enum daqiri_pcie_completion_status {
  DAQIRI_PCIE_COMPLETION_OK = 0,
  DAQIRI_PCIE_COMPLETION_BAD_DESCRIPTOR = 1,
  DAQIRI_PCIE_COMPLETION_LENGTH_ERROR = 2,
  DAQIRI_PCIE_COMPLETION_DMA_FAULT = 3,
  DAQIRI_PCIE_COMPLETION_DEVICE_RESET = 4,
  DAQIRI_PCIE_COMPLETION_INTERNAL_ERROR = 5,
};

/*
 * Exactly 32 bytes. Completions must echo epoch, sequence, region_id and
 * slot_id. TX completions must also echo length exactly; RX completions replace
 * length with the actual number of bytes written.
 */
struct daqiri_pcie_ring_entry {
  uint64_t epoch;
  uint64_t sequence;
  uint32_t region_id;
  uint32_t slot_id;
  uint32_t length;
  uint16_t status;
  uint16_t flags;
};

#if defined(__GNUC__) || defined(__clang__)
#define DAQIRI_PCIE_ALIGNED(bytes) __attribute__((aligned(bytes)))
#else
#define DAQIRI_PCIE_ALIGNED(bytes)
#endif

/*
 * Producer and consumer counters occupy different cache lines. They are
 * monotonically increasing (not masked); entry indexing uses counter & mask.
 * The producer publishes entries with release ordering and the consumer reads
 * it with acquire ordering. The consumer follows the inverse rule.
 */
struct DAQIRI_PCIE_ALIGNED(64) daqiri_pcie_cacheline_counter {
  volatile uint64_t value;
  uint8_t reserved[56];
};

struct DAQIRI_PCIE_ALIGNED(64) daqiri_pcie_ring_control {
  struct daqiri_pcie_cacheline_counter producer;
  struct daqiri_pcie_cacheline_counter consumer;
  uint32_t depth;
  uint32_t mask;
  uint8_t reserved[56];
};

struct daqiri_pcie_ioctl_header {
  uint32_t magic;
  uint16_t version_major;
  uint16_t version_minor;
  uint32_t struct_size;
  uint32_t flags;
};

struct daqiri_pcie_ioctl_caps {
  struct daqiri_pcie_ioctl_header header;
  uint64_t capabilities;
  uint32_t max_regions;
  uint32_t max_ring_depth;
  uint32_t min_slot_alignment;
  uint32_t reserved0;
  uint64_t reserved[4];
};

struct daqiri_pcie_ioctl_register_region {
  struct daqiri_pcie_ioctl_header header;
  int32_t dmabuf_fd;
  uint32_t direction;
  uint64_t bytes;
  uint32_t slot_stride;
  uint32_t slot_count;
  uint32_t region_id; /* driver output */
  uint32_t reserved0;
  uint64_t reserved[4];
};

struct daqiri_pcie_ioctl_unregister_region {
  struct daqiri_pcie_ioctl_header header;
  uint32_t region_id;
  uint32_t reserved0;
};

struct daqiri_pcie_ring_mapping {
  uint64_t control_offset;
  uint64_t entries_offset;
  uint32_t depth;
  uint32_t reserved0;
};

/*
 * CONFIGURE_QUEUES asks the driver to allocate one coherent mmap area. The
 * returned offsets are relative to mmap_offset within that mapping. A zero
 * depth disables the corresponding direction/ring.
 */
struct daqiri_pcie_ioctl_configure_queues {
  struct daqiri_pcie_ioctl_header header;
  uint64_t epoch;
  uint32_t requested_depth[DAQIRI_PCIE_RING_COUNT];
  uint32_t reserved0[4];
  uint64_t mmap_offset;
  uint64_t mmap_bytes;
  struct daqiri_pcie_ring_mapping rings[DAQIRI_PCIE_RING_COUNT];
  uint64_t reserved[4];
};

struct daqiri_pcie_ioctl_start {
  struct daqiri_pcie_ioctl_header header;
  uint64_t epoch;
};

struct daqiri_pcie_ioctl_stop {
  struct daqiri_pcie_ioctl_header header;
  uint32_t timeout_ms;
  uint32_t quiesced; /* nonzero only after all FPGA DMA has stopped */
};

struct daqiri_pcie_ioctl_reset {
  struct daqiri_pcie_ioctl_header header;
  /* RESET must synchronously stop all DMA before the ioctl returns. */
  uint64_t new_epoch;
};

struct daqiri_pcie_ioctl_status {
  struct daqiri_pcie_ioctl_header header;
  uint32_t status_flags;
  uint32_t fatal_code;
  uint64_t reset_count;
  uint64_t rx_completions;
  uint64_t tx_completions;
  uint64_t reserved[3];
};

#if defined(__linux__)
#define DAQIRI_PCIE_IOCTL_TYPE 0xD1
#define DAQIRI_PCIE_IOCTL_GET_CAPS \
  _IOWR(DAQIRI_PCIE_IOCTL_TYPE, 0x00, struct daqiri_pcie_ioctl_caps)
#define DAQIRI_PCIE_IOCTL_REGISTER_REGION \
  _IOWR(DAQIRI_PCIE_IOCTL_TYPE, 0x01, struct daqiri_pcie_ioctl_register_region)
#define DAQIRI_PCIE_IOCTL_UNREGISTER_REGION \
  _IOW(DAQIRI_PCIE_IOCTL_TYPE, 0x02, struct daqiri_pcie_ioctl_unregister_region)
#define DAQIRI_PCIE_IOCTL_CONFIGURE_QUEUES \
  _IOWR(DAQIRI_PCIE_IOCTL_TYPE, 0x03, struct daqiri_pcie_ioctl_configure_queues)
#define DAQIRI_PCIE_IOCTL_START _IOW(DAQIRI_PCIE_IOCTL_TYPE, 0x04, struct daqiri_pcie_ioctl_start)
#define DAQIRI_PCIE_IOCTL_STOP _IOWR(DAQIRI_PCIE_IOCTL_TYPE, 0x05, struct daqiri_pcie_ioctl_stop)
#define DAQIRI_PCIE_IOCTL_RESET _IOW(DAQIRI_PCIE_IOCTL_TYPE, 0x06, struct daqiri_pcie_ioctl_reset)
#define DAQIRI_PCIE_IOCTL_GET_STATUS \
  _IOWR(DAQIRI_PCIE_IOCTL_TYPE, 0x07, struct daqiri_pcie_ioctl_status)
#endif

#ifdef __cplusplus
} /* extern "C" */

static_assert(sizeof(daqiri_pcie_ring_entry) == 32,
              "DAQIRI PCIe ring entries are part of the FPGA ABI");
static_assert(sizeof(daqiri_pcie_cacheline_counter) == 64,
              "DAQIRI PCIe counters must occupy one cache line");
static_assert(sizeof(daqiri_pcie_ring_control) == 192,
              "DAQIRI PCIe ring control layout is part of the ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct daqiri_pcie_ring_entry) == 32,
               "DAQIRI PCIe ring entries are part of the FPGA ABI");
_Static_assert(sizeof(struct daqiri_pcie_cacheline_counter) == 64,
               "DAQIRI PCIe counters must occupy one cache line");
_Static_assert(sizeof(struct daqiri_pcie_ring_control) == 192,
               "DAQIRI PCIe ring control layout is part of the ABI");
#endif

#undef DAQIRI_PCIE_ALIGNED
