#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Derive an RTX PRO 6000 multi-queue cell from the (TX=2, RX=2) base.

Same pruning rules as scripts/gen_spark_mq_config.py, plus RTX-specific
substitutions for PCIe BDFs, GPU affinities, and the RX-port MAC.
"""

from __future__ import annotations

import argparse
import sys

import yaml

TX_PORT_BASE = 4096


def apply_cpu_layout(cfg: dict, cores: list[int] | None) -> None:
    if not cores or len(cores) < 9:
        return
    cfg["master_core"] = cores[0]
    tx_poll0, tx_work0, tx_poll1, tx_work1 = cores[1], cores[2], cores[3], cores[4]
    rx_poll0, rx_work0, rx_poll1, rx_work1 = cores[5], cores[6], cores[7], cores[8]
    for iface in cfg["interfaces"]:
        if "tx" in iface:
            for q in iface["tx"]["queues"]:
                q["cpu_core"] = tx_poll0 if q["id"] == 0 else tx_poll1
        if "rx" in iface:
            for q in iface["rx"]["queues"]:
                q["cpu_core"] = rx_poll0 if q["id"] == 0 else rx_poll1


def prune(base: dict, tx: int, rx: int, payload: int, batch: int | None,
          eth_dst: str | None, tx_bdf: str | None, rx_bdf: str | None,
          tx_gpu: int, rx_gpu: int,
          tx_gpu2: int | None = None, rx_gpu2: int | None = None,
          cpu_cores: list[int] | None = None) -> dict:
    cfg = base["daqiri"]["cfg"]
    n_ports = max(tx, rx)

    # Second-queue GPU defaults to the first-queue GPU (single-GPU behaviour).
    tx_gpu2 = tx_gpu if tx_gpu2 is None else tx_gpu2
    rx_gpu2 = rx_gpu if rx_gpu2 is None else rx_gpu2

    def keep_region(name: str) -> bool:
        if name.endswith("_1"):
            return tx == 2 if "_TX_" in name else rx == 2
        return True

    cfg["memory_regions"] = [
        m for m in cfg["memory_regions"] if keep_region(m["name"])
    ]
    for m in cfg["memory_regions"]:
        # Queue 1's memory region (name suffix "_1") can live on a different GPU
        # than queue 0's so the two queues drive independent PCIe paths.
        second = m["name"].endswith("_1")
        if "_TX_" in m["name"]:
            m["affinity"] = tx_gpu2 if second else tx_gpu
        elif "_RX_" in m["name"]:
            m["affinity"] = rx_gpu2 if second else rx_gpu

    for iface in cfg["interfaces"]:
        if iface["name"] == "tx_port" and tx_bdf:
            iface["address"] = tx_bdf
        if iface["name"] == "rx_port" and rx_bdf:
            iface["address"] = rx_bdf
        if "tx" in iface:
            iface["tx"]["queues"] = [
                q for q in iface["tx"]["queues"] if q["id"] == 0 or tx == 2
            ]
        if "rx" in iface:
            iface["rx"]["queues"] = [
                q for q in iface["rx"]["queues"] if q["id"] == 0 or rx == 2
            ]
            flows = []
            for f in iface["rx"]["flows"]:
                if f["id"] == 0:
                    flows.append(f)
                elif n_ports == 2:
                    f = dict(f)
                    f["action"] = dict(f["action"])
                    f["action"]["id"] = 1 if rx == 2 else 0
                    flows.append(f)
            iface["rx"]["flows"] = flows

    base["bench_rx"] = [
        b for b in base["bench_rx"] if b["queue_id"] == 0 or rx == 2
    ]

    bench_tx = [b for b in base["bench_tx"] if b["queue_id"] == 0 or tx == 2]
    for b in bench_tx:
        b["payload_size"] = payload
        if batch is not None:
            b["batch_size"] = batch
        if eth_dst is not None:
            b["eth_dst_addr"] = eth_dst
        if b["queue_id"] == 0 and tx == 1 and rx == 2:
            b["udp_src_port"] = f"{TX_PORT_BASE}-{TX_PORT_BASE + 1}"
            b["udp_dst_port"] = f"{TX_PORT_BASE}-{TX_PORT_BASE + 1}"
    base["bench_tx"] = bench_tx

    if cpu_cores and len(cpu_cores) >= 9:
        apply_cpu_layout(cfg, cpu_cores)
        tx_work0, tx_work1 = cpu_cores[2], cpu_cores[4]
        rx_work0, rx_work1 = cpu_cores[6], cpu_cores[8]
        for b in base["bench_rx"]:
            b["cpu_core"] = rx_work0 if b["queue_id"] == 0 else rx_work1
        for b in base["bench_tx"]:
            b["cpu_core"] = tx_work0 if b["queue_id"] == 0 else tx_work1

    return base


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("base", help="path to the (2,2) superset base YAML")
    ap.add_argument("--tx", type=int, choices=(1, 2), required=True)
    ap.add_argument("--rx", type=int, choices=(1, 2), required=True)
    ap.add_argument("--payload", type=int, required=True)
    ap.add_argument("--batch", type=int, default=None)
    ap.add_argument("--eth-dst", default=None)
    ap.add_argument("--tx-bdf", default=None)
    ap.add_argument("--rx-bdf", default=None)
    ap.add_argument("--tx-gpu", type=int, default=0)
    ap.add_argument("--rx-gpu", type=int, default=1)
    ap.add_argument("--tx-gpu2", type=int, default=None,
                    help="GPU ordinal for the 2nd TX queue (default: --tx-gpu)")
    ap.add_argument("--rx-gpu2", type=int, default=None,
                    help="GPU ordinal for the 2nd RX queue (default: --rx-gpu)")
    ap.add_argument("--cpu-cores", default=None,
                    help="comma-separated poll layout: master,TX q0 poll/work,"
                    " TX q1 poll/work, RX q0 poll/work, RX q1 poll/work")
    args = ap.parse_args()

    cpu_cores = None
    if args.cpu_cores:
        cpu_cores = [int(x.strip()) for x in args.cpu_cores.split(",") if x.strip()]

    with open(args.base, encoding="utf-8") as fh:
        base = yaml.safe_load(fh)

    out = prune(
        base,
        args.tx,
        args.rx,
        args.payload,
        args.batch,
        args.eth_dst,
        args.tx_bdf,
        args.rx_bdf,
        args.tx_gpu,
        args.rx_gpu,
        args.tx_gpu2,
        args.rx_gpu2,
        cpu_cores,
    )
    yaml.safe_dump(out, sys.stdout, sort_keys=False, default_flow_style=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
