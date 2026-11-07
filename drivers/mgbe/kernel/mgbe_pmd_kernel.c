/* SPDX-License-Identifier: GPL-2.0-only */
/* Included at the end of ether_linux.c, allowing reuse of its hardware control
 * helpers without exporting driver internals as a second kernel ABI. */
#include "mgbe_pmd_uapi.h"
#pragma push_macro("MAX_REG_OFFSET")
#undef MAX_REG_OFFSET
#include "nvethernetrm/osi/dma/mgbe_dma.h"
#pragma pop_macro("MAX_REG_OFFSET")
#include <linux/capability.h>
#include <linux/dma-map-ops.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/rtnetlink.h>
#include <linux/uaccess.h>

static char* pmd_interface = "";
module_param(pmd_interface, charp, 0400);
MODULE_PARM_DESC(pmd_interface,
                 "Experimental exclusive userspace port, e.g. mgbe0_0 (default off)");

#define PMD_DMA_PAGE 0x3000U
#define PMD_DMA_STATUS 0x3160U
#define PMD_FATAL_BUS_ERROR BIT(12)

struct pmd_memory {
  void* cpu;
  dma_addr_t dma;
  size_t size;
};
struct mgbe_pmd {
  struct miscdevice misc;
  struct kref refs;
  struct mutex coe_gate;
  struct ether_priv_data* pdata;
  struct address_space* mapping;
  struct mgbe_pmd_status* status;
  struct delayed_work monitor;
  struct pmd_memory memory[MGBE_PMD_REGIONS];
  struct osi_rx_ring rx;
  struct osi_tx_ring tx;
  struct mgbe_pmd_info info;
  bool owner, hw_open, terminal, saved, sysfs_removed, mac_loopback;
  const struct net_device_ops* net_ops;
  const struct ethtool_ops* ethtool_ops;
  netdev_features_t features;
  u32 old_mtu, old_dma_mtu, old_core_mtu, old_rx_len;
  u32 old_num_dma, old_core_num_dma, old_num_coe, old_num_mtl, old_ring_rx, old_ring_tx;
  u32 old_prio, old_ctrl, old_residual, old_ptp_queue;
  u32 old_riwt, old_tx_usecs, old_rx_frames, old_tx_frames;
};

bool mgbe_pmd_owned(struct ether_priv_data* pdata) {
  return pdata->pmd && READ_ONCE(pdata->pmd->owner);
}

int mgbe_pmd_net_open(struct net_device* ndev) {
  if (mgbe_pmd_owned(netdev_priv(ndev))) return -EBUSY;
  return ether_open(ndev);
}

static int pmd_deny_open(struct net_device* ndev) {
  return -EBUSY;
}
/* Kernel network configuration is unavailable while the character device owns
 * the port. The real ndo_open is called directly for control-plane bring-up. */
static int pmd_deny_mtu(struct net_device* ndev, int mtu) {
  return -EBUSY;
}
static int pmd_deny_mac(struct net_device* ndev, void* address) {
  return -EBUSY;
}
static const struct net_device_ops pmd_net_ops = {
    .ndo_open = pmd_deny_open,
    .ndo_change_mtu = pmd_deny_mtu,
    .ndo_set_mac_address = pmd_deny_mac,
};
static const struct ethtool_ops pmd_ethtool_ops = {};

static void pmd_stop_dma(struct mgbe_pmd* p) {
  void __iomem* base = p->pdata->osi_dma->base;
  u32 value;

  writel(0, base + MGBE_DMA_CHX_INTR_ENA(0));
  value = readl(base + MGBE_DMA_CHX_TX_CTRL(0));
  writel(value & ~BIT(0), base + MGBE_DMA_CHX_TX_CTRL(0));
  value = readl(base + MGBE_DMA_CHX_RX_CTRL(0));
  writel((value & ~BIT(0)) | BIT(31), base + MGBE_DMA_CHX_RX_CTRL(0));
  readl(base + MGBE_DMA_CHX_RX_CTRL(0));
}

bool mgbe_pmd_link_event(struct ether_priv_data* pdata, unsigned int enable) {
  struct mgbe_pmd* p = pdata->pmd;

  if (!p || !READ_ONCE(p->owner)) return false;
  if (READ_ONCE(p->mac_loopback)) return true;
  if (!enable) {
    netif_carrier_off(pdata->ndev);
    WRITE_ONCE(p->status->link, 0);
    if (READ_ONCE(p->status->state) == MGBE_PMD_RUNNING) {
      WRITE_ONCE(p->status->fault, MGBE_PMD_FAULT_LINK);
      smp_store_release(&p->status->state, MGBE_PMD_FAULT);
      pmd_stop_dma(p);
    }
  }
  /* A faulted userspace session must never be silently restarted by a tasklet. */
  return true;
}

static void pmd_monitor(struct work_struct* work) {
  struct mgbe_pmd* p = container_of(to_delayed_work(work), struct mgbe_pmd, monitor);

  /* close/remove hold RTNL while cancelling this work; never wait for RTNL. */
  if (rtnl_trylock()) {
    if (p->pdata && p->hw_open) {
      bool link = p->mac_loopback || netif_carrier_ok(p->pdata->ndev);
      u32 status = readl(p->pdata->osi_dma->base + PMD_DMA_STATUS);

      WRITE_ONCE(p->status->link, link);
      WRITE_ONCE(p->status->dma_status, status);
      if (READ_ONCE(p->status->state) == MGBE_PMD_RUNNING &&
          (!link || (status & PMD_FATAL_BUS_ERROR))) {
        WRITE_ONCE(p->status->fault, link ? MGBE_PMD_FAULT_DMA : MGBE_PMD_FAULT_LINK);
        smp_store_release(&p->status->state, MGBE_PMD_FAULT);
        pmd_stop_dma(p);
      }
    }
    rtnl_unlock();
  }
  schedule_delayed_work(&p->monitor, msecs_to_jiffies(10));
}

static void pmd_restore(struct mgbe_pmd* p) {
  struct ether_priv_data* pd = p->pdata;
  struct osi_dma_priv_data* d = pd->osi_dma;
  struct osi_core_priv_data* c = pd->osi_core;

  if (!p->saved) return;
  pd->ndev->mtu = p->old_mtu;
  pd->ndev->features = p->features;
  d->mtu = p->old_dma_mtu;
  d->rx_buf_len = p->old_rx_len;
  d->num_dma_chans = p->old_num_dma;
  d->num_dma_chans_coe = p->old_num_coe;
  d->rx_ring_sz = p->old_ring_rx;
  d->tx_ring_sz = p->old_ring_tx;
  d->use_riwt = p->old_riwt;
  d->use_tx_usecs = p->old_tx_usecs;
  d->use_rx_frames = p->old_rx_frames;
  d->use_tx_frames = p->old_tx_frames;
  d->exclusive_polling = 0;
  d->rx_ring[0] = NULL;
  d->tx_ring[0] = NULL;
  c->mtu = p->old_core_mtu;
  c->num_mtl_queues = p->old_num_mtl;
  c->num_dma_chans = p->old_core_num_dma;
  c->rxq_prio[0] = p->old_prio;
  c->rxq_ctrl[0] = p->old_ctrl;
  c->residual_queue = p->old_residual;
  c->ptp_config.ptp_rx_queue = p->old_ptp_queue;
  p->saved = false;
}

static void pmd_free_dma(struct mgbe_pmd* p) {
  unsigned int i;

  for (i = MGBE_PMD_RX_RING; i <= MGBE_PMD_TX_POOL; ++i) {
    struct pmd_memory* m = &p->memory[i];
    if (m->cpu) {
      dma_free_coherent(p->pdata->dev, m->size, m->cpu, m->dma);
      memset(m, 0, sizeof(*m));
    }
  }
  kfree(p->rx.rx_swcx);
  kfree(p->tx.tx_swcx);
  memset(&p->rx, 0, sizeof(p->rx));
  memset(&p->tx, 0, sizeof(p->tx));
}

static void pmd_shutdown(struct mgbe_pmd* p) {
  cancel_delayed_work_sync(&p->monitor);
  p->terminal = true;
  if (p->hw_open) {
    smp_store_release(&p->status->state, MGBE_PMD_IDLE);
    pmd_stop_dma(p);
    /* Vendor close stops PHY/work/IRQs/DMA, asserts MAC and PCS resets and
     * releases power. Keep DMA memory until AFTER this reset completes. */
    ether_close(p->pdata->ndev);
    p->hw_open = false;
    WRITE_ONCE(p->mac_loopback, false);
  }
}

static int pmd_allocate(struct mgbe_pmd* p, const struct mgbe_pmd_config* cfg) {
  unsigned int i;
  size_t ring_bytes = PAGE_ALIGN(cfg->ring_size * sizeof(struct osi_rx_desc));
  size_t slot = cfg->mtu <= 1500 ? 2048 : 16384;

  for (i = MGBE_PMD_RX_RING; i <= MGBE_PMD_TX_POOL; ++i) {
    struct pmd_memory* m = &p->memory[i];
    m->size = i <= MGBE_PMD_TX_RING ? ring_bytes : slot * cfg->ring_size;
    m->cpu = dma_alloc_coherent(p->pdata->dev, m->size, &m->dma, GFP_KERNEL);
    if (!m->cpu) return -ENOMEM;
    memset(m->cpu, 0, m->size);
    if (i <= MGBE_PMD_TX_RING && (u64)lower_32_bits(m->dma) + cfg->ring_size * 16ULL > U32_MAX)
      return -ERANGE;
  }
  p->rx.rx_swcx = kcalloc(cfg->ring_size, sizeof(*p->rx.rx_swcx), GFP_KERNEL);
  p->tx.tx_swcx = kcalloc(cfg->ring_size, sizeof(*p->tx.tx_swcx), GFP_KERNEL);
  if (!p->rx.rx_swcx || !p->tx.tx_swcx) return -ENOMEM;
  p->rx.rx_desc = p->memory[MGBE_PMD_RX_RING].cpu;
  p->rx.rx_desc_phy_addr = p->memory[MGBE_PMD_RX_RING].dma;
  p->tx.tx_desc = p->memory[MGBE_PMD_TX_RING].cpu;
  p->tx.tx_desc_phy_addr = p->memory[MGBE_PMD_TX_RING].dma;
  for (i = 0; i < cfg->ring_size; ++i) {
    p->rx.rx_swcx[i].buf_virt_addr = p->memory[MGBE_PMD_RX_POOL].cpu + i * slot;
    p->rx.rx_swcx[i].buf_phy_addr = p->memory[MGBE_PMD_RX_POOL].dma + i * slot;
  }
  p->info = (struct mgbe_pmd_info){
      .abi = MGBE_PMD_ABI,
      .ring_size = cfg->ring_size,
      .mtu = cfg->mtu,
      .slot_size = slot,
      .channel = 0,
      .reg_size = PAGE_SIZE,
      .rx_tail_offset = MGBE_DMA_CHX_RDTLP(0) - PMD_DMA_PAGE,
      .tx_tail_offset = MGBE_DMA_CHX_TDTLP(0) - PMD_DMA_PAGE,
      .rx_completed_offset = MGBE_DMA_CHX_RX_DESC_WR_RNG_OFFSET(0) - PMD_DMA_PAGE,
      .rx_ring_dma = p->memory[MGBE_PMD_RX_RING].dma,
      .tx_ring_dma = p->memory[MGBE_PMD_TX_RING].dma,
      .rx_pool_dma = p->memory[MGBE_PMD_RX_POOL].dma,
      .tx_pool_dma = p->memory[MGBE_PMD_TX_POOL].dma,
  };
  ether_addr_copy(p->info.mac, p->pdata->ndev->dev_addr);
  return 0;
}

int mgbe_pmd_attach_rings(struct ether_priv_data* pdata) {
  struct mgbe_pmd* p = pdata->pmd;

  ether_init_invalid_chan_ring(pdata->osi_dma);
  pdata->osi_dma->rx_ring[0] = &p->rx;
  pdata->osi_dma->tx_ring[0] = &p->tx;
  return 0;
}

static int pmd_configure(struct mgbe_pmd* p, const struct mgbe_pmd_config* cfg) {
  struct ether_priv_data* pd = p->pdata;
  struct osi_dma_priv_data* d = pd->osi_dma;
  struct osi_core_priv_data* c = pd->osi_core;
  int ret;

  if (cfg->abi != MGBE_PMD_ABI || cfg->reserved || cfg->ring_size < 64 ||
      cfg->ring_size > MGBE_PMD_MAX_RING || !is_power_of_2(cfg->ring_size) || cfg->mtu < 68 ||
      cfg->mtu > min_t(u32, 9000, pd->ndev->max_mtu))
    return -EINVAL;
  if (p->saved || p->terminal) return -EBUSY;
  ret = pmd_allocate(p, cfg);
  if (ret) {
    pmd_free_dma(p);
    return ret;
  }
  p->old_mtu = pd->ndev->mtu;
  p->features = pd->ndev->features;
  p->old_dma_mtu = d->mtu;
  p->old_core_mtu = c->mtu;
  p->old_rx_len = d->rx_buf_len;
  p->old_num_dma = d->num_dma_chans;
  p->old_num_coe = d->num_dma_chans_coe;
  p->old_core_num_dma = c->num_dma_chans;
  p->old_num_mtl = c->num_mtl_queues;
  p->old_ring_rx = d->rx_ring_sz;
  p->old_ring_tx = d->tx_ring_sz;
  p->old_riwt = d->use_riwt;
  p->old_tx_usecs = d->use_tx_usecs;
  p->old_rx_frames = d->use_rx_frames;
  p->old_tx_frames = d->use_tx_frames;
  p->old_prio = c->rxq_prio[0];
  p->old_ctrl = c->rxq_ctrl[0];
  p->old_residual = c->residual_queue;
  p->old_ptp_queue = c->ptp_config.ptp_rx_queue;
  p->saved = true;
  pd->ndev->mtu = cfg->mtu;
  pd->ndev->features &= ~NETIF_F_RXHASH;
  d->mtu = cfg->mtu;
  c->mtu = cfg->mtu;
  d->rx_ring_sz = d->tx_ring_sz = cfg->ring_size;
  d->num_dma_chans = c->num_dma_chans = c->num_mtl_queues = 1;
  /* DT reserves camera VM channels even when no camera owns them. Do not
   * initialize those inactive channels for an exclusive ordinary-packet port. */
  d->num_dma_chans_coe = 0;
  d->use_riwt = d->use_tx_usecs = d->use_rx_frames = d->use_tx_frames = 0;
  d->exclusive_polling = 1;
  c->rxq_prio[0] = 0xff;
  c->rxq_ctrl[0] = 2;
  c->residual_queue = c->ptp_config.ptp_rx_queue = 0;
  ret = ether_open(pd->ndev);
  if (ret) {
    pmd_restore(p);
    pmd_free_dma(p);
    return ret;
  }
  p->hw_open = true;
  /* Serialize MMC reads through the control ioctl instead of racing the
   * vendor's periodic reset-on-read counter accumulator. */
  ether_stats_work_queue_stop(pd);
  pmd_stop_dma(p);
  /* No RX IOC bits or timestamp context descriptors in the polling path. */
  for (ret = 0; ret < cfg->ring_size; ++ret) p->rx.rx_desc[ret].rdes3 &= ~BIT(30);
  dma_wmb();
  smp_store_release(&p->status->state, MGBE_PMD_CONFIGURED);
  schedule_delayed_work(&p->monitor, msecs_to_jiffies(10));
  return 0;
}

static void pmd_destroy(struct kref* refs) {
  struct mgbe_pmd* p = container_of(refs, struct mgbe_pmd, refs);

  free_page((unsigned long)p->status);
  kfree(p->misc.name);
  kfree(p);
}

static int pmd_open(struct inode* inode, struct file* file) {
  struct mgbe_pmd* p = container_of(file->private_data, struct mgbe_pmd, misc);
  struct ether_priv_data* pd;
  int ret = 0;

  if (!capable(CAP_SYS_RAWIO)) return -EPERM;
  rtnl_lock();
  mutex_lock(&p->coe_gate);
  pd = p->pdata;
  if (!pd || p->owner) {
    ret = pd ? -EBUSY : -ENODEV;
    goto out;
  }
  if (netif_running(pd->ndev) || pd->coe_enable || pd->osi_core->use_virtualization ||
      pd->osi_dma->dma_chans[0] != 0 || pd->osi_core->mtl_queues[0] != 0) {
    ret = -EBUSY;
    goto out;
  }
#ifdef MACSEC_SUPPORT
  if (pd->macsec_pdata && pd->macsec_pdata->coe.enable) {
    ret = -EBUSY;
    goto out;
  }
#endif
  /* Supported initial target only; an annotation alone is not a correctness
   * test. The benchmark must still validate recycled payloads on real traffic. */
  if (PAGE_SIZE != 4096 || !dev_is_dma_coherent(pd->dev)) {
    ret = -EOPNOTSUPP;
    goto out;
  }
  kref_get(&p->refs);
  p->net_ops = pd->ndev->netdev_ops;
  p->ethtool_ops = pd->ndev->ethtool_ops;
  pd->ndev->netdev_ops = &pmd_net_ops;
  pd->ndev->ethtool_ops = &pmd_ethtool_ops;
  ether_sysfs_unregister(pd);
  p->sysfs_removed = true;
  p->terminal = false;
  p->mac_loopback = false;
  memset(p->status, 0, PAGE_SIZE);
  p->info = (struct mgbe_pmd_info){.abi = MGBE_PMD_ABI};
  WRITE_ONCE(p->owner, true);
  p->mapping = file->f_mapping;
  file->private_data = p;
out:
  mutex_unlock(&p->coe_gate);
  rtnl_unlock();
  return ret;
}

static int pmd_release(struct inode* inode, struct file* file) {
  struct mgbe_pmd* p = file->private_data;

  rtnl_lock();
  if (p->pdata) {
    pmd_shutdown(p);
    pmd_restore(p);
    pmd_free_dma(p);
    p->pdata->ndev->netdev_ops = p->net_ops;
    p->pdata->ndev->ethtool_ops = p->ethtool_ops;
    if (p->sysfs_removed && ether_sysfs_register(p->pdata))
      dev_warn(p->pdata->dev, "PMD: failed to restore sysfs controls\n");
    p->sysfs_removed = false;
  }
  p->mapping = NULL;
  smp_store_release(&p->owner, false);
  rtnl_unlock();
  kref_put(&p->refs, pmd_destroy);
  return 0;
}

static long pmd_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
  struct mgbe_pmd* p = file->private_data;
  struct mgbe_pmd_config cfg;
  void __user* user = (void __user*)arg;
  long ret = 0;

  rtnl_lock();
  if (!p->pdata) {
    ret = -ENODEV;
    goto out;
  }
  switch (cmd) {
    case MGBE_PMD_IOC_INFO:
      ret = copy_to_user(user, &p->info, sizeof(p->info)) ? -EFAULT : 0;
      break;
    case MGBE_PMD_IOC_CONFIGURE:
      ret = copy_from_user(&cfg, user, sizeof(cfg)) ? -EFAULT : pmd_configure(p, &cfg);
      break;
    case MGBE_PMD_IOC_START:
      if (!p->hw_open || p->terminal || p->status->state != MGBE_PMD_CONFIGURED) {
        ret = -EINVAL;
        break;
      }
      if (!p->mac_loopback && !netif_carrier_ok(p->pdata->ndev)) {
        ret = -ENETDOWN;
        break;
      }
      dma_wmb();
      writel(readl(p->pdata->osi_dma->base + MGBE_DMA_CHX_TX_CTRL(0)) | BIT(0),
             p->pdata->osi_dma->base + MGBE_DMA_CHX_TX_CTRL(0));
      writel((readl(p->pdata->osi_dma->base + MGBE_DMA_CHX_RX_CTRL(0)) | BIT(0)) & ~BIT(31),
             p->pdata->osi_dma->base + MGBE_DMA_CHX_RX_CTRL(0));
      WRITE_ONCE(p->status->link, 1);
      smp_store_release(&p->status->state, MGBE_PMD_RUNNING);
      break;
    case MGBE_PMD_IOC_MAC_LOOPBACK: {
      struct osi_ioctl command = {.cmd = OSI_CMD_MAC_LB, .arg1_u32 = OSI_ENABLE};

      if (!p->hw_open || p->terminal || p->status->state != MGBE_PMD_CONFIGURED) {
        ret = -EINVAL;
        break;
      }
      if (osi_handle_ioctl(p->pdata->osi_core, &command)) {
        ret = -EIO;
        break;
      }
      p->pdata->mac_loopback_mode = OSI_ENABLE;
      WRITE_ONCE(p->mac_loopback, true);
      WRITE_ONCE(p->status->link, 1);
      break;
    }
    case MGBE_PMD_IOC_STOP:
      pmd_shutdown(p);
      break;
    case MGBE_PMD_IOC_STATS: {
      struct osi_ioctl command = {.cmd = OSI_CMD_READ_MMC};
      struct osi_mmc_counters* m = &p->pdata->osi_core->mmc;
      struct mgbe_pmd_stats stats;

      if (!p->hw_open) {
        ret = -ENETDOWN;
        break;
      }
      if (osi_handle_ioctl(p->pdata->osi_core, &command)) {
        ret = -EIO;
        break;
      }
      /* The vendor independently accumulates low and high halves of each
       * reset-on-read counter. ADD them; OR loses carries in the low sum. */
      stats.rx_bytes = m->mmc_rx_octetcount_gb + (m->mmc_rx_octetcount_gb_h << 32);
      stats.tx_bytes = m->mmc_tx_octetcount_gb + (m->mmc_tx_octetcount_gb_h << 32);
      stats.rx_frames = m->mmc_rx_framecount_gb + (m->mmc_rx_framecount_gb_h << 32);
      stats.tx_frames = m->mmc_tx_framecount_gb + (m->mmc_tx_framecount_gb_h << 32);
      stats.rx_crc_errors = m->mmc_rx_crc_error + (m->mmc_rx_crc_error_h << 32);
      stats.rx_fifo_overflows = m->mmc_rx_fifo_overflow + (m->mmc_rx_fifo_overflow_h << 32);
      ret = copy_to_user(user, &stats, sizeof(stats)) ? -EFAULT : 0;
      break;
    }
    default:
      ret = -ENOTTY;
  }
out:
  rtnl_unlock();
  return ret;
}

static int pmd_mmap(struct file* file, struct vm_area_struct* vma) {
  struct mgbe_pmd* p = file->private_data;
  unsigned long region = vma->vm_pgoff;
  size_t length = vma->vm_end - vma->vm_start;
  struct resource* resource;
  int ret = -EINVAL;

  if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC)) return -EINVAL;
  rtnl_lock();
  if (!p->pdata) {
    ret = -ENODEV;
    goto out;
  }
  if (region >= MGBE_PMD_REGIONS || !p->hw_open) goto out;
  vm_flags_set(vma, VM_DONTCOPY | VM_DONTDUMP);
  vm_flags_clear(vma, VM_MAYEXEC);
  vma->vm_pgoff = 0;
  if (region == MGBE_PMD_STATUS) {
    if (length != PAGE_SIZE || (vma->vm_flags & VM_WRITE)) goto out;
    vm_flags_clear(vma, VM_MAYWRITE);
    ret = remap_pfn_range(vma, vma->vm_start, virt_to_phys(p->status) >> PAGE_SHIFT, PAGE_SIZE,
                          vma->vm_page_prot);
  } else if (region == MGBE_PMD_REGS) {
    resource =
        platform_get_resource_byname(to_platform_device(p->pdata->dev), IORESOURCE_MEM, "mac");
    if (!resource || length != PAGE_SIZE || resource_size(resource) < PMD_DMA_PAGE + PAGE_SIZE)
      goto out;
    vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
    ret = io_remap_pfn_range(vma, vma->vm_start, (resource->start + PMD_DMA_PAGE) >> PAGE_SHIFT,
                             PAGE_SIZE, vma->vm_page_prot);
  } else {
    struct pmd_memory* m = &p->memory[region];
    if (length != m->size) goto out;
    ret = dma_mmap_coherent(p->pdata->dev, vma, m->cpu, m->dma, m->size);
  }
out:
  rtnl_unlock();
  return ret;
}

static const struct file_operations pmd_fops = {
    .owner = THIS_MODULE,
    .open = pmd_open,
    .release = pmd_release,
    .unlocked_ioctl = pmd_ioctl,
    .compat_ioctl = compat_ptr_ioctl,
    .mmap = pmd_mmap,
    .llseek = no_llseek,
};

void mgbe_pmd_register(struct ether_priv_data* pdata) {
  struct mgbe_pmd* p;
  int ret;

  if (!*pmd_interface || strcmp(pmd_interface, pdata->ndev->name) ||
      pdata->osi_core->mac != OSI_MAC_HW_MGBE_T26X)
    return;
  p = kzalloc(sizeof(*p), GFP_KERNEL);
  if (!p) return;
  p->status = (void*)get_zeroed_page(GFP_KERNEL);
  p->misc.name = kasprintf(GFP_KERNEL, "mgbe-pmd-%s", pdata->ndev->name);
  if (!p->status || !p->misc.name) {
    if (p->status) free_page((unsigned long)p->status);
    kfree(p->misc.name);
    kfree(p);
    return;
  }
  kref_init(&p->refs);
  mutex_init(&p->coe_gate);
  INIT_DELAYED_WORK(&p->monitor, pmd_monitor);
  p->pdata = pdata;
  p->info.abi = MGBE_PMD_ABI;
  p->misc.minor = MISC_DYNAMIC_MINOR;
  p->misc.fops = &pmd_fops;
  p->misc.parent = pdata->dev;
  p->misc.mode = 0600;
  pdata->pmd = p;
  ret = misc_register(&p->misc);
  if (ret) {
    pdata->pmd = NULL;
    kref_put(&p->refs, pmd_destroy);
    dev_warn(pdata->dev, "PMD registration failed: %d\n", ret);
  }
}

bool mgbe_pmd_unregister(struct ether_priv_data* pdata) {
  struct mgbe_pmd* p = pdata->pmd;
  bool sysfs_removed;

  if (!p) return false;
  misc_deregister(&p->misc);
  rtnl_lock();
  if (p->owner) {
    pmd_shutdown(p);
    /* Revoke both DMA and MMIO mappings before vendor resource teardown. */
    unmap_mapping_range(p->mapping, 0, 0, 1);
    pmd_restore(p);
    pmd_free_dma(p);
    pdata->ndev->netdev_ops = p->net_ops;
    pdata->ndev->ethtool_ops = p->ethtool_ops;
    WRITE_ONCE(p->status->state, MGBE_PMD_REMOVED);
  }
  p->pdata = NULL;
  pdata->pmd = NULL;
  sysfs_removed = p->sysfs_removed;
  rtnl_unlock();
  kref_put(&p->refs, pmd_destroy);
  return sysfs_removed;
}

void mgbe_pmd_platform_shutdown(struct ether_priv_data* pdata) {
  rtnl_lock();
  if (mgbe_pmd_owned(pdata)) pmd_shutdown(pdata->pmd);
  rtnl_unlock();
}

/* CoE configuration may originate outside the netdev ops. Serialize it with
 * acquisition, including a configuration already in progress at open(). */
int nvether_coe_config(struct net_device* ndev, struct nvether_coe_cfg* config) {
  struct ether_priv_data* pdata = netdev_priv(ndev);
  struct mgbe_pmd* p = pdata->pmd;
  int ret;

  if (!p) return pmd_coe_config_impl(ndev, config);
  mutex_lock(&p->coe_gate);
  ret = smp_load_acquire(&p->owner) ? -EBUSY : pmd_coe_config_impl(ndev, config);
  mutex_unlock(&p->coe_gate);
  return ret;
}

int nvether_coe_chan_config(struct net_device* ndev, u32 channel,
                            struct nvether_per_coe_cfg* config) {
  struct ether_priv_data* pdata = netdev_priv(ndev);
  struct mgbe_pmd* p = pdata->pmd;
  int ret;

  if (!p) return pmd_coe_chan_config_impl(ndev, channel, config);
  mutex_lock(&p->coe_gate);
  ret = smp_load_acquire(&p->owner) ? -EBUSY : pmd_coe_chan_config_impl(ndev, channel, config);
  mutex_unlock(&p->coe_gate);
  return ret;
}
