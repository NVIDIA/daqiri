// SPDX-License-Identifier: GPL-2.0-only
/* BlueField-3 Generic PCIe endpoint for the DAQIRI PCIe ABI. */
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#include <daqiri/pcie_abi.h>

#include "../daqiri_bf3_regs.h"

#define BF3_MAX_RING_DEPTH 4096U
#define BF3_RING_ALIGN 256U
#define BF3_STOP_TIMEOUT_MS 5000U

struct bf3_region {
  struct dma_buf* dmabuf;
  struct dma_buf_attachment* attachment;
  struct sg_table* sgt;
  struct daqiri_pcie_ioctl_register_region desc;
  dma_addr_t dma_addr;
};
struct bf3_dev {
  struct pci_dev* pdev;
  void __iomem* bar0;
  struct miscdevice misc;
  struct mutex lock;
  bool opened;
  bool running;
  u64 epoch;
  struct bf3_region regions[DAQIRI_BF3_REGION_COUNT];
  void* rings;
  dma_addr_t rings_dma;
  size_t rings_bytes;
};

static void bf3_write64(struct bf3_dev* d, u32 lo_reg, u64 value) {
  writel(lower_32_bits(value), d->bar0 + lo_reg);
  writel(upper_32_bits(value), d->bar0 + lo_reg + sizeof(u32));
}

static u64 bf3_read64(struct bf3_dev* d, u32 lo_reg) {
  u32 lo = readl(d->bar0 + lo_reg);
  u32 hi = readl(d->bar0 + lo_reg + sizeof(u32));

  return ((u64)hi << 32) | lo;
}

static ssize_t char_show(struct device* dev, struct device_attribute* attr, char* buf) {
  struct bf3_dev* d = dev_get_drvdata(dev);
  return sysfs_emit(buf, "%u:%u\n", MAJOR(d->misc.this_device->devt),
                    MINOR(d->misc.this_device->devt));
}
static DEVICE_ATTR_RO(char);
static struct attribute* bf3_attrs[] = {&dev_attr_char.attr, NULL};
static const struct attribute_group bf3_attr_group = {.name = "daqiri_pcie", .attrs = bf3_attrs};

static bool bf3_valid_header(const struct daqiri_pcie_ioctl_header* h, size_t n) {
  return h->magic == DAQIRI_PCIE_ABI_MAGIC && h->version_major == DAQIRI_PCIE_ABI_VERSION_MAJOR &&
         h->version_minor <= DAQIRI_PCIE_ABI_VERSION_MINOR && h->struct_size == n && h->flags == 0;
}

static bool bf3_depth(u32 n) {
  return n <= BF3_MAX_RING_DEPTH && (!n || is_power_of_2(n));
}

static void bf3_clear_region_regs(struct bf3_dev* d, unsigned int index) {
  bf3_write64(d, DAQIRI_BF3_REG_REGION_DMA_LO(index), 0);
  bf3_write64(d, DAQIRI_BF3_REG_REGION_BYTES_LO(index), 0);
  writel(0, d->bar0 + DAQIRI_BF3_REG_REGION_STRIDE(index));
  writel(0, d->bar0 + DAQIRI_BF3_REG_REGION_COUNT(index));
  writel(0, d->bar0 + DAQIRI_BF3_REG_REGION_ID(index));
  writel(index, d->bar0 + DAQIRI_BF3_REG_REGION_DIRECTION(index));
}

static void bf3_detach_region(struct bf3_dev* d, unsigned int index) {
  struct bf3_region* r = &d->regions[index];

  bf3_clear_region_regs(d, index);
  if (r->sgt) dma_buf_unmap_attachment(r->attachment, r->sgt, DMA_BIDIRECTIONAL);
  if (r->attachment) dma_buf_detach(r->dmabuf, r->attachment);
  if (r->dmabuf) dma_buf_put(r->dmabuf);
  memset(r, 0, sizeof(*r));
}

static void bf3_free_rings(struct bf3_dev* d) {
  unsigned int i;

  for (i = 0; i < DAQIRI_PCIE_RING_COUNT; ++i) bf3_write64(d, DAQIRI_BF3_REG_RING_DMA_LO(i), 0);
  writel(0, d->bar0 + DAQIRI_BF3_REG_RING_BYTES);
  if (d->rings) {
    dma_free_coherent(&d->pdev->dev, d->rings_bytes, d->rings, d->rings_dma);
    d->rings = NULL;
    d->rings_dma = 0;
    d->rings_bytes = 0;
  }
}

static int bf3_wait_command(struct bf3_dev* d, u32 expected_status, u32 timeout_ms) {
  unsigned long deadline;
  u32 status;

  if (!timeout_ms) timeout_ms = BF3_STOP_TIMEOUT_MS;
  deadline = jiffies + msecs_to_jiffies(timeout_ms);
  do {
    status = readl(d->bar0 + DAQIRI_BF3_REG_STATUS);
    if (readl(d->bar0 + DAQIRI_BF3_REG_COMMAND) == DAQIRI_BF3_CMD_IDLE) {
      if (status & DAQIRI_PCIE_STATUS_FLAG_FATAL) return -EIO;
      if ((status & expected_status) == expected_status) return 0;
    }
    usleep_range(1000, 2000);
  } while (time_before(jiffies, deadline));
  return -ETIMEDOUT;
}

static int bf3_quiesce(struct bf3_dev* d, u32 timeout_ms) {
  u32 status = readl(d->bar0 + DAQIRI_BF3_REG_STATUS);
  u32 command = readl(d->bar0 + DAQIRI_BF3_REG_COMMAND);
  int e;

  if (command != DAQIRI_BF3_CMD_IDLE) {
    e = bf3_wait_command(d, 0, timeout_ms);
    status = readl(d->bar0 + DAQIRI_BF3_REG_STATUS);
    if (e && !(status & DAQIRI_PCIE_STATUS_FLAG_QUIESCED)) return e;
  }
  if (status & DAQIRI_PCIE_STATUS_FLAG_QUIESCED) {
    d->running = false;
    return 0;
  }
  writel(DAQIRI_BF3_CMD_STOP, d->bar0 + DAQIRI_BF3_REG_COMMAND);
  readl(d->bar0 + DAQIRI_BF3_REG_COMMAND);
  e = bf3_wait_command(d, DAQIRI_PCIE_STATUS_FLAG_QUIESCED, timeout_ms);
  if (!e) d->running = false;
  return e;
}
static int bf3_register_region(struct bf3_dev* d, struct daqiri_pcie_ioctl_register_region* x) {
  struct bf3_region* r;
  struct scatterlist* sg;
  dma_addr_t next_dma;
  u64 mapped_bytes = 0;
  u64 expected_bytes;
  unsigned int index;
  int e, i;

  if (!bf3_valid_header(&x->header, sizeof(*x)) || x->dmabuf_fd < 0 ||
      x->direction > DAQIRI_PCIE_DIRECTION_TX || !x->bytes || !x->slot_count || !x->slot_stride ||
      x->slot_stride % BF3_RING_ALIGN ||
      check_mul_overflow((u64)x->slot_stride, (u64)x->slot_count, &expected_bytes) ||
      expected_bytes > x->bytes)
    return -EINVAL;
  if (d->running) return -EBUSY;
  index = x->direction;
  if (d->regions[index].dmabuf) return -EBUSY;
  r = &d->regions[index];
  r->dmabuf = dma_buf_get(x->dmabuf_fd);
  if (IS_ERR(r->dmabuf)) {
    e = PTR_ERR(r->dmabuf);
    r->dmabuf = NULL;
    return e;
  }
  r->attachment = dma_buf_attach(r->dmabuf, &d->pdev->dev);
  if (IS_ERR(r->attachment)) {
    e = PTR_ERR(r->attachment);
    r->attachment = NULL;
    goto fail;
  }
  r->sgt = dma_buf_map_attachment(r->attachment, DMA_BIDIRECTIONAL);
  if (IS_ERR(r->sgt)) {
    e = PTR_ERR(r->sgt);
    r->sgt = NULL;
    goto fail;
  }
  /*
   * NVIDIA describes a BAR1 dma-buf as one 64 KiB SG entry per GPU page.
   * DevEmu v1 publishes one IOVA range, so accept the multi-entry form when
   * every DMA address is adjacent.  This is still direct BF3 peer DMA; no CPU
   * mapping, copy, or host bounce buffer is involved.
   */
  next_dma = sg_dma_address(r->sgt->sgl);
  for_each_sg(r->sgt->sgl, sg, r->sgt->nents, i) {
    u64 end;

    if (sg_dma_address(sg) != next_dma ||
        check_add_overflow(mapped_bytes, (u64)sg_dma_len(sg), &mapped_bytes) ||
        check_add_overflow((u64)sg_dma_address(sg), (u64)sg_dma_len(sg), &end))
      break;
    next_dma = (dma_addr_t)end;
  }
  if (i != r->sgt->nents || mapped_bytes < x->bytes) {
    dev_err(
        &d->pdev->dev,
        "CUDA DMA-BUF has a non-contiguous BF3 IOVA (nents=%u, contiguous=%llu, required=%llu)\n",
        r->sgt->nents, mapped_bytes, x->bytes);
    e = -ERANGE;
    goto fail;
  }
  r->dma_addr = sg_dma_address(r->sgt->sgl);
  r->desc = *x;
  x->region_id = index + 1; /* stable within a single exclusive session */
  r->desc.region_id = x->region_id;
  bf3_write64(d, DAQIRI_BF3_REG_REGION_DMA_LO(index), r->dma_addr);
  bf3_write64(d, DAQIRI_BF3_REG_REGION_BYTES_LO(index), x->bytes);
  writel(x->slot_stride, d->bar0 + DAQIRI_BF3_REG_REGION_STRIDE(index));
  writel(x->slot_count, d->bar0 + DAQIRI_BF3_REG_REGION_COUNT(index));
  writel(x->region_id, d->bar0 + DAQIRI_BF3_REG_REGION_ID(index));
  writel(x->direction, d->bar0 + DAQIRI_BF3_REG_REGION_DIRECTION(index));
  return 0;

fail:
  bf3_detach_region(d, index);
  return e;
}
static int bf3_configure(struct bf3_dev* d, struct daqiri_pcie_ioctl_configure_queues* x) {
  size_t bytes = 0, alloc_bytes, off = 0;
  int i;

  if (!bf3_valid_header(&x->header, sizeof(*x)) || d->running || d->rings) return -EINVAL;
  for (i = 0; i < DAQIRI_PCIE_RING_COUNT; ++i) {
    size_t ring_bytes;

    if (!bf3_depth(x->requested_depth[i])) return -EINVAL;
    if (!x->requested_depth[i]) continue;
    ring_bytes = sizeof(struct daqiri_pcie_ring_control) +
                 x->requested_depth[i] * sizeof(struct daqiri_pcie_ring_entry);
    if (check_add_overflow(bytes, ALIGN(ring_bytes, 64), &bytes)) return -EOVERFLOW;
  }
  if (!bytes) return -EINVAL;
  alloc_bytes = PAGE_ALIGN(bytes);
  d->rings = dma_alloc_coherent(&d->pdev->dev, alloc_bytes, &d->rings_dma, GFP_KERNEL);
  if (!d->rings) return -ENOMEM;
  memset(d->rings, 0, alloc_bytes);
  d->rings_bytes = alloc_bytes;
  d->epoch = x->epoch;
  x->mmap_offset = 0;
  x->mmap_bytes = alloc_bytes;
  for (i = 0; i < DAQIRI_PCIE_RING_COUNT; ++i) {
    struct daqiri_pcie_ring_control* c;
    size_t ring_bytes;

    if (!x->requested_depth[i]) {
      memset(&x->rings[i], 0, sizeof(x->rings[i]));
      bf3_write64(d, DAQIRI_BF3_REG_RING_DMA_LO(i), 0);
      continue;
    }
    c = (struct daqiri_pcie_ring_control*)((u8*)d->rings + off);
    c->depth = x->requested_depth[i];
    c->mask = c->depth - 1;
    x->rings[i].control_offset = off;
    x->rings[i].entries_offset = off + sizeof(*c);
    x->rings[i].depth = c->depth;
    bf3_write64(d, DAQIRI_BF3_REG_RING_DMA_LO(i), d->rings_dma + off);
    ring_bytes = sizeof(*c) + c->depth * sizeof(struct daqiri_pcie_ring_entry);
    off += ALIGN(ring_bytes, 64);
  }
  bf3_write64(d, DAQIRI_BF3_REG_EPOCH_LO, d->epoch);
  writel(alloc_bytes, d->bar0 + DAQIRI_BF3_REG_RING_BYTES);
  writel(DAQIRI_BF3_CMD_CONFIGURE, d->bar0 + DAQIRI_BF3_REG_COMMAND);
  readl(d->bar0 + DAQIRI_BF3_REG_COMMAND);
  return bf3_wait_command(d, DAQIRI_PCIE_STATUS_FLAG_QUIESCED, BF3_STOP_TIMEOUT_MS);
}
static long bf3_ioctl(struct file* f, unsigned int cmd, unsigned long arg) {
  struct bf3_dev* d = f->private_data;
  void __user* user = (void __user*)arg;
  int e = 0;

  if (mutex_lock_interruptible(&d->lock)) return -ERESTARTSYS;
  switch (cmd) {
    case DAQIRI_PCIE_IOCTL_GET_CAPS: {
      struct daqiri_pcie_ioctl_caps x;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      if (!bf3_valid_header(&x.header, sizeof(x))) {
        e = -EINVAL;
        break;
      }
      memset((u8*)&x + sizeof(x.header), 0, sizeof(x) - sizeof(x.header));
      x.capabilities =
          DAQIRI_PCIE_CAP_DMABUF_PCIE | DAQIRI_PCIE_CAP_DMA_FENCE | DAQIRI_PCIE_CAP_DEVICE_RESET;
      x.max_regions = DAQIRI_BF3_REGION_COUNT;
      x.max_ring_depth = BF3_MAX_RING_DEPTH;
      x.min_slot_alignment = BF3_RING_ALIGN;
      e = copy_to_user(user, &x, sizeof(x)) ? -EFAULT : 0;
      break;
    }
    case DAQIRI_PCIE_IOCTL_REGISTER_REGION: {
      struct daqiri_pcie_ioctl_register_region x;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      e = bf3_register_region(d, &x);
      if (!e && copy_to_user(user, &x, sizeof(x))) {
        bf3_detach_region(d, x.direction);
        e = -EFAULT;
      }
      break;
    }
    case DAQIRI_PCIE_IOCTL_UNREGISTER_REGION: {
      struct daqiri_pcie_ioctl_unregister_region x;
      unsigned int index;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      if (!bf3_valid_header(&x.header, sizeof(x)) || x.region_id < 1 ||
          x.region_id > DAQIRI_BF3_REGION_COUNT || d->running) {
        e = -EINVAL;
        break;
      }
      index = x.region_id - 1;
      if (!d->regions[index].dmabuf || d->regions[index].desc.region_id != x.region_id) {
        e = -ENOENT;
        break;
      }
      if (!(readl(d->bar0 + DAQIRI_BF3_REG_STATUS) & DAQIRI_PCIE_STATUS_FLAG_QUIESCED)) {
        e = -EBUSY;
        break;
      }
      bf3_detach_region(d, index);
      break;
    }
    case DAQIRI_PCIE_IOCTL_CONFIGURE_QUEUES: {
      struct daqiri_pcie_ioctl_configure_queues x;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      e = bf3_configure(d, &x);
      if (!e && copy_to_user(user, &x, sizeof(x))) {
        bf3_free_rings(d);
        e = -EFAULT;
      }
      break;
    }
    case DAQIRI_PCIE_IOCTL_START: {
      struct daqiri_pcie_ioctl_start x;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      if (!bf3_valid_header(&x.header, sizeof(x)) || x.epoch != d->epoch || !d->rings ||
          d->running) {
        e = -EINVAL;
        break;
      }
      writel(DAQIRI_BF3_CMD_START, d->bar0 + DAQIRI_BF3_REG_COMMAND);
      readl(d->bar0 + DAQIRI_BF3_REG_COMMAND);
      e = bf3_wait_command(d, DAQIRI_PCIE_STATUS_FLAG_RUNNING, BF3_STOP_TIMEOUT_MS);
      if (!e) d->running = true;
      break;
    }
    case DAQIRI_PCIE_IOCTL_STOP: {
      struct daqiri_pcie_ioctl_stop x;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      if (!bf3_valid_header(&x.header, sizeof(x))) {
        e = -EINVAL;
        break;
      }
      e = bf3_quiesce(d, x.timeout_ms);
      x.quiesced = e == 0;
      if (copy_to_user(user, &x, sizeof(x))) e = -EFAULT;
      break;
    }
    case DAQIRI_PCIE_IOCTL_RESET: {
      struct daqiri_pcie_ioctl_reset x;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      if (!bf3_valid_header(&x.header, sizeof(x))) {
        e = -EINVAL;
        break;
      }
      e = bf3_quiesce(d, BF3_STOP_TIMEOUT_MS);
      if (e) break;
      d->epoch = x.new_epoch;
      bf3_write64(d, DAQIRI_BF3_REG_EPOCH_LO, d->epoch);
      writel(DAQIRI_BF3_CMD_RESET, d->bar0 + DAQIRI_BF3_REG_COMMAND);
      readl(d->bar0 + DAQIRI_BF3_REG_COMMAND);
      e = bf3_wait_command(d, DAQIRI_PCIE_STATUS_FLAG_QUIESCED, BF3_STOP_TIMEOUT_MS);
      break;
    }
    case DAQIRI_PCIE_IOCTL_GET_STATUS: {
      struct daqiri_pcie_ioctl_status x;

      if (copy_from_user(&x, user, sizeof(x))) {
        e = -EFAULT;
        break;
      }
      if (!bf3_valid_header(&x.header, sizeof(x))) {
        e = -EINVAL;
        break;
      }
      memset((u8*)&x + sizeof(x.header), 0, sizeof(x) - sizeof(x.header));
      x.status_flags = readl(d->bar0 + DAQIRI_BF3_REG_STATUS);
      x.fatal_code = readl(d->bar0 + DAQIRI_BF3_REG_FATAL_CODE);
      x.reset_count = bf3_read64(d, DAQIRI_BF3_REG_RESET_COUNT_LO);
      x.rx_completions = bf3_read64(d, DAQIRI_BF3_REG_RX_COMPLETIONS_LO);
      x.tx_completions = bf3_read64(d, DAQIRI_BF3_REG_TX_COMPLETIONS_LO);
      e = copy_to_user(user, &x, sizeof(x)) ? -EFAULT : 0;
      break;
    }
    default:
      e = -ENOTTY;
  }
  mutex_unlock(&d->lock);
  return e;
}
static int bf3_open(struct inode* inode, struct file* file) {
  struct bf3_dev* d = container_of(file->private_data, struct bf3_dev, misc);
  int e = 0;

  if (mutex_lock_interruptible(&d->lock)) return -ERESTARTSYS;
  if (d->opened)
    e = -EBUSY;
  else {
    d->opened = true;
    file->private_data = d;
  }
  mutex_unlock(&d->lock);
  return e ?: nonseekable_open(inode, file);
}

static int bf3_release(struct inode* inode, struct file* file) {
  struct bf3_dev* d = file->private_data;
  unsigned int i;

  mutex_lock(&d->lock);
  if (bf3_quiesce(d, BF3_STOP_TIMEOUT_MS))
    dev_err(&d->pdev->dev, "device did not quiesce during close; retaining DMA mappings\n");
  else {
    for (i = 0; i < DAQIRI_BF3_REGION_COUNT; ++i) bf3_detach_region(d, i);
    bf3_free_rings(d);
  }
  d->opened = false;
  mutex_unlock(&d->lock);
  return 0;
}

static int bf3_mmap(struct file* file, struct vm_area_struct* vma) {
  struct bf3_dev* d = file->private_data;
  size_t requested = vma->vm_end - vma->vm_start;

  if (!d->rings || vma->vm_pgoff || requested != d->rings_bytes) return -EINVAL;
  return dma_mmap_coherent(&d->pdev->dev, vma, d->rings, d->rings_dma, d->rings_bytes);
}

static const struct file_operations bf3_fops = {
    .owner = THIS_MODULE,
    .open = bf3_open,
    .release = bf3_release,
    .unlocked_ioctl = bf3_ioctl,
    .compat_ioctl = compat_ptr_ioctl,
    .mmap = bf3_mmap,
    .llseek = no_llseek,
};

static int bf3_probe(struct pci_dev* pdev, const struct pci_device_id* id) {
  struct bf3_dev* d;
  int e;

  e = pci_enable_device_mem(pdev);
  if (e) return e;
  e = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
  if (e) goto disable;
  e = pci_request_region(pdev, 0, "daqiri_bf3_pcie");
  if (e) goto disable;
  pci_set_master(pdev);
  d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
  if (!d) {
    e = -ENOMEM;
    goto release_region;
  }
  d->pdev = pdev;
  mutex_init(&d->lock);
  d->bar0 = pci_iomap(pdev, 0, DAQIRI_BF3_BAR0_SIZE);
  if (!d->bar0) {
    e = -ENODEV;
    goto release_region;
  }
  if (readl(d->bar0 + DAQIRI_BF3_REG_ABI_MAGIC) != DAQIRI_PCIE_ABI_MAGIC) {
    dev_err(&pdev->dev, "DevEmu BAR does not expose the DAQIRI ABI magic\n");
    e = -EPROTO;
    goto unmap;
  }
  d->misc.minor = MISC_DYNAMIC_MINOR;
  d->misc.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "daqiri-pcie-%s", pci_name(pdev));
  d->misc.fops = &bf3_fops;
  d->misc.parent = &pdev->dev;
  if (!d->misc.name) {
    e = -ENOMEM;
    goto unmap;
  }
  pci_set_drvdata(pdev, d);
  e = misc_register(&d->misc);
  if (e) goto unmap;
  e = sysfs_create_group(&pdev->dev.kobj, &bf3_attr_group);
  if (e) goto deregister;
  dev_info(&pdev->dev, "DAQIRI BF3 PCIe endpoint ready as /dev/%s\n", d->misc.name);
  return 0;

deregister:
  misc_deregister(&d->misc);
unmap:
  pci_iounmap(pdev, d->bar0);
release_region:
  pci_clear_master(pdev);
  pci_release_region(pdev, 0);
disable:
  pci_disable_device(pdev);
  return e;
}

static void bf3_remove(struct pci_dev* pdev) {
  struct bf3_dev* d = pci_get_drvdata(pdev);
  unsigned int i;

  sysfs_remove_group(&pdev->dev.kobj, &bf3_attr_group);
  misc_deregister(&d->misc);
  mutex_lock(&d->lock);
  if (bf3_quiesce(d, BF3_STOP_TIMEOUT_MS))
    dev_err(&pdev->dev, "forcing removal after device quiesce timeout\n");
  for (i = 0; i < DAQIRI_BF3_REGION_COUNT; ++i) bf3_detach_region(d, i);
  bf3_free_rings(d);
  mutex_unlock(&d->lock);
  pci_iounmap(pdev, d->bar0);
  pci_clear_master(pdev);
  pci_release_region(pdev, 0);
  pci_disable_device(pdev);
}

static const struct pci_device_id bf3_ids[] = {
    {PCI_DEVICE(DAQIRI_BF3_VENDOR_ID, DAQIRI_BF3_DEVICE_ID)},
    {},
};
MODULE_DEVICE_TABLE(pci, bf3_ids);

static struct pci_driver bf3_driver = {
    .name = "daqiri_bf3_pcie",
    .id_table = bf3_ids,
    .probe = bf3_probe,
    .remove = bf3_remove,
};
module_pci_driver(bf3_driver);

MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("DAQIRI BlueField-3 DevEmu PCIe endpoint");
MODULE_LICENSE("GPL");
