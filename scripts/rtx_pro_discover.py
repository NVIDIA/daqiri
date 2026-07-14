#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Discover RTX PRO 6000 bench affinity: CUDA ordinals, PIX GPUs per NIC, poll cores.

Reads NIC PCIe BDFs (from env or CLI), matches CUDA-visible GPUs by PCI bus id,
uses ``nvidia-smi topo -m`` PIX links for GPUDirect placement, and picks poll
cores from ``isolcpus`` on the NIC's NUMA node (fallback: low cores on that node).

Emits shell ``export`` lines for ``source`` from discover_rtx_pro_topology.sh.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import re
import subprocess
import sys
from pathlib import Path


def normalize_bdf(bdf: str) -> str:
    bdf = bdf.strip().lower()
    if not bdf:
        return bdf
    # nvidia-smi uses 00000000:bb:dd.f; CUDA uses 0000:bb:dd.f
    m = re.match(r"^(?:0000:)?(?:00000000:)?([0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f])$", bdf)
    if m:
        return f"0000:{m.group(1)}"
    if not bdf.startswith("0000:"):
        bdf = f"0000:{bdf}"
    return bdf


def pci_numa_node(bdf: str) -> int:
    path = Path(f"/sys/bus/pci/devices/{normalize_bdf(bdf)}/numa_node")
    if path.is_file():
        return int(path.read_text().strip())
    return 0


def parse_isolcpu_sets() -> list[set[int]]:
    cmdline = Path("/proc/cmdline").read_text()
    m = re.search(r"isolcpus=[^\s]+", cmdline)
    if not m:
        return []
    spec = m.group(0).split("=", 1)[1]
    cores: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part or part in ("domain", "managed_irq", "nohz", "nohz-daemon"):
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            cores.update(range(int(lo), int(hi) + 1))
        elif part.isdigit():
            cores.add(int(part))
    return [cores] if cores else []


def numa_cpu_list(node: int) -> list[int]:
    path = Path(f"/sys/devices/system/node/node{node}/cpulist")
    if not path.is_file():
        return list(range(128))
    out: list[int] = []
    for part in path.read_text().strip().split(","):
        if "-" in part:
            lo, hi = part.split("-", 1)
            out.extend(range(int(lo), int(hi) + 1))
        elif part:
            out.append(int(part))
    return out


def pick_poll_cores(numa_node: int, count: int = 9) -> list[int]:
    isol = parse_isolcpu_sets()
    candidates = numa_cpu_list(numa_node)
    if isol:
        pool = sorted(c for c in candidates if any(c in s for s in isol))
        if len(pool) >= count:
            return pool[:count]
    # No isolcpus (or too few on this node): use the upper half of the node's
    # core list so we stay away from core 0 / typical IRQ housekeeping.
    if len(candidates) >= count + 4:
        return candidates[4 : 4 + count]
    return candidates[:count]


def load_cuda() -> ctypes.CDLL | None:
    for name in ("libcuda.so.1", "libcuda.so"):
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    return None


def cuda_gpus_pci_order() -> list[tuple[int, str]]:
    cuda = load_cuda()
    if cuda is None:
        return []
    cuda.cuInit(0)
    n = ctypes.c_int()
    if cuda.cuDeviceGetCount(ctypes.byref(n)) != 0:
        return []
    buf = ctypes.create_string_buffer(64)
    gpus: list[tuple[int, str]] = []
    for ordinal in range(n.value):
        dev = ctypes.c_int()
        if cuda.cuDeviceGet(ctypes.byref(dev), ordinal) != 0:
            continue
        if cuda.cuDeviceGetPCIBusId(buf, 64, dev) != 0:
            continue
        bdf = buf.value.decode().strip("\x00").lower()
        gpus.append((ordinal, normalize_bdf(bdf)))
    return gpus


def nvidia_smi_gpus() -> list[tuple[int, str]]:
    try:
        out = subprocess.check_output(
            [
                "nvidia-smi",
                "--query-gpu=index,pci.bus_id",
                "--format=csv,noheader,nounits",
            ],
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    gpus: list[tuple[int, str]] = []
    for line in out.strip().splitlines():
        idx_s, bdf = [p.strip() for p in line.split(",", 1)]
        gpus.append((int(idx_s), normalize_bdf(bdf)))
    return gpus


def mlx5_nic_labels() -> dict[str, str]:
    """Map normalized BDF -> topo label (NIC0, NIC1, ...)."""
    labels: dict[str, str] = {}
    try:
        out = subprocess.check_output(["nvidia-smi", "topo", "-m"], text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return labels
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)
    in_legend = False
    for line in out.splitlines():
        if "NIC Legend" in line:
            in_legend = True
            continue
        if not in_legend:
            continue
        m = re.match(r"\s*(NIC\d+):\s*(\S+)", line)
        if not m:
            continue
        nic_label, mlx = m.group(1), m.group(2)
        dev = Path(f"/sys/class/infiniband/{mlx}/device")
        if dev.is_symlink():
            bdf = normalize_bdf(dev.resolve().name)
            labels[bdf] = nic_label
    return labels


def topo_pix_gpus(nic_label: str) -> list[str]:
    """Return nvidia-smi GPU labels (GPU1, GPU2, ...) with PIX to nic_label."""
    try:
        out = subprocess.check_output(["nvidia-smi", "topo", "-m"], text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)
    lines = [ln for ln in out.splitlines() if ln.strip()]
    if not lines:
        return []
    header = re.split(r"\t+", lines[0].strip())
    try:
        nic_col = header.index(nic_label) + 1  # +1: row label occupies column 0
    except ValueError:
        return []
    pix: list[str] = []
    for line in lines[1:]:
        cols = re.split(r"\t+", line.strip())
        if not cols or not cols[0].startswith("GPU"):
            continue
        if len(cols) <= nic_col:
            continue
        if cols[nic_col].strip() == "PIX":
            pix.append(cols[0].strip())
    return pix


def gpu_label_to_bdf(gpu_label: str) -> str | None:
    # Topo labels GPU1..GPUN match nvidia-smi index 1..N (not 0-based).
    m = re.match(r"GPU(\d+)", gpu_label)
    if not m:
        return None
    smi_idx = int(m.group(1))
    for idx, bdf in nvidia_smi_gpus():
        if idx == smi_idx:
            return bdf
    return None


def ordinal_for_bdf(bdf: str, cuda_gpus: list[tuple[int, str]]) -> int | None:
    bdf = normalize_bdf(bdf)
    for ordinal, bus in cuda_gpus:
        if bus == bdf:
            return ordinal
    return None


def pix_ordinals_for_nic(nic_bdf: str, cuda_gpus: list[tuple[int, str]]) -> list[int]:
    labels = mlx5_nic_labels()
    nic_label = labels.get(normalize_bdf(nic_bdf))
    if not nic_label:
        return []
    ordinals: list[int] = []
    for gpu_label in topo_pix_gpus(nic_label):
        bus = gpu_label_to_bdf(gpu_label)
        if bus is None:
            continue
        ord_ = ordinal_for_bdf(bus, cuda_gpus)
        if ord_ is not None:
            ordinals.append(ord_)
    return ordinals


def pick_gpu_pair(pix: list[int], fallback: int) -> tuple[int, int]:
    if not pix:
        return fallback, fallback
    if len(pix) == 1:
        return pix[0], pix[0]
    return pix[0], pix[1]


def emit_exports(tx_bdf: str, rx_bdf: str) -> int:
    cuda_gpus = cuda_gpus_pci_order()
    if not cuda_gpus:
        print("# WARNING: CUDA not available; GPU ordinals not discovered", file=sys.stderr)
        return 1

    tx_pix = pix_ordinals_for_nic(tx_bdf, cuda_gpus)
    rx_pix = pix_ordinals_for_nic(rx_bdf, cuda_gpus)

    tx_gpu, tx_gpu2 = pick_gpu_pair(tx_pix, cuda_gpus[0][0])
    rx_gpu, rx_gpu2 = pick_gpu_pair(rx_pix, cuda_gpus[min(1, len(cuda_gpus) - 1)][0])

    # Poll on the NUMA node of the TX port (both ports are usually co-located).
    numa = pci_numa_node(tx_bdf)
    cores = pick_poll_cores(numa, 9)
    while len(cores) < 9:
        cores.append(cores[-1] if cores else 0)

    # master, TX q0 poll/work, TX q1 poll/work, RX q0 poll/work, RX q1 poll/work
    layout = {
        "RTX_MASTER_CORE": cores[0],
        "RTX_TX_Q0_POLL": cores[1],
        "RTX_TX_Q0_WORK": cores[2],
        "RTX_TX_Q1_POLL": cores[3],
        "RTX_TX_Q1_WORK": cores[4],
        "RTX_RX_Q0_POLL": cores[5],
        "RTX_RX_Q0_WORK": cores[6],
        "RTX_RX_Q1_POLL": cores[7],
        "RTX_RX_Q1_WORK": cores[8],
    }

    print(f"export RTX_TX_GPU={tx_gpu}")
    print(f"export RTX_RX_GPU={rx_gpu}")
    print(f"export RTX_TX_GPU2={tx_gpu2}")
    print(f"export RTX_RX_GPU2={rx_gpu2}")
    print(f'export RTX_CPU_CORES="{" ".join(str(c) for c in cores)}"')
    for key, val in layout.items():
        print(f"export {key}={val}")

    bus_by_ord = {o: b for o, b in cuda_gpus}
    print(f"# CUDA ordinals (PCI_BUS_ID): TX={tx_gpu}({bus_by_ord.get(tx_gpu,'?')}) "
          f"TX2={tx_gpu2}({bus_by_ord.get(tx_gpu2,'?')}) "
          f"RX={rx_gpu}({bus_by_ord.get(rx_gpu,'?')}) "
          f"RX2={rx_gpu2}({bus_by_ord.get(rx_gpu2,'?')})", file=sys.stderr)
    print(f"# poll cores NUMA {numa}: {' '.join(str(c) for c in cores)}", file=sys.stderr)
    if not tx_pix:
        print(f"# WARNING: no PIX GPU found for TX BDF {tx_bdf}; using fallback ordinal {tx_gpu}", file=sys.stderr)
    if not rx_pix:
        print(f"# WARNING: no PIX GPU found for RX BDF {rx_bdf}; using fallback ordinal {rx_gpu}", file=sys.stderr)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tx-bdf", default=os.environ.get("RTX_TX_BDF", ""))
    ap.add_argument("--rx-bdf", default=os.environ.get("RTX_RX_BDF", ""))
    ap.add_argument("--emit-shell", action="store_true", default=True)
    args = ap.parse_args()
    if not args.tx_bdf or not args.rx_bdf:
        print("ERROR: --tx-bdf and --rx-bdf required", file=sys.stderr)
        return 1
    return emit_exports(args.tx_bdf, args.rx_bdf)


if __name__ == "__main__":
    sys.exit(main())
