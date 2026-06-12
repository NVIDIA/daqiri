#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""Derive a DGX Spark multi-queue core-scaling cell from the (TX=2, RX=2) base.

run_spark_mq_bench.sh sweeps the matrix cells (TX,RX) = (1,1),(1,2),(2,1),(2,2).
Rather than check in four near-identical YAMLs, we keep one superset base --
examples/daqiri_bench_raw_tx_rx_spark_mq.yaml -- and prune it down to each cell
here. The pruning mirrors exactly what the four hand-written configs used to be:

  * memory regions / TX queues / RX queues / bench entries: keep index 0, and
    keep index 1 only when that side has 2 queues.
  * flows: keep flow_0; keep flow_1 when there are two distinct UDP ports on the
    wire, i.e. when EITHER side has 2 queues (max(tx, rx) == 2). flow_1 routes to
    RX queue 1 when RX has 2 queues, else to RX queue 0 (the 2t1r case, where two
    TX flows both land on the lone RX queue).
  * bench_tx UDP ports: a single TX worker feeding two RX queues (1t2r) round-
    robins the port range "4096-4097"; otherwise queue N uses port 4096+N.

PyYAML round-trips structure (not comments); the bench's yaml-cpp parser only
reads parsed content, so dropping comments/key-order is harmless. Output goes to
stdout.
"""

from __future__ import annotations

import argparse
import sys

import yaml

TX_PORT_BASE = 4096


def prune(base: dict, tx: int, rx: int, payload: int, batch: int | None,
          eth_dst: str | None) -> dict:
    cfg = base["daqiri"]["cfg"]
    n_ports = max(tx, rx)

    # Memory regions: keep <side>_GPU_0 always, <side>_GPU_1 per that side's count.
    def keep_region(name: str) -> bool:
        if name.endswith("_1"):
            return tx == 2 if "_TX_" in name else rx == 2
        return True

    cfg["memory_regions"] = [m for m in cfg["memory_regions"]
                             if keep_region(m["name"])]

    for iface in cfg["interfaces"]:
        if "tx" in iface:
            iface["tx"]["queues"] = [q for q in iface["tx"]["queues"]
                                     if q["id"] == 0 or tx == 2]
        if "rx" in iface:
            iface["rx"]["queues"] = [q for q in iface["rx"]["queues"]
                                     if q["id"] == 0 or rx == 2]
            flows = []
            for f in iface["rx"]["flows"]:
                if f["id"] == 0:
                    flows.append(f)
                elif n_ports == 2:
                    f["action"]["id"] = 1 if rx == 2 else 0
                    flows.append(f)
            iface["rx"]["flows"] = flows

    base["bench_rx"] = [b for b in base["bench_rx"]
                        if b["queue_id"] == 0 or rx == 2]

    bench_tx = [b for b in base["bench_tx"] if b["queue_id"] == 0 or tx == 2]
    for b in bench_tx:
        b["payload_size"] = payload
        if batch is not None:
            b["batch_size"] = batch
        if eth_dst is not None:
            b["eth_dst_addr"] = eth_dst
        if b["queue_id"] == 0 and tx == 1 and rx == 2:
            # Single TX worker round-robins both flows' ports.
            b["udp_src_port"] = f"{TX_PORT_BASE}-{TX_PORT_BASE + 1}"
            b["udp_dst_port"] = f"{TX_PORT_BASE}-{TX_PORT_BASE + 1}"
    base["bench_tx"] = bench_tx

    return base


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("base", help="path to the (2,2) superset base YAML")
    ap.add_argument("--tx", type=int, choices=(1, 2), required=True)
    ap.add_argument("--rx", type=int, choices=(1, 2), required=True)
    ap.add_argument("--payload", type=int, required=True)
    ap.add_argument("--batch", type=int, default=None)
    ap.add_argument("--eth-dst", default=None,
                    help="fill bench_tx eth_dst_addr (the rx_port MAC)")
    args = ap.parse_args()

    with open(args.base, encoding="utf-8") as fh:
        base = yaml.safe_load(fh)

    out = prune(base, args.tx, args.rx, args.payload, args.batch, args.eth_dst)
    yaml.safe_dump(out, sys.stdout, sort_keys=False, default_flow_style=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
