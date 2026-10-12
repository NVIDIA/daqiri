#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Resolve cabled-loopback benchmark defaults against live hardware."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

if __package__:
    from .daqiri_config import ConfigError, load_document, render_document
else:
    from daqiri_config import ConfigError, load_document, render_document


REPO_ROOT = Path(__file__).resolve().parents[1]
RAW_TEMPLATE = REPO_ROOT / "examples" / "daqiri_bench_raw_tx_rx.yaml"

# Placement and pacing settings used by the published Spark/Thor benchmark runs.
_COMMON_DEFAULTS = {
    "tx_phys_port": "p0",
    "rx_phys_port": "p1",
    "ibverbs_batch_size": 4096,
    "ibverbs_num_bufs": 8192,
    "rdma_memory_kind": "host_pinned",
}

PLATFORM_DEFAULTS = {
    "dgx-spark": {
        **_COMMON_DEFAULTS,
        "workload_gpu": "^NVIDIA GB10$",
        "expected_link_mbps": 100000,
        "isolated_cpus": "16-19",
        "preferred_tx_bdf": "0000:01:00.0",
        "preferred_rx_bdf": "0002:01:00.1",
        "dpdk_memory_kind": "host_pinned",
        "dpdk_payload_batches": [],
        "dpdk_payload_pacing_gbps": [],
        "ibverbs_memory_kind": "host_pinned",
        "ibverbs_payload_batches": [],
        "ibverbs_payload_pacing_gbps": [],
        "master_core": 8,
        "dpdk_tx_queue_core": 17,
        "dpdk_rx_queue_core": 18,
        "dpdk_tx_worker_core": 16,
        "dpdk_rx_worker_core": 19,
        "rdma_client_rx_core": 18,
        "rdma_client_tx_core": 17,
        "rdma_server_rx_core": 19,
        "rdma_server_tx_core": 16,
        "socket_server_cores": [16, 18, 5, 7],
        "socket_client_cores": [17, 19, 6, 9],
        "socket_pair_counts": [1, 2, 4],
        "socket_headline_pairs": [4],
    },
    "igx-thor": {
        **_COMMON_DEFAULTS,
        "workload_gpu": "^NVIDIA RTX PRO 6000 Blackwell.*$",
        "expected_link_mbps": 200000,
        "isolated_cpus": "8-13",
        "preferred_tx_bdf": "0004:03:00.0",
        "preferred_rx_bdf": "0004:03:00.1",
        "dpdk_memory_kind": "device",
        "dpdk_payload_batches": ["256:1024", "64:1024"],
        "dpdk_payload_pacing_gbps": ["256:50", "64:20"],
        "ibverbs_memory_kind": "device",
        "ibverbs_payload_batches": ["1024:512", "256:256", "64:512"],
        "ibverbs_payload_pacing_gbps": ["256:125", "64:65"],
        "master_core": 5,
        "dpdk_tx_queue_core": 9,
        "dpdk_rx_queue_core": 10,
        "dpdk_tx_worker_core": 8,
        "dpdk_rx_worker_core": 11,
        "rdma_client_rx_core": 10,
        "rdma_client_tx_core": 11,
        "rdma_server_rx_core": 9,
        "rdma_server_tx_core": 8,
        "socket_server_cores": [8, 10, 12],
        "socket_client_cores": [9, 11, 13],
        "socket_pair_counts": [1, 2, 3],
        "socket_headline_pairs": [3],
    },
}


class ResolveError(ValueError):
    pass


@dataclass(frozen=True)
class Gpu:
    name: str
    uuid: str


@dataclass(frozen=True)
class Port:
    netdev: str
    bdf: str
    phys_port: str
    mac: str
    carrier: bool


def _run(*args: str) -> str:
    result = subprocess.run(args, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise ResolveError(f"{' '.join(args)} failed: {detail}")
    return result.stdout


def discover_gpus() -> list[Gpu]:
    output = _run(
        "nvidia-smi",
        "--query-gpu=name,uuid",
        "--format=csv,noheader,nounits",
    )
    gpus = []
    for line in output.splitlines():
        if not line.strip():
            continue
        try:
            name, uuid = (part.strip() for part in line.rsplit(",", 1))
        except ValueError as exc:
            raise ResolveError(f"unexpected nvidia-smi row: {line!r}") from exc
        gpus.append(Gpu(name=name, uuid=uuid))
    return gpus


def discover_ports(sysfs_root: Path = Path("/sys")) -> list[Port]:
    ports = []
    for netdev_path in sorted((sysfs_root / "class" / "net").glob("*")):
        device_path = netdev_path / "device"
        infiniband_path = device_path / "infiniband"
        if not device_path.exists() or not infiniband_path.exists():
            continue
        resolved = device_path.resolve()
        phys_port_path = netdev_path / "phys_port_name"
        address_path = netdev_path / "address"
        carrier_path = netdev_path / "carrier"
        try:
            phys_port = phys_port_path.read_text(encoding="utf-8").strip()
            mac = address_path.read_text(encoding="utf-8").strip()
            carrier = carrier_path.read_text(encoding="utf-8").strip() == "1"
        except OSError:
            continue
        ports.append(
            Port(
                netdev=netdev_path.name,
                bdf=resolved.name,
                phys_port=phys_port,
                mac=mac,
                carrier=carrier,
            )
        )
    return ports


def discover_namespace_ports(namespaces: tuple[str, str]) -> list[Port]:
    ports = []
    for namespace in namespaces:
        netdevs = _run("ip", "netns", "exec", namespace, "ls", "/sys/class/net")
        for netdev in sorted(netdevs.split()):
            if netdev == "lo":
                continue
            base = f"/sys/class/net/{netdev}"
            bdf = Path(
                _run("ip", "netns", "exec", namespace, "readlink", "-f", f"{base}/device").strip()
            ).name
            if not bdf:
                continue
            try:
                phys_port = _run(
                    "ip", "netns", "exec", namespace, "cat", f"{base}/phys_port_name"
                ).strip()
                mac = _run(
                    "ip", "netns", "exec", namespace, "cat", f"{base}/address"
                ).strip()
                carrier = (
                    _run("ip", "netns", "exec", namespace, "cat", f"{base}/carrier").strip()
                    == "1"
                )
            except ResolveError:
                continue
            ports.append(
                Port(
                    netdev=netdev,
                    bdf=bdf,
                    phys_port=phys_port,
                    mac=mac,
                    carrier=carrier,
                )
            )
    return ports


def select_platform(requested: str, platforms: dict[str, dict[str, Any]]) -> str:
    if requested not in platforms:
        raise ResolveError(f"unknown platform {requested!r}")
    return requested


def select_gpu(values: dict[str, Any], gpus: list[Gpu], override: str | None) -> Gpu:
    if override:
        matches = [gpu for gpu in gpus if gpu.uuid == override]
    else:
        matches = [gpu for gpu in gpus if re.search(str(values["workload_gpu"]), gpu.name)]
    if len(matches) != 1:
        label = override or values["workload_gpu"]
        raise ResolveError(f"GPU selector {label!r} matched {len(matches)} devices")
    return matches[0]


def select_port(
    ports: list[Port], phys_port: str, preferred_bdf: str, override_bdf: str | None
) -> Port:
    candidates = [port for port in ports if port.carrier and port.phys_port == phys_port]
    requested_bdf = override_bdf or preferred_bdf
    preferred = [port for port in candidates if port.bdf == requested_bdf]
    if preferred:
        return preferred[0]
    if override_bdf:
        raise ResolveError(
            f"requested BDF {override_bdf} is not a carrier-up RDMA netdev on {phys_port}"
        )
    if not candidates:
        raise ResolveError(f"no carrier-up RDMA netdev found for physical port {phys_port}")
    return sorted(candidates, key=lambda port: port.bdf)[0]


def resolve(
    platforms: dict[str, dict[str, Any]],
    requested_platform: str,
    gpus: list[Gpu],
    ports: list[Port],
    skip_nic: bool,
) -> dict[str, Any]:
    platform = select_platform(requested_platform, platforms)
    values = platforms[platform]
    gpu = select_gpu(values, gpus, os.environ.get("DAQIRI_GPU_UUID"))
    result: dict[str, Any] = {
        "BENCH_PLATFORM": platform,
        "DEFAULT_GPU_UUID": gpu.uuid,
        "EXPECTED_LINK_MBPS": int(values["expected_link_mbps"]),
        "DEFAULT_ISOLATED_CPUS": str(values["isolated_cpus"]),
    }
    for key, value in values.items():
        if key in {
            "workload_gpu",
            "expected_link_mbps",
            "isolated_cpus",
            "preferred_tx_bdf",
            "preferred_rx_bdf",
            "tx_phys_port",
            "rx_phys_port",
        }:
            continue
        output_key = f"DEFAULT_{key.upper()}"
        result[output_key] = (
            " ".join(str(item) for item in value) if isinstance(value, list) else value
        )
    if not skip_nic:
        tx_port = select_port(
            ports,
            str(values["tx_phys_port"]),
            str(values["preferred_tx_bdf"]),
            os.environ.get("DPDK_TX_PCI"),
        )
        rx_port = select_port(
            ports,
            str(values["rx_phys_port"]),
            str(values["preferred_rx_bdf"]),
            os.environ.get("DPDK_RX_PCI"),
        )
        if tx_port.phys_port == rx_port.phys_port or tx_port.netdev == rx_port.netdev:
            raise ResolveError("TX and RX must resolve to distinct physical ports")
        result.update(
            {
                "DEFAULT_DPDK_TX_PCI": tx_port.bdf,
                "DEFAULT_DPDK_RX_PCI": rx_port.bdf,
                "DEFAULT_DPDK_TX_NETDEV": tx_port.netdev,
                "DEFAULT_DPDK_RX_NETDEV": rx_port.netdev,
                "DEFAULT_ETH_SRC_ADDR": tx_port.mac,
                "DEFAULT_ETH_DST_ADDR": rx_port.mac,
            }
        )
    return result


def fill_raw_template(path: Path, values: dict[str, Any], engine: str) -> None:
    """Fill hardware fields in an existing raw TX/RX template, retaining its workload."""
    try:
        document = load_document(path)
        cfg = document["daqiri"]["cfg"]
        if cfg["stream_type"] != "raw":
            raise ResolveError("--write-config requires a raw TX/RX template")
        interfaces = {interface["name"]: interface for interface in cfg["interfaces"]}
        regions = {region["name"]: region for region in cfg["memory_regions"]}
        cfg["engine"] = engine
        cfg["master_core"] = values["DEFAULT_MASTER_CORE"]
        for direction in ("tx", "rx"):
            interface = interfaces[f"{direction}_port"]
            interface["address"] = values[f"DEFAULT_DPDK_{direction.upper()}_PCI"]
            queues = interface[direction]["queues"]
            if len(queues) != 1 or queues[0].get("poll_mode", "indirect") != "indirect":
                raise ResolveError("--write-config expects one indirect TX and RX queue")
            queue = queues[0]
            queue["cpu_core"] = values[f"DEFAULT_DPDK_{direction.upper()}_QUEUE_CORE"]
            if engine == "ibverbs":
                queue["batch_size"] = values["DEFAULT_IBVERBS_BATCH_SIZE"]
            for name in queue["memory_regions"]:
                regions[name]["kind"] = values[f"DEFAULT_{engine.upper()}_MEMORY_KIND"]
            for bench in document[f"bench_{direction}"]:
                if bench["interface_name"] == interface["name"]:
                    bench["cpu_core"] = values[f"DEFAULT_DPDK_{direction.upper()}_WORKER_CORE"]
                    if direction == "tx":
                        bench["eth_dst_addr"] = values["DEFAULT_ETH_DST_ADDR"]
                        if engine == "ibverbs":
                            bench["batch_size"] = queue["batch_size"]
                        for key, address in (("ip_src_addr", "192.0.2.1"),
                                             ("ip_dst_addr", "192.0.2.2")):
                            if str(bench.get(key, "")).startswith("<"):
                                bench[key] = address
            # Raw ibverbs requires nonzero interface-local flow IDs.
            used_ids = {flow["id"] for flow in interface.get("rx", {}).get("flows", [])}
            for flow in interface.get("rx", {}).get("flows", []):
                if flow["id"] == 0 and engine == "ibverbs":
                    flow["id"] = max(used_ids) + 1
                    used_ids.add(flow["id"])
        rendered = render_document(document)
        path.write_text(rendered, encoding="utf-8")
    except (OSError, ConfigError, KeyError, TypeError) as exc:
        raise ResolveError(f"cannot fill raw TX/RX template {path}: {exc}") from exc


def render_shell(values: dict[str, Any]) -> str:
    return "\n".join(f"{key}={shlex.quote(str(value))}" for key, value in sorted(values.items()))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write-config", nargs="?", const=RAW_TEMPLATE, type=Path,
        help="fill the existing raw TX/RX template in place (optionally specify its path)",
    )
    parser.add_argument("--engine", choices=("dpdk", "ibverbs"), default="dpdk",
                        help="engine for --write-config (default: dpdk)")
    parser.add_argument("--platform", required=True)
    parser.add_argument("--skip-nic", action="store_true")
    parser.add_argument("--client-namespace")
    parser.add_argument("--server-namespace")
    parser.add_argument("--format", choices=("json", "shell"), default="json")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if bool(args.client_namespace) != bool(args.server_namespace):
            raise ResolveError("client and server namespaces must be specified together")
        if args.write_config and args.skip_nic:
            raise ResolveError("--write-config requires NIC discovery")
        if args.skip_nic:
            ports = []
        elif args.client_namespace:
            ports = discover_namespace_ports(
                (args.client_namespace, args.server_namespace)
            )
        else:
            ports = discover_ports()
        values = resolve(
            PLATFORM_DEFAULTS,
            args.platform,
            discover_gpus(),
            ports,
            args.skip_nic,
        )
        if args.write_config:
            fill_raw_template(args.write_config, values, args.engine)
            print(f"Updated {args.write_config}", file=sys.stderr)
    except ResolveError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    if args.format == "shell":
        print(render_shell(values))
    else:
        print(json.dumps(values, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
