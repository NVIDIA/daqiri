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

This is the structural inverse of unioning the old _netns_server / _netns_client
files. run_spark_bench.sh pipes the output through its existing per-message-size
awk/sed rewrites (num_bufs/buf_size/depths for RDMA; ports/cores for sockets),
so this script intentionally does NOT touch those fields. Output goes to stdout.
"""

from __future__ import annotations

import argparse
import sys

import yaml


def split_role(base: dict, role: str) -> dict:
    cfg = base["daqiri"]["cfg"]
    cfg["interfaces"] = [
        i for i in cfg["interfaces"]
        if i.get("socket_config", {}).get("mode") == role
    ]
    if not cfg["interfaces"]:
        raise SystemExit(f"no interface with socket_config.mode == {role!r}")
    cfg["memory_regions"] = [
        m for m in cfg["memory_regions"] if role.upper() in m["name"]
    ]

    other = "client" if role == "server" else "server"
    for key in [k for k in base if k.endswith(f"_bench_{other}")]:
        del base[key]
    if not any(k.endswith(f"_bench_{role}") for k in base):
        raise SystemExit(f"base has no *_bench_{role} section")
    return base


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("base", help="path to the combined (both-role) netns base YAML")
    ap.add_argument("--role", choices=("server", "client"), required=True)
    args = ap.parse_args()

    with open(args.base, encoding="utf-8") as fh:
        base = yaml.safe_load(fh)

    out = split_role(base, args.role)
    yaml.safe_dump(out, sys.stdout, sort_keys=False, default_flow_style=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
