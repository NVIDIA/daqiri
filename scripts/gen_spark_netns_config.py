#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""Split a combined DGX Spark netns bench base into a single-role config.

The socket/RDMA wire-loopback sweep runs the server and client in separate
network namespaces, so each process needs a config that names only its own
interface (a combined two-interface config would make a process init the other
namespace's IP, which it does not own). Rather than check in a server YAML and a
client YAML per protocol, we keep one combined base -- both interfaces, both
memory regions, both bench sections -- and split it to a role here:

  * interfaces: keep the one whose socket_config.mode == role.
  * memory regions: keep those whose name contains the role (SERVER / CLIENT).
  * bench sections: drop the other role's <prefix>_bench_<role> mapping.
  * optional queue overrides: update RX batch size plus RX queue, TX queue, and
    benchmark-worker affinity without text substitutions that conflate them.

This is the structural inverse of unioning the old _netns_server / _netns_client
files. run_spark_bench.sh supplies optional structured queue overrides, then
pipes the output through per-message-size awk/sed rewrites
(num_bufs/buf_size/depths for RDMA; payload and ports for sockets). Output goes
to stdout.
"""

from __future__ import annotations

import argparse
import sys

import yaml


def split_role(
    base: dict,
    role: str,
    rx_queue_cpu_core: int | None = None,
    rx_queue_batch_size: int | None = None,
    tx_queue_cpu_core: int | None = None,
    bench_cpu_core: int | None = None,
) -> dict:
    cfg = base["daqiri"]["cfg"]
    cfg["interfaces"] = [
        i for i in cfg["interfaces"]
        if i.get("socket_config", {}).get("mode") == role
    ]
    if not cfg["interfaces"]:
        raise SystemExit(f"no interface with socket_config.mode == {role!r}")
    if rx_queue_cpu_core is not None:
        for interface in cfg["interfaces"]:
            for queue in interface.get("rx", {}).get("queues", []):
                queue["cpu_core"] = rx_queue_cpu_core
    if rx_queue_batch_size is not None:
        for interface in cfg["interfaces"]:
            for queue in interface.get("rx", {}).get("queues", []):
                queue["batch_size"] = rx_queue_batch_size
    if tx_queue_cpu_core is not None:
        for interface in cfg["interfaces"]:
            for queue in interface.get("tx", {}).get("queues", []):
                queue["cpu_core"] = tx_queue_cpu_core
    cfg["memory_regions"] = [
        m for m in cfg["memory_regions"] if role.upper() in m["name"]
    ]

    other = "client" if role == "server" else "server"
    for key in [k for k in base if k.endswith(f"_bench_{other}")]:
        del base[key]
    role_bench_keys = [k for k in base if k.endswith(f"_bench_{role}")]
    if not role_bench_keys:
        raise SystemExit(f"base has no *_bench_{role} section")
    if bench_cpu_core is not None:
        for key in role_bench_keys:
            base[key]["cpu_core"] = bench_cpu_core
    return base


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("base", help="path to the combined (both-role) netns base YAML")
    ap.add_argument("--role", choices=("server", "client"), required=True)
    ap.add_argument("--rx-queue-cpu-core", type=int)
    ap.add_argument("--rx-queue-batch-size", type=int)
    ap.add_argument("--tx-queue-cpu-core", type=int)
    ap.add_argument("--bench-cpu-core", type=int)
    args = ap.parse_args()

    with open(args.base, encoding="utf-8") as fh:
        base = yaml.safe_load(fh)

    out = split_role(
        base,
        args.role,
        rx_queue_cpu_core=args.rx_queue_cpu_core,
        rx_queue_batch_size=args.rx_queue_batch_size,
        tx_queue_cpu_core=args.tx_queue_cpu_core,
        bench_cpu_core=args.bench_cpu_core,
    )
    yaml.safe_dump(out, sys.stdout, sort_keys=False, default_flow_style=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
