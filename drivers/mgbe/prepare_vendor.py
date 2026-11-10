#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Apply the opt-in PMD hooks to a COPY of the pinned Canonical driver.

The generated vendor.patch is self-contained, including the new shim and UAPI.
Any unexpected source context is an error; never patch another driver silently.
"""
import difflib
import pathlib
import re
import subprocess
import sys


def prepare(source: pathlib.Path, module: pathlib.Path):
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if commit != "85b5dd4a7b14c7311c651b4fd9e60515a813da0f":
        raise RuntimeError("unsupported vendor source commit")
    prototype = pathlib.Path(__file__).resolve().parent
    originals = {}

    def edit(path, before, after, count=1):
        p = module / path
        text = p.read_text()
        if text.count(before) != count:
            raise RuntimeError(f"{path}: expected {count} occurrences of {before!r}")
        originals.setdefault(path, text)
        p.write_text(text.replace(before, after))

    def edit_function(path, name, before, after):
        text = (module / path).read_text()
        match = re.search(r"^[^\n;=]*\b" + name + r"\([^;{}]*\)\n\{", text, re.M)
        if not match:
            raise RuntimeError(f"{path}: cannot find function {name}")
        end = text.index("\n}", match.start()) + 2
        body = text[match.start():end]
        if body.count(before) != 1:
            raise RuntimeError(f"{path}: unexpected context in {name}: {before!r}")
        edit(path, body, body.replace(before, after))

    edit("ether_linux.h", "struct ether_priv_data {\n",
         "struct ether_priv_data {\n\tstruct mgbe_pmd *pmd;\n")
    edit("Makefile",
         "$(srctree.nvidia-oot)/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm/include",
         "$(src)/nvethernetrm/include", count=2)
    edit("ether_linux.c", '#include "ether_linux.h"',
         '#include "ether_linux.h"\n#include "mgbe_pmd_internal.h"')
    for fn in ["ether_napi_enable", "ether_napi_disable"]:
        # Keep C declarations before the first statement for kernel style.
        before = f"static void {fn}(struct ether_priv_data *pdata)\n{{"
        start = (module / "ether_linux.c").read_text().index(before)
        text = (module / "ether_linux.c").read_text()
        end = text.index("\n}", start)
        body = text[start:end]
        body2 = body.replace("\n\tfor (i = 0;", "\n\tif (mgbe_pmd_owned(pdata))\n\t\treturn;\n\n\tfor (i = 0;", 1)
        if body == body2:
            raise RuntimeError(f"cannot locate {fn} loop")
        edit("ether_linux.c", body, body2)
    edit_function("ether_linux.c", "free_dma_resources", "\tfree_tx_dma_resources(osi_dma, dev);",
         "\tif (mgbe_pmd_owned(pdata))\n\t\treturn;\n\n\tfree_tx_dma_resources(osi_dma, dev);")
    edit("ether_linux.c", "\tether_init_invalid_chan_ring(osi_dma);",
         "\tif (mgbe_pmd_owned(pdata))\n\t\treturn mgbe_pmd_attach_rings(pdata);\n\n\tether_init_invalid_chan_ring(osi_dma);")
    edit("ether_linux.c", ".ndo_open = ether_open,", ".ndo_open = mgbe_pmd_net_open,")
    edit("ether_linux.c", "\tether_init_rss(pdata, pdata->ndev->features);",
         "\tif (!mgbe_pmd_owned(pdata))\n\t\tether_init_rss(pdata, pdata->ndev->features);")
    # Keep non-hashed broadcast traffic on the exclusive port's fallback queue.
    edit_function("ether_linux.c", "ether_update_mac_addr_filter",
                  "if (osi_dma->num_dma_chans > 1)",
                  "if (osi_dma->num_dma_chans > 1 && !mgbe_pmd_owned(pdata))")
    edit("ether_linux.c", "\tret = ether_ptp_init(pdata);",
         "\tret = mgbe_pmd_owned(pdata) ? 0 : ether_ptp_init(pdata);")
    # Both the open error path and normal close have a PTP teardown.
    edit("ether_linux.c", "\tether_ptp_remove(pdata);",
         "\tif (!mgbe_pmd_owned(pdata))\n\t\tether_ptp_remove(pdata);", count=2)
    # Register after the work items, locks, timers and tasklets exist.
    edit("ether_linux.c", "\n\treturn 0;\n\nerr_sysfs:",
         "\n\tmgbe_pmd_register(pdata);\n\treturn 0;\n\nerr_sysfs:")
    before = "\tstruct device_node *np = dev->of_node;\n\n#ifdef MACSEC_SUPPORT"
    edit("ether_linux.c", before,
         "\tstruct device_node *np = dev->of_node;\n\tbool pmd_sysfs_removed;\n\n\tpmd_sysfs_removed = mgbe_pmd_unregister(pdata);\n\n#ifdef MACSEC_SUPPORT")
    edit_function("ether_linux.c", "ether_remove", "\tether_sysfs_unregister(pdata);",
                  "\tif (!pmd_sysfs_removed)\n\t\tether_sysfs_unregister(pdata);")
    edit_function("ether_linux.c", "ether_shutdown", "\tif (!netif_running(ndev))",
                  "\tif (mgbe_pmd_owned(pdata)) {\n\t\tmgbe_pmd_platform_shutdown(pdata);\n\t\treturn;\n\t}\n\n\tif (!netif_running(ndev))")
    # Suspend would invalidate userspace mappings behind its back. Reject it
    # while a session owns the port, including a configured-but-stopped session.
    text = (module / "ether_linux.c").read_text()
    for fn in ["ether_suspend_noirq", "ether_resume_noirq"]:
        marker = f"static int {fn}(struct device *dev)"
        if marker not in text:
            marker = f"int {fn}(struct device *dev)"
        start = text.index(marker)
        end = text.index("\n}", start)
        body = text[start:end]
        # Insert before the first existing early-return branch.
        pos = body.index("\n\tif (")
        changed = body[:pos] + "\n\tif (mgbe_pmd_owned(pdata))\n\t\treturn -EBUSY;\n" + body[pos:]
        edit("ether_linux.c", body, changed)
        text = (module / "ether_linux.c").read_text()
    # CoE callers use exported functions independently of the netdev ops.
    for fn in ["nvether_coe_config", "nvether_coe_chan_config"]:
        text = (module / "ether_linux.c").read_text()
        start = text.index(f"int {fn}(")
        end = text.index("\n}", start)
        body = text[start:end]
        pos = body.index("\n\tif (")
        edit("ether_linux.c", body,
             body[:pos] + "\n\tif (mgbe_pmd_owned(pdata))\n\t\treturn -EBUSY;\n" + body[pos:])
        edit("ether_linux.c", "int " + fn + "(",
             "static int pmd_" + fn.removeprefix("nvether_") + "_impl(")
    # The link callback can execute in IRQ context: it masks/stops DMA and marks
    # the session faulted; userspace must close/reopen rather than auto-recover.
    edit("osd.c", '#include "ether_linux.h"',
         '#include "ether_linux.h"\n#include "mgbe_pmd_internal.h"')
    text = (module / "osd.c").read_text()
    start = text.index("void osd_restart_lane_bringup(")
    end = text.index("\n}", start)
    body = text[start:end]
    pos = body.index("\n\tpdata->tx_start_stop")
    edit("osd.c", body,
         body[:pos] + "\n\tif (mgbe_pmd_link_event(pdata, en_disable))\n\t\treturn;\n" + body[pos:])
    edit("nvethernetrm/include/osi_dma.h", "struct osi_dma_priv_data {\n",
         "struct osi_dma_priv_data {\n\t/* Exclusive userspace polling: initialize but do not arm DMA/IRQs. */\n\tnveu32_t exclusive_polling;\n")
    edit("nvethernetrm/osi/dma/osi_dma.c",
         "\n\tret = intr_fn[OSI_DMA_INTR_ENABLE](osi_dma, VIRT_INTR_CHX_CNTRL(chan),",
         "\n\tif (osi_dma->exclusive_polling)\n\t\treturn 0;\n\n\tret = intr_fn[OSI_DMA_INTR_ENABLE](osi_dma, VIRT_INTR_CHX_CNTRL(chan),", count=2)
    edit("nvethernetrm/osi/dma/osi_dma.c",
         "val |= (DMA_CHX_INTR_TIE | DMA_CHX_INTR_RIE);",
         "if (osi_dma->exclusive_polling)\n\t\tval = 0;\n\telse\n\t\tval |= (DMA_CHX_INTR_TIE | DMA_CHX_INTR_RIE);")
    # The two replacements above belong to the TX/RX interrupt enables in the
    # same init function; only the first is reached for the PMD path.
    for filename in ["mgbe_pmd_internal.h", "mgbe_pmd_kernel.c"]:
        originals[filename] = ""
        (module / filename).write_text((prototype / "kernel" / filename).read_text())
    originals["mgbe_pmd_uapi.h"] = ""
    (module / "mgbe_pmd_uapi.h").write_text((prototype / "include/mgbe_pmd_uapi.h").read_text())
    with (module / "ether_linux.c").open("a") as f:
        f.write('\n/* Experimental PMD control shim, opt-in per port. */\n#include "mgbe_pmd_kernel.c"\n')
    patch = []
    for path, original in originals.items():
        relative = path if path.startswith("nvethernetrm/") else "nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/" + path
        patch.extend(difflib.unified_diff(original.splitlines(True),
                     (module / path).read_text().splitlines(True),
                     fromfile="a/" + relative if original else "/dev/null",
                     tofile="b/" + relative))
    (module.parent / "vendor.patch").write_text("".join(patch))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare_vendor.py SOURCE_ROOT MODULE_COPY")
    prepare(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
