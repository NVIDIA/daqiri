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

import yaml


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULTS_PATH = REPO_ROOT / "examples" / "cabled_loopback_hardware.yaml"


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


def load_defaults(path: Path) -> dict[str, dict[str, Any]]:
    try:
        document = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise ResolveError(f"cannot load hardware defaults {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("version") != 1:
        raise ResolveError("hardware defaults must declare version: 1")
    platforms = document.get("platforms")
    if not isinstance(platforms, dict) or not platforms:
        raise ResolveError("hardware defaults must contain a non-empty platforms mapping")
    required = {
        "workload_gpu",
        "expected_link_mbps",
        "isolated_cpus",
        "preferred_tx_bdf",
        "preferred_rx_bdf",
        "tx_phys_port",
        "rx_phys_port",
        "dpdk_memory_kind",
        "dpdk_payload_batches",
        "dpdk_payload_pacing_gbps",
        "ibverbs_memory_kind",
        "ibverbs_batch_size",
        "ibverbs_num_bufs",
        "ibverbs_payload_batches",
        "ibverbs_payload_pacing_gbps",
        "rdma_memory_kind",
        "master_core",
        "dpdk_tx_queue_core",
        "dpdk_rx_queue_core",
        "dpdk_tx_worker_core",
        "dpdk_rx_worker_core",
        "rdma_client_rx_core",
        "rdma_client_tx_core",
        "rdma_server_rx_core",
        "rdma_server_tx_core",
        "socket_server_cores",
        "socket_client_cores",
        "socket_pair_counts",
        "socket_headline_pairs",
    }
    for name, values in platforms.items():
        if not isinstance(values, dict):
            raise ResolveError(f"platform {name!r} must be a mapping")
        missing = sorted(required - values.keys())
        if missing:
            raise ResolveError(f"platform {name!r} is missing: {', '.join(missing)}")
        for key in ("workload_gpu",):
            try:
                re.compile(str(values[key]))
            except re.error as exc:
                raise ResolveError(f"platform {name!r} has invalid {key}: {exc}") from exc
        server_cores = values["socket_server_cores"]
        client_cores = values["socket_client_cores"]
        if not isinstance(server_cores, list) or not server_cores:
            raise ResolveError(f"platform {name!r} must define socket_server_cores")
        if not isinstance(client_cores, list) or len(client_cores) != len(server_cores):
            raise ResolveError(
                f"platform {name!r} must define equally sized socket core lists"
            )
        pair_counts = values["socket_pair_counts"]
        headline_pairs = values["socket_headline_pairs"]
        if not isinstance(pair_counts, list) or not pair_counts:
            raise ResolveError(f"platform {name!r} must define socket_pair_counts")
        if not isinstance(headline_pairs, list) or not headline_pairs:
            raise ResolveError(f"platform {name!r} must define socket_headline_pairs")
        if max(pair_counts + headline_pairs) > len(server_cores):
            raise ResolveError(
                f"platform {name!r} requests more socket pairs than available core pairs"
            )
    return platforms


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


def render_shell(values: dict[str, Any]) -> str:
    return "\n".join(f"{key}={shlex.quote(str(value))}" for key, value in sorted(values.items()))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--defaults", type=Path, default=DEFAULTS_PATH)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--skip-nic", action="store_true")
    parser.add_argument("--client-namespace")
    parser.add_argument("--server-namespace")
    parser.add_argument("--format", choices=("json", "shell"), default="json")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        platforms = load_defaults(args.defaults)
        if bool(args.client_namespace) != bool(args.server_namespace):
            raise ResolveError("client and server namespaces must be specified together")
        if args.skip_nic:
            ports = []
        elif args.client_namespace:
            ports = discover_namespace_ports(
                (args.client_namespace, args.server_namespace)
            )
        else:
            ports = discover_ports()
        values = resolve(
            platforms,
            args.platform,
            discover_gpus(),
            ports,
            args.skip_nic,
        )
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
