// SPDX-License-Identifier: Apache-2.0
/*
 * DAQIRI BF3 Generic-PCI controller core.
 *
 * The DOCA adapter supplies dma_read(), dma_write(), and BAR/doorbell polling.
 * Keeping the ownership state machine free of DOCA types makes it testable and
 * ensures the emulated endpoint observes exactly the public pcie_abi.h wire
 * layout.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <daqiri/pcie_abi.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_type.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "../daqiri_bf3_regs.h"

struct bf3_dma_ops {
  int (*read)(uint32_t region, uint32_t slot, uint32_t bytes, void* scratch);
  int (*write)(uint32_t region, uint32_t slot, uint32_t bytes, const void* scratch);
  int (*fence)(void);
};

struct bf3_ring {
  struct daqiri_pcie_ring_control* control;
  struct daqiri_pcie_ring_entry* entries;
};

struct bf3_controller {
  uint64_t epoch;
  uint32_t rx_region;
  uint32_t tx_region;
  struct bf3_ring rx_available, rx_completion, tx_submission, tx_completion;
  struct bf3_dma_ops dma;
  void* scratch;
};

static bool ring_pop(struct bf3_ring* ring, struct daqiri_pcie_ring_entry* entry) {
  uint64_t consumer = __atomic_load_n(&ring->control->consumer.value, __ATOMIC_RELAXED);
  uint64_t producer = __atomic_load_n(&ring->control->producer.value, __ATOMIC_ACQUIRE);
  if (consumer == producer) return false;
  *entry = ring->entries[consumer & ring->control->mask];
  __atomic_store_n(&ring->control->consumer.value, consumer + 1, __ATOMIC_RELEASE);
  return true;
}

static bool ring_push(struct bf3_ring* ring, const struct daqiri_pcie_ring_entry* entry) {
  uint64_t producer = __atomic_load_n(&ring->control->producer.value, __ATOMIC_RELAXED);
  uint64_t consumer = __atomic_load_n(&ring->control->consumer.value, __ATOMIC_ACQUIRE);
  if (producer - consumer == ring->control->depth) return false;
  ring->entries[producer & ring->control->mask] = *entry;
  __atomic_store_n(&ring->control->producer.value, producer + 1, __ATOMIC_RELEASE);
  return true;
}

/* Processes one round-trip. The caller keeps polling until both source rings drain. */
int bf3_controller_progress(struct bf3_controller* c) {
  struct daqiri_pcie_ring_entry tx, rx, done;
  if (!ring_pop(&c->tx_submission, &tx) || !ring_pop(&c->rx_available, &rx)) return 0;
  if (tx.epoch != c->epoch || rx.epoch != c->epoch || tx.region_id != c->tx_region ||
      rx.region_id != c->rx_region || tx.length > rx.length)
    return -1;
  if (c->dma.read(tx.region_id, tx.slot_id, tx.length, c->scratch) ||
      c->dma.write(rx.region_id, rx.slot_id, tx.length, c->scratch) || c->dma.fence())
    return -1;
  memset(&done, 0, sizeof(done));
  done.epoch = c->epoch;
  done.sequence = rx.sequence;
  done.region_id = rx.region_id;
  done.slot_id = rx.slot_id;
  done.length = tx.length;
  done.status = DAQIRI_PCIE_COMPLETION_OK;
  if (!ring_push(&c->rx_completion, &done)) return -1;
  done.sequence = tx.sequence;
  done.region_id = tx.region_id;
  done.slot_id = tx.slot_id;
  done.length = tx.length;
  return ring_push(&c->tx_completion, &done) ? 1 : -1;
}

/*
 * DOCA 2.9 DevEmu adapter. Host addresses below are IOVAs in the emulated
 * function's domain. Every access to them, including ring accesses, goes
 * through DOCA DMA; they are never dereferenced by the Arm CPU.
 */
#define BF3_PCI_TYPE_NAME "DAQIRI BF3 PCIe v1"
#define BF3_BAR_ID 0U
#define BF3_STATEFUL_START 0U
#define BF3_DMA_TASKS 1U
#define BF3_IDLE_NS 10000L

struct bf3_bar_region {
  uint64_t dma_addr;
  uint64_t bytes;
  uint32_t stride;
  uint32_t count;
  uint32_t id;
  uint32_t direction;
};

struct bf3_bar_state {
  uint32_t command;
  uint32_t status;
  uint32_t abi_magic;
  uint32_t abi_version;
  uint64_t epoch;
  uint64_t reserved18;
  uint64_t ring_dma[DAQIRI_PCIE_RING_COUNT];
  uint32_t ring_bytes;
  uint8_t reserved44[12];
  struct bf3_bar_region region[DAQIRI_BF3_REGION_COUNT];
  uint32_t fatal_code;
  uint32_t reserved94;
  uint64_t reset_count;
  uint64_t rx_completions;
  uint64_t tx_completions;
};

_Static_assert(offsetof(struct bf3_bar_state, ring_dma) == 0x20, "ring BAR offset");
_Static_assert(offsetof(struct bf3_bar_state, region) == 0x50, "region BAR offset");
_Static_assert(offsetof(struct bf3_bar_state, fatal_code) == 0x90, "status BAR offset");

struct bf3_doca {
  struct doca_pe* pe;
  struct doca_dev* manager_dev;
  struct doca_dev* dma_dev;
  struct doca_devemu_pci_type* pci_type;
  struct doca_dev_rep* rep;
  struct doca_devemu_pci_dev* pci_dev;
  struct doca_ctx* pci_ctx;
  struct doca_dma* dma;
  struct doca_ctx* dma_ctx;
  struct doca_buf_inventory* inventory;
  struct doca_mmap* local_mmap;
  struct doca_mmap* ring_mmap;
  struct doca_mmap* region_mmap[DAQIRI_BF3_REGION_COUNT];
  uint8_t* scratch;
  size_t scratch_bytes;
  volatile size_t dma_pending;
  doca_error_t dma_result;
  volatile bool bar_pending;
  volatile bool flr_pending;
  volatile bool hotplug_changed;
  enum doca_devemu_pci_hotplug_state hotplug_state;
  bool configured;
  bool running;
  bool has_rx;
  bool has_tx;
  bool created_rep;
  uint64_t ring_base;
  struct bf3_bar_state bar;
};

static volatile sig_atomic_t bf3_quit;

static void bf3_signal(int signum) {
  (void)signum;
  bf3_quit = 1;
}

static void bf3_log_error(const char* what, doca_error_t result) {
  fprintf(stderr, "%s: %s\n", what, doca_error_get_descr(result));
}

static void bf3_dma_done(struct doca_dma_task_memcpy* task, union doca_data task_data,
                         union doca_data ctx_data) {
  struct bf3_doca* c = ctx_data.ptr;

  (void)task_data;
  c->dma_result = DOCA_SUCCESS;
  doca_task_free(doca_dma_task_memcpy_as_task(task));
  --c->dma_pending;
}

static void bf3_dma_error(struct doca_dma_task_memcpy* task, union doca_data task_data,
                          union doca_data ctx_data) {
  struct bf3_doca* c = ctx_data.ptr;

  (void)task_data;
  c->dma_result = doca_task_get_status(doca_dma_task_memcpy_as_task(task));
  doca_task_free(doca_dma_task_memcpy_as_task(task));
  --c->dma_pending;
}

static void bf3_bar_write_event(
    struct doca_devemu_pci_dev_event_bar_stateful_region_driver_write* event,
    union doca_data user_data) {
  struct bf3_doca* c = user_data.ptr;

  (void)event;
  c->bar_pending = true;
}

static void bf3_flr_event(struct doca_devemu_pci_dev* pci_dev, union doca_data user_data) {
  struct bf3_doca* c = user_data.ptr;

  (void)pci_dev;
  c->flr_pending = true;
}

static void bf3_hotplug_event(struct doca_devemu_pci_dev* pci_dev, union doca_data user_data) {
  struct bf3_doca* c = user_data.ptr;

  if (doca_devemu_pci_dev_get_hotplug_state(pci_dev, &c->hotplug_state) == DOCA_SUCCESS)
    c->hotplug_changed = true;
}

static doca_error_t bf3_open_manager(const char* pci_addr, const struct doca_devemu_pci_type* type,
                                     struct doca_dev** dev) {
  struct doca_devinfo** list = NULL;
  uint32_t count = 0;
  uint32_t i;
  doca_error_t result;

  result = doca_devinfo_create_list(&list, &count);
  if (result != DOCA_SUCCESS) return result;
  result = DOCA_ERROR_NOT_FOUND;
  for (i = 0; i < count; ++i) {
    uint8_t equal = 0, supported = 0;

    if (doca_devinfo_is_equal_pci_addr(list[i], pci_addr, &equal) != DOCA_SUCCESS || !equal)
      continue;
    if (doca_devemu_pci_cap_type_is_hotplug_supported(list[i], type, &supported) != DOCA_SUCCESS ||
        !supported) {
      result = DOCA_ERROR_NOT_SUPPORTED;
      continue;
    }
    result = doca_dev_open(list[i], dev);
    break;
  }
  doca_devinfo_destroy_list(list);
  return result;
}

static doca_error_t bf3_find_rep(struct doca_devemu_pci_type* type, const char* vuid,
                                 struct doca_dev_rep** rep) {
  struct doca_devinfo_rep** list = NULL;
  uint32_t count = 0, i;
  doca_error_t result;

  result = doca_devemu_pci_type_create_rep_list(type, &list, &count);
  if (result != DOCA_SUCCESS) return result;
  result = DOCA_ERROR_NOT_FOUND;
  for (i = 0; i < count; ++i) {
    char actual[DOCA_DEVINFO_REP_VUID_SIZE] = {0};

    if (doca_devinfo_rep_get_vuid(list[i], actual, sizeof(actual)) != DOCA_SUCCESS ||
        strcmp(actual, vuid) != 0)
      continue;
    result = doca_dev_rep_open(list[i], rep);
    break;
  }
  doca_devinfo_rep_destroy_list(list);
  return result;
}

static doca_error_t bf3_configure_type(struct bf3_doca* c) {
  doca_error_t result;
  uint8_t* defaults;
  struct bf3_bar_state* bar;

#define BF3_TYPE_CALL(call, label)  \
  do {                              \
    result = (call);                \
    if (result != DOCA_SUCCESS) {   \
      bf3_log_error(label, result); \
      return result;                \
    }                               \
  } while (0)
  BF3_TYPE_CALL(doca_devemu_pci_type_set_dev(c->pci_type, c->manager_dev), "set PCI type device");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_vendor_id(c->pci_type, DAQIRI_BF3_VENDOR_ID),
                "set vendor id");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_device_id(c->pci_type, DAQIRI_BF3_DEVICE_ID),
                "set device id");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_subsystem_vendor_id(c->pci_type, DAQIRI_BF3_VENDOR_ID),
                "set subsystem vendor");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_subsystem_id(c->pci_type, DAQIRI_BF3_SUBSYSTEM_ID),
                "set subsystem id");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_revision_id(c->pci_type, 1), "set revision");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_class_code(c->pci_type, DAQIRI_BF3_CLASS_CODE),
                "set class");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_memory_bar_conf(c->pci_type, 0, 14,
                                                         DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT, 0),
                "configure BAR0");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_memory_bar_conf(c->pci_type, 1, 0,
                                                         DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT, 0),
                "configure BAR0 extension");
  BF3_TYPE_CALL(doca_devemu_pci_type_set_bar_stateful_region_conf(
                    c->pci_type, BF3_BAR_ID, BF3_STATEFUL_START, DAQIRI_BF3_STATEFUL_SIZE),
                "configure stateful BAR");
  BF3_TYPE_CALL(doca_devemu_pci_type_start(c->pci_type), "start PCI type");
  defaults = calloc(1, DAQIRI_BF3_STATEFUL_SIZE);
  if (!defaults) return DOCA_ERROR_NO_MEMORY;
  bar = (struct bf3_bar_state*)defaults;
  bar->status = DAQIRI_PCIE_STATUS_FLAG_QUIESCED;
  bar->abi_magic = DAQIRI_PCIE_ABI_MAGIC;
  bar->abi_version = (DAQIRI_PCIE_ABI_VERSION_MAJOR << 16) | DAQIRI_PCIE_ABI_VERSION_MINOR;
  result = doca_devemu_pci_type_modify_bar_stateful_region_default_values(
      c->pci_type, BF3_BAR_ID, BF3_STATEFUL_START, defaults, DAQIRI_BF3_STATEFUL_SIZE);
  free(defaults);
  if (result != DOCA_SUCCESS) bf3_log_error("set BAR defaults", result);
  return result;
#undef BF3_TYPE_CALL
}

static doca_error_t bf3_setup_devemu(struct bf3_doca* c, const char* pci_addr, const char* vuid) {
  doca_error_t result;
  union doca_data data = {.ptr = c};
  char created_vuid[DOCA_DEVINFO_REP_VUID_SIZE] = {0};

  result = doca_pe_create(&c->pe);
  if (result != DOCA_SUCCESS) return result;
  result = doca_devemu_pci_type_create(BF3_PCI_TYPE_NAME, &c->pci_type);
  if (result != DOCA_SUCCESS) return result;
  result = bf3_open_manager(pci_addr, c->pci_type, &c->manager_dev);
  if (result != DOCA_SUCCESS) return result;
  result = bf3_configure_type(c);
  if (result != DOCA_SUCCESS) return result;
  if (vuid) {
    result = bf3_find_rep(c->pci_type, vuid, &c->rep);
  } else {
    result = doca_devemu_pci_dev_create_rep(c->pci_type, &c->rep);
    c->created_rep = result == DOCA_SUCCESS;
  }
  if (result != DOCA_SUCCESS) return result;
  if (doca_devinfo_rep_get_vuid(doca_dev_rep_as_devinfo(c->rep), created_vuid,
                                sizeof(created_vuid)) == DOCA_SUCCESS)
    printf("DAQIRI DevEmu VUID: %s\n", created_vuid);
  result = doca_devemu_pci_dev_create(c->pci_type, c->rep, c->pe, &c->pci_dev);
  if (result != DOCA_SUCCESS) return result;
  result = doca_devemu_pci_dev_event_bar_stateful_region_driver_write_register(
      c->pci_dev, bf3_bar_write_event, BF3_BAR_ID, BF3_STATEFUL_START, data);
  if (result != DOCA_SUCCESS) return result;
  result = doca_devemu_pci_dev_event_flr_register(c->pci_dev, bf3_flr_event, data);
  if (result != DOCA_SUCCESS) return result;
  result =
      doca_devemu_pci_dev_event_hotplug_state_change_register(c->pci_dev, bf3_hotplug_event, data);
  if (result != DOCA_SUCCESS) return result;
  c->pci_ctx = doca_devemu_pci_dev_as_ctx(c->pci_dev);
  result = doca_ctx_start(c->pci_ctx);
  if (result != DOCA_SUCCESS) return result;
  result = doca_devemu_pci_dev_get_hotplug_state(c->pci_dev, &c->hotplug_state);
  if (result != DOCA_SUCCESS) return result;
  if (c->hotplug_state != DOCA_DEVEMU_PCI_HP_STATE_POWER_ON) {
    result = doca_devemu_pci_dev_hotplug(c->pci_dev);
    if (result != DOCA_SUCCESS) return result;
    while (c->hotplug_state != DOCA_DEVEMU_PCI_HP_STATE_POWER_ON && !bf3_quit) {
      doca_pe_progress(c->pe);
      usleep(1000);
    }
  }
  return bf3_quit ? DOCA_ERROR_UNEXPECTED : DOCA_SUCCESS;
}

static doca_error_t bf3_setup_dma(struct bf3_doca* c) {
  doca_error_t result;
  union doca_data data = {.ptr = c};

  result = doca_dev_open(doca_dev_as_devinfo(c->manager_dev), &c->dma_dev);
  if (result != DOCA_SUCCESS) return result;
  result = doca_dma_create(c->dma_dev, &c->dma);
  if (result != DOCA_SUCCESS) return result;
  result = doca_dma_task_memcpy_set_conf(c->dma, bf3_dma_done, bf3_dma_error, BF3_DMA_TASKS);
  if (result != DOCA_SUCCESS) return result;
  c->dma_ctx = doca_dma_as_ctx(c->dma);
  result = doca_ctx_set_user_data(c->dma_ctx, data);
  if (result != DOCA_SUCCESS) return result;
  result = doca_pe_connect_ctx(c->pe, c->dma_ctx);
  if (result != DOCA_SUCCESS) return result;
  result = doca_ctx_start(c->dma_ctx);
  if (result != DOCA_SUCCESS) return result;
  result = doca_buf_inventory_create(2, &c->inventory);
  if (result != DOCA_SUCCESS) return result;
  return doca_buf_inventory_start(c->inventory);
}

static void bf3_destroy_mmap(struct doca_mmap** mmap) {
  if (!*mmap) return;
  doca_mmap_stop(*mmap);
  doca_mmap_destroy(*mmap);
  *mmap = NULL;
}

static void bf3_destroy_remote_maps(struct bf3_doca* c) {
  unsigned int i;

  bf3_destroy_mmap(&c->ring_mmap);
  for (i = 0; i < DAQIRI_BF3_REGION_COUNT; ++i) bf3_destroy_mmap(&c->region_mmap[i]);
  c->configured = false;
  c->running = false;
  c->has_rx = false;
  c->has_tx = false;
}

static doca_error_t bf3_create_remote_mmap(struct bf3_doca* c, uint64_t addr, uint64_t bytes,
                                           struct doca_mmap** mmap) {
  doca_error_t result;
  const char* stage = "doca_devemu_pci_mmap_create";

  result = doca_devemu_pci_mmap_create(c->pci_dev, mmap);
  if (result == DOCA_SUCCESS) {
    stage = "doca_mmap_set_max_num_devices";
    result = doca_mmap_set_max_num_devices(*mmap, 1);
  }
  if (result == DOCA_SUCCESS) {
    stage = "doca_mmap_add_dev";
    result = doca_mmap_add_dev(*mmap, c->dma_dev);
  }
  if (result == DOCA_SUCCESS) {
    stage = "doca_mmap_set_permissions";
    result = doca_mmap_set_permissions(*mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
  }
  if (result == DOCA_SUCCESS) {
    stage = "doca_mmap_set_memrange";
    result = doca_mmap_set_memrange(*mmap, (void*)(uintptr_t)addr, bytes);
  }
  if (result == DOCA_SUCCESS) {
    stage = "doca_mmap_start";
    result = doca_mmap_start(*mmap);
  }
  if (result != DOCA_SUCCESS) {
    fprintf(stderr, "%s failed for remote [0x%llx, +0x%llx): %s\n", stage, (unsigned long long)addr,
            (unsigned long long)bytes, doca_error_get_descr(result));
    bf3_destroy_mmap(mmap);
  }
  return result;
}

static doca_error_t bf3_resize_scratch(struct bf3_doca* c, size_t bytes) {
  doca_error_t result;

  if (c->scratch_bytes >= bytes) return DOCA_SUCCESS;
  bf3_destroy_mmap(&c->local_mmap);
  free(c->scratch);
  c->scratch = calloc(1, bytes);
  if (!c->scratch) {
    c->scratch_bytes = 0;
    return DOCA_ERROR_NO_MEMORY;
  }
  c->scratch_bytes = bytes;
  result = doca_mmap_create(&c->local_mmap);
  if (result == DOCA_SUCCESS) result = doca_mmap_add_dev(c->local_mmap, c->dma_dev);
  if (result == DOCA_SUCCESS) result = doca_mmap_set_memrange(c->local_mmap, c->scratch, bytes);
  if (result == DOCA_SUCCESS) result = doca_mmap_start(c->local_mmap);
  if (result != DOCA_SUCCESS) bf3_destroy_mmap(&c->local_mmap);
  return result;
}

static doca_error_t bf3_dma_copy(struct bf3_doca* c, struct doca_mmap* src_mmap, void* src,
                                 struct doca_mmap* dst_mmap, void* dst, size_t bytes) {
  struct doca_buf *src_buf = NULL, *dst_buf = NULL;
  struct doca_dma_task_memcpy* dma_task = NULL;
  struct doca_task* task;
  union doca_data data = {0};
  doca_error_t result;

  result = doca_buf_inventory_buf_get_by_addr(c->inventory, src_mmap, src, bytes, &src_buf);
  if (result != DOCA_SUCCESS) return result;
  result = doca_buf_inventory_buf_get_by_addr(c->inventory, dst_mmap, dst, bytes, &dst_buf);
  if (result != DOCA_SUCCESS) goto out_src;
  result = doca_dma_task_memcpy_alloc_init(c->dma, src_buf, dst_buf, data, &dma_task);
  if (result != DOCA_SUCCESS) goto out_dst;
  result = doca_buf_set_data(src_buf, src, bytes);
  if (result != DOCA_SUCCESS) {
    doca_task_free(doca_dma_task_memcpy_as_task(dma_task));
    goto out_dst;
  }
  c->dma_result = DOCA_ERROR_IN_PROGRESS;
  c->dma_pending = 1;
  task = doca_dma_task_memcpy_as_task(dma_task);
  result = doca_task_submit(task);
  if (result != DOCA_SUCCESS) {
    c->dma_pending = 0;
    doca_task_free(task);
    goto out_dst;
  }
  while (c->dma_pending) doca_pe_progress(c->pe);
  result = c->dma_result;
out_dst:
  doca_buf_dec_refcount(dst_buf, NULL);
out_src:
  doca_buf_dec_refcount(src_buf, NULL);
  return result;
}

static doca_error_t bf3_dma_read(struct bf3_doca* c, struct doca_mmap* remote, uint64_t addr,
                                 void* out, size_t bytes) {
  doca_error_t result;

  result = bf3_dma_copy(c, remote, (void*)(uintptr_t)addr, c->local_mmap, c->scratch, bytes);
  if (result == DOCA_SUCCESS) memcpy(out, c->scratch, bytes);
  return result;
}

static doca_error_t bf3_dma_write(struct bf3_doca* c, struct doca_mmap* remote, uint64_t addr,
                                  const void* data, size_t bytes) {
  memcpy(c->scratch, data, bytes);
  return bf3_dma_copy(c, c->local_mmap, c->scratch, remote, (void*)(uintptr_t)addr, bytes);
}

static doca_error_t bf3_set_bar(struct bf3_doca* c, size_t offset, void* value, size_t size) {
  return doca_devemu_pci_dev_modify_bar_stateful_region_values(c->pci_dev, BF3_BAR_ID, offset,
                                                               value, size);
}

static doca_error_t bf3_publish_status(struct bf3_doca* c, uint32_t status, uint32_t fatal) {
  doca_error_t result;

  c->bar.status = status;
  c->bar.fatal_code = fatal;
  result = bf3_set_bar(c, offsetof(struct bf3_bar_state, fatal_code), &fatal, sizeof(fatal));
  if (result == DOCA_SUCCESS)
    result = bf3_set_bar(c, offsetof(struct bf3_bar_state, status), &status, sizeof(status));
  return result;
}

static doca_error_t bf3_read_bar(struct bf3_doca* c) {
  return doca_devemu_pci_dev_query_bar_stateful_region_values(c->pci_dev, BF3_BAR_ID, 0, &c->bar,
                                                              sizeof(c->bar));
}

static doca_error_t bf3_configure_remote(struct bf3_doca* c) {
  uint64_t min_ring = UINT64_MAX;
  size_t scratch_bytes = sizeof(struct daqiri_pcie_ring_control);
  unsigned int i;
  doca_error_t result;

  bf3_destroy_remote_maps(c);
  if (c->bar.abi_magic != DAQIRI_PCIE_ABI_MAGIC || !c->bar.ring_bytes)
    return DOCA_ERROR_INVALID_VALUE;
  c->has_rx = c->bar.ring_dma[DAQIRI_PCIE_RING_RX_AVAILABLE] &&
              c->bar.ring_dma[DAQIRI_PCIE_RING_RX_COMPLETION];
  c->has_tx = c->bar.ring_dma[DAQIRI_PCIE_RING_TX_SUBMISSION] &&
              c->bar.ring_dma[DAQIRI_PCIE_RING_TX_COMPLETION];
  if (!c->has_rx && !c->has_tx) return DOCA_ERROR_INVALID_VALUE;
  for (i = 0; i < DAQIRI_PCIE_RING_COUNT; ++i)
    if (c->bar.ring_dma[i] && c->bar.ring_dma[i] < min_ring) min_ring = c->bar.ring_dma[i];
  if (min_ring == UINT64_MAX) return DOCA_ERROR_INVALID_VALUE;
  c->ring_base = min_ring;
  result = bf3_create_remote_mmap(c, min_ring, c->bar.ring_bytes, &c->ring_mmap);
  if (result != DOCA_SUCCESS) return result;
  for (i = 0; i < DAQIRI_BF3_REGION_COUNT; ++i) {
    const struct bf3_bar_region* r = &c->bar.region[i];
    uint64_t required;
    bool active = (i == DAQIRI_BF3_REGION_RX) ? c->has_rx : c->has_tx;

    if (!active) continue;

    if (!r->dma_addr || !r->bytes || !r->stride || !r->count || r->id != i + 1 ||
        r->direction != i ||
        __builtin_mul_overflow((uint64_t)r->stride, (uint64_t)r->count, &required) ||
        required > r->bytes)
      return DOCA_ERROR_INVALID_VALUE;
    result = bf3_create_remote_mmap(c, r->dma_addr, r->bytes, &c->region_mmap[i]);
    if (result != DOCA_SUCCESS) return result;
    if (r->stride > scratch_bytes) scratch_bytes = r->stride;
  }
  result = bf3_resize_scratch(c, scratch_bytes);
  if (result == DOCA_SUCCESS) c->configured = true;
  return result;
}

static bool bf3_valid_control(const struct daqiri_pcie_ring_control* control) {
  return control->depth && control->depth <= 4096 && !(control->depth & (control->depth - 1)) &&
         control->mask == control->depth - 1 &&
         control->producer.value - control->consumer.value <= control->depth;
}

static doca_error_t bf3_read_entry(struct bf3_doca* c, unsigned int ring_id, uint64_t counter,
                                   uint32_t mask, struct daqiri_pcie_ring_entry* entry) {
  uint64_t addr = c->bar.ring_dma[ring_id] + sizeof(struct daqiri_pcie_ring_control) +
                  (counter & mask) * sizeof(*entry);
  return bf3_dma_read(c, c->ring_mmap, addr, entry, sizeof(*entry));
}

static doca_error_t bf3_write_entry(struct bf3_doca* c, unsigned int ring_id, uint64_t counter,
                                    uint32_t mask, const struct daqiri_pcie_ring_entry* entry) {
  uint64_t addr = c->bar.ring_dma[ring_id] + sizeof(struct daqiri_pcie_ring_control) +
                  (counter & mask) * sizeof(*entry);
  return bf3_dma_write(c, c->ring_mmap, addr, entry, sizeof(*entry));
}

static doca_error_t bf3_write_counter(struct bf3_doca* c, unsigned int ring_id,
                                      size_t counter_offset, uint64_t value) {
  return bf3_dma_write(c, c->ring_mmap, c->bar.ring_dma[ring_id] + counter_offset, &value,
                       sizeof(value));
}

static doca_error_t bf3_progress_tx(struct bf3_doca* c, struct daqiri_pcie_ring_control* control,
                                    bool* made_progress) {
  struct daqiri_pcie_ring_entry tx, done = {0};
  const unsigned int sub = DAQIRI_PCIE_RING_TX_SUBMISSION;
  const unsigned int comp = DAQIRI_PCIE_RING_TX_COMPLETION;
  uint64_t tx_addr;
  doca_error_t result;

  if (control[sub].producer.value == control[sub].consumer.value ||
      control[comp].producer.value - control[comp].consumer.value == control[comp].depth)
    return DOCA_SUCCESS;
  result = bf3_read_entry(c, sub, control[sub].consumer.value, control[sub].mask, &tx);
  if (result != DOCA_SUCCESS) return result;
  if (tx.epoch != c->bar.epoch || tx.region_id != c->bar.region[DAQIRI_BF3_REGION_TX].id ||
      tx.slot_id >= c->bar.region[DAQIRI_BF3_REGION_TX].count || !tx.length ||
      tx.length > c->bar.region[DAQIRI_BF3_REGION_TX].stride)
    return DOCA_ERROR_INVALID_VALUE;
  tx_addr = c->bar.region[DAQIRI_BF3_REGION_TX].dma_addr +
            (uint64_t)tx.slot_id * c->bar.region[DAQIRI_BF3_REGION_TX].stride;
  result = bf3_dma_copy(c, c->region_mmap[DAQIRI_BF3_REGION_TX], (void*)(uintptr_t)tx_addr,
                        c->local_mmap, c->scratch, tx.length);
  if (result != DOCA_SUCCESS) return result;
  done = tx;
  done.status = DAQIRI_PCIE_COMPLETION_OK;
  result = bf3_write_entry(c, comp, control[comp].producer.value, control[comp].mask, &done);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, comp, offsetof(struct daqiri_pcie_ring_control, producer.value),
                               control[comp].producer.value + 1);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, sub, offsetof(struct daqiri_pcie_ring_control, consumer.value),
                               control[sub].consumer.value + 1);
  if (result == DOCA_SUCCESS) {
    ++c->bar.tx_completions;
    *made_progress = true;
  }
  return result;
}

static doca_error_t bf3_progress_rx(struct bf3_doca* c, struct daqiri_pcie_ring_control* control,
                                    bool* made_progress) {
  struct daqiri_pcie_ring_entry rx, done = {0};
  const unsigned int avail = DAQIRI_PCIE_RING_RX_AVAILABLE;
  const unsigned int comp = DAQIRI_PCIE_RING_RX_COMPLETION;
  uint64_t sequence = c->bar.rx_completions;
  uint64_t rx_addr;
  size_t i;
  doca_error_t result;

  if (control[avail].producer.value == control[avail].consumer.value ||
      control[comp].producer.value - control[comp].consumer.value == control[comp].depth)
    return DOCA_SUCCESS;
  result = bf3_read_entry(c, avail, control[avail].consumer.value, control[avail].mask, &rx);
  if (result != DOCA_SUCCESS) return result;
  if (rx.epoch != c->bar.epoch || rx.region_id != c->bar.region[DAQIRI_BF3_REGION_RX].id ||
      rx.slot_id >= c->bar.region[DAQIRI_BF3_REGION_RX].count || rx.length < sizeof(sequence) ||
      rx.length > c->bar.region[DAQIRI_BF3_REGION_RX].stride)
    return DOCA_ERROR_INVALID_VALUE;
  memcpy(c->scratch, &sequence, sizeof(sequence));
  for (i = sizeof(sequence); i < rx.length; ++i)
    c->scratch[i] = (uint8_t)((sequence + i * 17U) & 0xffU);
  rx_addr = c->bar.region[DAQIRI_BF3_REGION_RX].dma_addr +
            (uint64_t)rx.slot_id * c->bar.region[DAQIRI_BF3_REGION_RX].stride;
  result = bf3_dma_copy(c, c->local_mmap, c->scratch, c->region_mmap[DAQIRI_BF3_REGION_RX],
                        (void*)(uintptr_t)rx_addr, rx.length);
  if (result != DOCA_SUCCESS) return result;
  done = rx;
  done.status = DAQIRI_PCIE_COMPLETION_OK;
  result = bf3_write_entry(c, comp, control[comp].producer.value, control[comp].mask, &done);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, comp, offsetof(struct daqiri_pcie_ring_control, producer.value),
                               control[comp].producer.value + 1);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, avail, offsetof(struct daqiri_pcie_ring_control, consumer.value),
                               control[avail].consumer.value + 1);
  if (result == DOCA_SUCCESS) {
    ++c->bar.rx_completions;
    *made_progress = true;
  }
  return result;
}

static doca_error_t bf3_progress_roundtrip(struct bf3_doca* c, bool* made_progress) {
  struct daqiri_pcie_ring_control control[DAQIRI_PCIE_RING_COUNT] = {0};
  struct daqiri_pcie_ring_entry tx, rx, tx_done = {0}, rx_done = {0};
  uint64_t tx_addr, rx_addr;
  unsigned int i;
  doca_error_t result;

  *made_progress = false;
  for (i = 0; i < DAQIRI_PCIE_RING_COUNT; ++i) {
    if (!c->bar.ring_dma[i]) continue;
    result = bf3_dma_read(c, c->ring_mmap, c->bar.ring_dma[i], &control[i], sizeof(control[i]));
    if (result != DOCA_SUCCESS) {
      fprintf(stderr, "ring %u control DMA read failed at 0x%llx: %s\n", i,
              (unsigned long long)c->bar.ring_dma[i], doca_error_get_descr(result));
      return result;
    }
    if (!bf3_valid_control(&control[i])) {
      fprintf(stderr, "ring %u invalid control: depth=%u mask=%u producer=%llu consumer=%llu\n", i,
              control[i].depth, control[i].mask, (unsigned long long)control[i].producer.value,
              (unsigned long long)control[i].consumer.value);
      return DOCA_ERROR_INVALID_VALUE;
    }
  }
  if (!c->has_rx) return bf3_progress_tx(c, control, made_progress);
  if (!c->has_tx) return bf3_progress_rx(c, control, made_progress);
  if (control[DAQIRI_PCIE_RING_TX_SUBMISSION].producer.value ==
          control[DAQIRI_PCIE_RING_TX_SUBMISSION].consumer.value ||
      control[DAQIRI_PCIE_RING_RX_AVAILABLE].producer.value ==
          control[DAQIRI_PCIE_RING_RX_AVAILABLE].consumer.value)
    return DOCA_SUCCESS;
  if (control[DAQIRI_PCIE_RING_RX_COMPLETION].producer.value -
              control[DAQIRI_PCIE_RING_RX_COMPLETION].consumer.value ==
          control[DAQIRI_PCIE_RING_RX_COMPLETION].depth ||
      control[DAQIRI_PCIE_RING_TX_COMPLETION].producer.value -
              control[DAQIRI_PCIE_RING_TX_COMPLETION].consumer.value ==
          control[DAQIRI_PCIE_RING_TX_COMPLETION].depth)
    return DOCA_SUCCESS;
  result = bf3_read_entry(c, DAQIRI_PCIE_RING_TX_SUBMISSION,
                          control[DAQIRI_PCIE_RING_TX_SUBMISSION].consumer.value,
                          control[DAQIRI_PCIE_RING_TX_SUBMISSION].mask, &tx);
  if (result == DOCA_SUCCESS)
    result = bf3_read_entry(c, DAQIRI_PCIE_RING_RX_AVAILABLE,
                            control[DAQIRI_PCIE_RING_RX_AVAILABLE].consumer.value,
                            control[DAQIRI_PCIE_RING_RX_AVAILABLE].mask, &rx);
  if (result != DOCA_SUCCESS) return result;
  if (tx.epoch != c->bar.epoch || rx.epoch != c->bar.epoch ||
      tx.region_id != c->bar.region[DAQIRI_BF3_REGION_TX].id ||
      rx.region_id != c->bar.region[DAQIRI_BF3_REGION_RX].id ||
      tx.slot_id >= c->bar.region[DAQIRI_BF3_REGION_TX].count ||
      rx.slot_id >= c->bar.region[DAQIRI_BF3_REGION_RX].count || !tx.length ||
      tx.length > c->bar.region[DAQIRI_BF3_REGION_TX].stride || tx.length > rx.length ||
      tx.length > c->bar.region[DAQIRI_BF3_REGION_RX].stride)
    return DOCA_ERROR_INVALID_VALUE;
  tx_addr = c->bar.region[DAQIRI_BF3_REGION_TX].dma_addr +
            (uint64_t)tx.slot_id * c->bar.region[DAQIRI_BF3_REGION_TX].stride;
  rx_addr = c->bar.region[DAQIRI_BF3_REGION_RX].dma_addr +
            (uint64_t)rx.slot_id * c->bar.region[DAQIRI_BF3_REGION_RX].stride;
  result = bf3_dma_copy(c, c->region_mmap[DAQIRI_BF3_REGION_TX], (void*)(uintptr_t)tx_addr,
                        c->local_mmap, c->scratch, tx.length);
  if (result == DOCA_SUCCESS)
    result = bf3_dma_copy(c, c->local_mmap, c->scratch, c->region_mmap[DAQIRI_BF3_REGION_RX],
                          (void*)(uintptr_t)rx_addr, tx.length);
  if (result != DOCA_SUCCESS) return result;
  rx_done.epoch = c->bar.epoch;
  rx_done.sequence = rx.sequence;
  rx_done.region_id = rx.region_id;
  rx_done.slot_id = rx.slot_id;
  rx_done.length = tx.length;
  rx_done.status = DAQIRI_PCIE_COMPLETION_OK;
  tx_done = rx_done;
  tx_done.sequence = tx.sequence;
  tx_done.region_id = tx.region_id;
  tx_done.slot_id = tx.slot_id;
  result = bf3_write_entry(c, DAQIRI_PCIE_RING_RX_COMPLETION,
                           control[DAQIRI_PCIE_RING_RX_COMPLETION].producer.value,
                           control[DAQIRI_PCIE_RING_RX_COMPLETION].mask, &rx_done);
  if (result == DOCA_SUCCESS)
    result = bf3_write_entry(c, DAQIRI_PCIE_RING_TX_COMPLETION,
                             control[DAQIRI_PCIE_RING_TX_COMPLETION].producer.value,
                             control[DAQIRI_PCIE_RING_TX_COMPLETION].mask, &tx_done);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, DAQIRI_PCIE_RING_RX_COMPLETION,
                               offsetof(struct daqiri_pcie_ring_control, producer.value),
                               control[DAQIRI_PCIE_RING_RX_COMPLETION].producer.value + 1);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, DAQIRI_PCIE_RING_TX_COMPLETION,
                               offsetof(struct daqiri_pcie_ring_control, producer.value),
                               control[DAQIRI_PCIE_RING_TX_COMPLETION].producer.value + 1);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, DAQIRI_PCIE_RING_RX_AVAILABLE,
                               offsetof(struct daqiri_pcie_ring_control, consumer.value),
                               control[DAQIRI_PCIE_RING_RX_AVAILABLE].consumer.value + 1);
  if (result == DOCA_SUCCESS)
    result = bf3_write_counter(c, DAQIRI_PCIE_RING_TX_SUBMISSION,
                               offsetof(struct daqiri_pcie_ring_control, consumer.value),
                               control[DAQIRI_PCIE_RING_TX_SUBMISSION].consumer.value + 1);
  if (result != DOCA_SUCCESS) return result;
  ++c->bar.rx_completions;
  ++c->bar.tx_completions;
  *made_progress = true;
  return DOCA_SUCCESS;
}

static doca_error_t bf3_handle_command(struct bf3_doca* c) {
  uint32_t idle = DAQIRI_BF3_CMD_IDLE;
  doca_error_t result;

  c->bar_pending = false;
  result = bf3_read_bar(c);
  if (result != DOCA_SUCCESS || c->bar.command == DAQIRI_BF3_CMD_IDLE) return result;
  switch (c->bar.command) {
    case DAQIRI_BF3_CMD_CONFIGURE:
      result = bf3_configure_remote(c);
      if (result == DOCA_SUCCESS)
        result = bf3_publish_status(c, DAQIRI_PCIE_STATUS_FLAG_QUIESCED, 0);
      break;
    case DAQIRI_BF3_CMD_START:
      if (!c->configured)
        result = DOCA_ERROR_BAD_STATE;
      else {
        c->running = true;
        result = bf3_publish_status(c, DAQIRI_PCIE_STATUS_FLAG_RUNNING, 0);
      }
      break;
    case DAQIRI_BF3_CMD_STOP:
      bf3_destroy_remote_maps(c);
      result = bf3_publish_status(c, DAQIRI_PCIE_STATUS_FLAG_QUIESCED, 0);
      break;
    case DAQIRI_BF3_CMD_RESET:
      bf3_destroy_remote_maps(c);
      ++c->bar.reset_count;
      result = bf3_set_bar(c, offsetof(struct bf3_bar_state, reset_count), &c->bar.reset_count,
                           sizeof(c->bar.reset_count));
      if (result == DOCA_SUCCESS)
        result = bf3_publish_status(c, DAQIRI_PCIE_STATUS_FLAG_QUIESCED, 0);
      break;
    default:
      result = DOCA_ERROR_INVALID_VALUE;
      break;
  }
  if (result == DOCA_SUCCESS)
    result = bf3_set_bar(c, offsetof(struct bf3_bar_state, command), &idle, sizeof(idle));
  if (result != DOCA_SUCCESS)
    fprintf(stderr, "command %u finalization failed: %s\n", c->bar.command,
            doca_error_get_descr(result));
  return result;
}

static void bf3_fatal(struct bf3_doca* c, doca_error_t result) {
  bf3_log_error("controller fatal error", result);
  bf3_destroy_remote_maps(c);
  bf3_publish_status(c, DAQIRI_PCIE_STATUS_FLAG_FATAL | DAQIRI_PCIE_STATUS_FLAG_QUIESCED,
                     (uint32_t)result);
}

static void bf3_cleanup(struct bf3_doca* c) {
  bf3_destroy_remote_maps(c);
  bf3_destroy_mmap(&c->local_mmap);
  free(c->scratch);
  if (c->inventory) doca_buf_inventory_destroy(c->inventory);
  if (c->dma_ctx) doca_ctx_stop(c->dma_ctx);
  if (c->dma) doca_dma_destroy(c->dma);
  if (c->dma_dev) doca_dev_close(c->dma_dev);
  if (c->pci_dev && c->hotplug_state == DOCA_DEVEMU_PCI_HP_STATE_POWER_ON) {
    doca_devemu_pci_dev_hotunplug(c->pci_dev);
    while (c->hotplug_state != DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF) {
      if (!doca_pe_progress(c->pe)) usleep(1000);
    }
  }
  if (c->pci_ctx) doca_ctx_stop(c->pci_ctx);
  if (c->pci_dev) doca_devemu_pci_dev_destroy(c->pci_dev);
  if (c->rep) {
    if (c->created_rep)
      doca_devemu_pci_dev_destroy_rep(c->rep);
    else
      doca_dev_rep_close(c->rep);
  }
  if (c->pci_type) {
    doca_devemu_pci_type_stop(c->pci_type);
    doca_devemu_pci_type_destroy(c->pci_type);
  }
  if (c->manager_dev) doca_dev_close(c->manager_dev);
  if (c->pe) doca_pe_destroy(c->pe);
}

static void bf3_usage(const char* program) {
  printf("Usage: %s [--pci-addr 03:00.0] [--vuid existing-vuid]\n", program);
}

int main(int argc, char** argv) {
  const char* pci_addr = "03:00.0";
  const char* vuid = NULL;
  struct bf3_doca controller = {0};
  struct timespec idle = {.tv_sec = 0, .tv_nsec = BF3_IDLE_NS};
  doca_error_t result;
  int i;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--pci-addr") && i + 1 < argc)
      pci_addr = argv[++i];
    else if (!strcmp(argv[i], "--vuid") && i + 1 < argc)
      vuid = argv[++i];
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      bf3_usage(argv[0]);
      return 0;
    } else {
      bf3_usage(argv[0]);
      return 2;
    }
  }
  signal(SIGINT, bf3_signal);
  signal(SIGTERM, bf3_signal);
  result = bf3_setup_devemu(&controller, pci_addr, vuid);
  if (result == DOCA_SUCCESS) result = bf3_setup_dma(&controller);
  if (result != DOCA_SUCCESS) {
    bf3_log_error("controller setup failed", result);
    bf3_cleanup(&controller);
    return 1;
  }
  printf("DAQIRI BF3 controller ready on manager %s\n", pci_addr);
  while (!bf3_quit) {
    bool made_progress = false;
    int pe_progress = doca_pe_progress(controller.pe);

    if (controller.flr_pending) {
      controller.flr_pending = false;
      bf3_destroy_remote_maps(&controller);
      result = bf3_publish_status(&controller, DAQIRI_PCIE_STATUS_FLAG_QUIESCED, 0);
      if (result != DOCA_SUCCESS) bf3_fatal(&controller, result);
    }
    if (controller.bar_pending) {
      result = bf3_handle_command(&controller);
      if (result != DOCA_SUCCESS) {
        fprintf(stderr, "fatal source: host command\n");
        bf3_fatal(&controller, result);
      }
    }
    if (controller.running) {
      result = bf3_progress_roundtrip(&controller, &made_progress);
      if (result != DOCA_SUCCESS) {
        fprintf(stderr, "fatal source: roundtrip progress\n");
        bf3_fatal(&controller, result);
      }
    }
    if (!pe_progress && !made_progress) nanosleep(&idle, NULL);
  }
  bf3_cleanup(&controller);
  return 0;
}
