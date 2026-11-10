/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef MGBE_PMD_UAPI_H
#define MGBE_PMD_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MGBE_PMD_ABI 2U
#define MGBE_PMD_MAX_QUEUES 4U
#define MGBE_PMD_RSS_KEY_SIZE 40U
#define MGBE_PMD_RSS_TABLE_SIZE 128U
#define MGBE_PMD_MAX_RING 4096U
/* mmap offsets are region identifiers multiplied by the system page size. */
enum mgbe_pmd_region {
  MGBE_PMD_STATUS = 0,
  MGBE_PMD_RX_RING,
  MGBE_PMD_TX_RING,
  MGBE_PMD_RX_POOL,
  MGBE_PMD_TX_POOL,
  MGBE_PMD_REGS,
  MGBE_PMD_REGIONS
};
enum mgbe_pmd_state {
  MGBE_PMD_IDLE = 0,
  MGBE_PMD_CONFIGURED,
  MGBE_PMD_RUNNING,
  MGBE_PMD_FAULT,
  MGBE_PMD_REMOVED
};
enum mgbe_pmd_fault { MGBE_PMD_FAULT_NONE = 0, MGBE_PMD_FAULT_LINK, MGBE_PMD_FAULT_DMA };
struct mgbe_pmd_config {
  __u32 abi;
  __u32 ring_size;
  __u32 mtu;
  __u32 queue_count;
};
struct mgbe_pmd_info {
  __u32 abi;
  __u32 ring_size;
  __u32 mtu;
  __u32 slot_size;
  __u32 channel;
  __u32 reg_size;
  __u32 rx_tail_offset;
  __u32 tx_tail_offset;
  __u32 rx_completed_offset;
  __u32 queue_count;
  __u32 max_queues;
  __u32 flags;
  __aligned_u64 rx_ring_dma;
  __aligned_u64 tx_ring_dma;
  __aligned_u64 rx_pool_dma;
  __aligned_u64 tx_pool_dma;
  __u8 mac[6];
  __u8 padding[2];
};
/* RSS settings apply to the whole port. Update only before START; every table
 * entry must target an enabled DMA channel. Non-hashed packets use queue zero. */
struct mgbe_pmd_rss {
  __u32 abi;
  __u32 reserved;
  __u8 key[MGBE_PMD_RSS_KEY_SIZE];
  __u32 table[MGBE_PMD_RSS_TABLE_SIZE];
};
/* Read-only shared status. Load state with acquire ordering before each burst. */
struct mgbe_pmd_status {
  __u32 state;
  __u32 fault;
  __u32 link;
  __u32 dma_status;
};
struct mgbe_pmd_stats {
  __aligned_u64 rx_bytes;
  __aligned_u64 tx_bytes;
  __aligned_u64 rx_frames;
  __aligned_u64 tx_frames;
  __aligned_u64 rx_crc_errors;
  __aligned_u64 rx_fifo_overflows;
};
#define MGBE_PMD_IOC_INFO _IOR('M', 0x70, struct mgbe_pmd_info)
#define MGBE_PMD_IOC_CONFIGURE _IOW('M', 0x71, struct mgbe_pmd_config)
#define MGBE_PMD_IOC_START _IO('M', 0x72)
#define MGBE_PMD_IOC_STOP _IO('M', 0x73)
#define MGBE_PMD_IOC_STATS _IOR('M', 0x74, struct mgbe_pmd_stats)
/* Opt-in internal MAC return path, configured before START. No physical link
 * is required. Results describe the local MAC/DMA path, not wire throughput. */
/* Input abi/channel, output the selected queue's mapping information. */
#define MGBE_PMD_IOC_QUEUE_INFO _IOWR('M', 0x76, struct mgbe_pmd_info)
#define MGBE_PMD_IOC_GET_RSS _IOR('M', 0x77, struct mgbe_pmd_rss)
#define MGBE_PMD_IOC_SET_RSS _IOW('M', 0x78, struct mgbe_pmd_rss)
#define MGBE_PMD_IOC_MAC_LOOPBACK _IO('M', 0x75)

#endif
