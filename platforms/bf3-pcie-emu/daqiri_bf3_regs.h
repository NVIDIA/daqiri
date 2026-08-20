/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* BAR0 is a DOCA DevEmu stateful region. All registers are little-endian. */
#define DAQIRI_BF3_BAR0_SIZE 0x4000U
#define DAQIRI_BF3_STATEFUL_SIZE 0x100U

#define DAQIRI_BF3_REG_COMMAND 0x00U
#define DAQIRI_BF3_REG_STATUS 0x04U
#define DAQIRI_BF3_REG_ABI_MAGIC 0x08U
#define DAQIRI_BF3_REG_ABI_VERSION 0x0cU
#define DAQIRI_BF3_REG_EPOCH_LO 0x10U
#define DAQIRI_BF3_REG_EPOCH_HI 0x14U

#define DAQIRI_BF3_REG_QUEUE_TABLE_DMA_LO 0x20U
#define DAQIRI_BF3_REG_QUEUE_TABLE_DMA_HI 0x24U
#define DAQIRI_BF3_REG_QUEUE_TABLE_BYTES 0x28U
#define DAQIRI_BF3_REG_QUEUE_COUNT 0x2cU

#define DAQIRI_BF3_REG_DOORBELL_BASE 0x40U
#define DAQIRI_BF3_REG_DOORBELL(id) (DAQIRI_BF3_REG_DOORBELL_BASE + (id) * sizeof(uint32_t))

#define DAQIRI_BF3_REG_FATAL_CODE 0x90U
#define DAQIRI_BF3_REG_RESET_COUNT_LO 0x98U
#define DAQIRI_BF3_REG_RESET_COUNT_HI 0x9cU
#define DAQIRI_BF3_REG_RX_COMPLETIONS_LO 0xa0U
#define DAQIRI_BF3_REG_RX_COMPLETIONS_HI 0xa4U
#define DAQIRI_BF3_REG_TX_COMPLETIONS_LO 0xa8U
#define DAQIRI_BF3_REG_TX_COMPLETIONS_HI 0xacU

#define DAQIRI_BF3_CMD_IDLE 0U
#define DAQIRI_BF3_CMD_CONFIGURE 1U
#define DAQIRI_BF3_CMD_START 2U
#define DAQIRI_BF3_CMD_STOP 3U
#define DAQIRI_BF3_CMD_RESET 4U

#define DAQIRI_BF3_QUEUE_TABLE_MAGIC 0x44515154U /* "DQQT" */

struct daqiri_bf3_queue_table_header {
  uint32_t magic;
  uint16_t version_major;
  uint16_t version_minor;
  uint32_t num_queues;
  uint32_t descriptor_size;
  uint64_t epoch;
  uint64_t reserved[5];
};

struct daqiri_bf3_queue_descriptor {
  uint64_t work_ring_dma;
  uint64_t completion_ring_dma;
  uint64_t region_dma;
  uint64_t region_bytes;
  uint32_t depth;
  uint32_t stride;
  uint32_t slot_count;
  uint32_t region_id;
  uint16_t queue_id;
  uint8_t direction;
  uint8_t doorbell_id;
  uint32_t reserved0;
};

struct daqiri_bf3_queue_table {
  struct daqiri_bf3_queue_table_header header;
  struct daqiri_bf3_queue_descriptor queues[DAQIRI_PCIE_MAX_QUEUES];
};

#define DAQIRI_BF3_DEVICE_ID 0xda71U
#define DAQIRI_BF3_VENDOR_ID 0x15b3U
#define DAQIRI_BF3_SUBSYSTEM_ID 0xda71U
#define DAQIRI_BF3_CLASS_CODE 0x058000U
