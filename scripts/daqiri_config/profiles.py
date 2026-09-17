# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Reusable high-level DAQIRI configuration profiles."""

from __future__ import annotations

import ipaddress
import re
from dataclasses import dataclass
from typing import Any

from .core import ConfigError


SUPPORTED_TRANSFORMS = ("none", "vlan", "vxlan", "gre", "nvgre")
SUPPORTED_SOCKET_TRANSPORTS = ("udp", "tcp", "roce")
MAC_ADDRESS_RE = re.compile(r"^(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$")
ETHERNET_HEADER_SIZE = 14
UDP_IPV4_HEADER_SIZE = 42
IPV4_MAX_TOTAL_LENGTH = 65535


def _require_positive(name: str, value: int) -> None:
    if value <= 0:
        raise ConfigError(f"{name} must be greater than zero")


def _require_port(name: str, value: int) -> None:
    if value <= 0 or value > 65535:
        raise ConfigError(f"{name} must be in the range 1..65535")


def _require_ip(name: str, value: str) -> None:
    try:
        address = ipaddress.ip_address(value)
    except ValueError as exc:
        raise ConfigError(f"{name} must be an IPv4 address") from exc
    if address.version != 4:
        raise ConfigError(f"{name} must be an IPv4 address")


def _require_mac(name: str, value: str) -> None:
    if MAC_ADDRESS_RE.fullmatch(value) is None:
        raise ConfigError(f"{name} must be a six-octet MAC address")


def _endpoint(transport: str, address: str, port: int | None) -> str:
    return f"{transport}://{address}" + (f":{port}" if port is not None else "")


@dataclass(frozen=True)
class SocketPairSpec:
    """A unidirectional client/TX to server/RX socket or RoCE pair."""

    transport: str
    client_address: str
    server_address: str
    client_port: int
    server_port: int
    client_master_core: int
    server_master_core: int
    client_rx_core: int
    client_tx_core: int
    server_rx_core: int
    server_tx_core: int
    client_worker_core: int | None
    server_worker_core: int | None
    message_size: int
    buffer_size: int
    num_bufs: int
    rx_num_bufs: int | None = None
    tx_num_bufs: int | None = None
    rx_batch_size: int | None = None
    affinity: int = 0
    memory_kind: str | None = None
    iterations: int | None = None
    rx_depth: int | None = None
    tx_depth: int | None = None
    roce_transport_mode: str | None = None
    include_benchmark: bool = True

    def __post_init__(self) -> None:
        if self.transport not in SUPPORTED_SOCKET_TRANSPORTS:
            raise ConfigError(
                f"transport must be one of {', '.join(SUPPORTED_SOCKET_TRANSPORTS)}"
            )
        if self.transport != "roce" and (
            self.rx_num_bufs is not None or self.tx_num_bufs is not None
        ):
            raise ConfigError("rx_num_bufs and tx_num_bufs are supported only for RoCE")
        if self.transport == "roce" and self.rx_batch_size is not None:
            raise ConfigError("rx_batch_size is supported only for TCP/UDP")
        if self.transport == "roce" and self.iterations is not None:
            raise ConfigError("iterations is supported only for TCP/UDP")
        if self.transport != "roce" and (
            self.rx_depth is not None
            or self.tx_depth is not None
            or self.roce_transport_mode is not None
        ):
            raise ConfigError(
                "rx_depth, tx_depth, and roce_transport_mode are supported only for RoCE"
            )
        for name in ("client_port", "server_port"):
            _require_port(name, getattr(self, name))
        for name in ("client_address", "server_address"):
            _require_ip(name, getattr(self, name))
        for name in (
            "client_master_core",
            "server_master_core",
            "client_rx_core",
            "client_tx_core",
            "server_rx_core",
            "server_tx_core",
        ):
            if getattr(self, name) < -1:
                raise ConfigError(f"{name} must be -1 or a non-negative CPU index")
        for name in ("client_worker_core", "server_worker_core"):
            value = getattr(self, name)
            if value is not None and value < -1:
                raise ConfigError(f"{name} must be -1 or a non-negative CPU index")
        if self.include_benchmark and (
            self.client_worker_core is None or self.server_worker_core is None
        ):
            raise ConfigError("benchmark profiles require client and server worker cores")
        if self.affinity < 0:
            raise ConfigError("affinity must not be negative")
        for name in (
            "message_size",
            "buffer_size",
            "num_bufs",
        ):
            _require_positive(name, getattr(self, name))
        for name in ("rx_batch_size", "iterations", "rx_depth", "tx_depth"):
            value = getattr(self, name)
            if value is not None:
                _require_positive(name, value)
        if self.rx_num_bufs is not None:
            _require_positive("rx_num_bufs", self.rx_num_bufs)
        if self.tx_num_bufs is not None:
            _require_positive("tx_num_bufs", self.tx_num_bufs)
        if self.buffer_size < self.message_size:
            raise ConfigError("buffer_size must be at least message_size")
        if self.transport == "udp" and self.message_size > 65507:
            raise ConfigError("UDP message_size must not exceed 65507 bytes")
        rx_batch_size = self.rx_batch_size if self.rx_batch_size is not None else 1
        if self.transport == "udp" and rx_batch_size > 32:
            raise ConfigError("UDP rx_batch_size must not exceed 32")
        if self.transport in ("udp", "tcp") and rx_batch_size > self.num_bufs:
            raise ConfigError("rx_batch_size must not exceed num_bufs")
        if self.transport in ("udp", "tcp") and self.memory_kind == "device":
            raise ConfigError("TCP/UDP socket profiles cannot use device memory")
        if self.transport == "roce" and self.roce_transport_mode not in (
            None,
            "RC",
            "UC",
            "UD",
        ):
            raise ConfigError("roce_transport_mode must be RC, UC, or UD")
        if self.transport == "roce":
            queue_cores = (
                self.client_master_core,
                self.server_master_core,
                self.client_rx_core,
                self.client_tx_core,
                self.server_rx_core,
                self.server_tx_core,
            )
            if any(core < 0 for core in queue_cores):
                raise ConfigError("RoCE master and queue cores must be non-negative")
            rx_depth = self.rx_depth if self.rx_depth is not None else 128
            tx_depth = self.tx_depth if self.tx_depth is not None else 128
            if rx_depth > (self.rx_num_bufs or self.num_bufs):
                raise ConfigError("rx_depth must not exceed RX memory-region num_bufs")
            if tx_depth > (self.tx_num_bufs or self.num_bufs):
                raise ConfigError("tx_depth must not exceed TX memory-region num_bufs")


def _socket_memory_regions(spec: SocketPairSpec, role: str) -> list[dict[str, Any]]:
    suffix = "CLIENT" if role == "tx" else "SERVER"
    memory_kind = spec.memory_kind or ("host_pinned" if spec.transport == "roce" else "host")
    if spec.transport == "roce":
        return [
            {
                "name": f"DATA_RX_GPU_{suffix}",
                "kind": memory_kind,
                "affinity": spec.affinity,
                "num_bufs": spec.rx_num_bufs or spec.num_bufs,
                "buf_size": spec.buffer_size,
            },
            {
                "name": f"DATA_TX_GPU_{suffix}",
                "kind": memory_kind,
                "affinity": spec.affinity,
                "num_bufs": spec.tx_num_bufs or spec.num_bufs,
                "buf_size": spec.buffer_size,
            },
        ]
    return [
        {
            "name": f"DATA_SOCKET_{suffix}",
            "kind": memory_kind,
            "affinity": spec.affinity,
            "num_bufs": spec.num_bufs,
            "buf_size": spec.buffer_size,
        }
    ]


def _socket_role_document(spec: SocketPairSpec, role: str) -> dict[str, Any]:
    is_client = role == "tx"
    mode = "client" if is_client else "server"
    suffix = mode.upper()
    local_address = spec.client_address if is_client else spec.server_address
    local_port = spec.client_port if is_client else spec.server_port
    remote_address = spec.server_address if is_client else spec.client_address
    remote_port = spec.server_port if is_client else spec.client_port
    master_core = spec.client_master_core if is_client else spec.server_master_core
    rx_core = spec.client_rx_core if is_client else spec.server_rx_core
    tx_core = spec.client_tx_core if is_client else spec.server_tx_core
    worker_core = spec.client_worker_core if is_client else spec.server_worker_core
    memory_regions = _socket_memory_regions(spec, role)

    socket_config: dict[str, Any] = {
        "mode": mode,
        "local_addr": _endpoint(
            spec.transport,
            local_address,
            None if spec.transport == "roce" and is_client else local_port,
        ),
    }
    if spec.transport != "roce":
        socket_config["remote_addr"] = _endpoint(
            spec.transport, remote_address, remote_port
        )
        socket_config["max_payload_size"] = min(65535, spec.buffer_size)

    interface: dict[str, Any] = {
        "name": f"{spec.transport}_{mode}",
        "address": local_address,
        "socket_config": socket_config,
    }
    if spec.transport == "roce":
        interface["roce_config"] = {"transport_mode": spec.roce_transport_mode or "RC"}
        interface["rx"] = {
            "queues": [
                {
                    "name": f"{suffix}_RX_Queue",
                    "id": 0,
                    "cpu_core": rx_core,
                    "batch_size": 1,
                }
            ]
        }
        interface["tx"] = {
            "queues": [
                {
                    "name": f"{suffix}_TX_Queue",
                    "id": 0,
                    "cpu_core": tx_core,
                    "batch_size": 1,
                }
            ]
        }
    else:
        region_name = memory_regions[0]["name"]
        interface["rx"] = {
            "queues": [
                {
                    "name": f"{suffix}_RX_Queue",
                    "id": 0,
                    "cpu_core": rx_core,
                    "batch_size": (spec.rx_batch_size or 1) if not is_client else 1,
                    "memory_regions": [region_name],
                }
            ]
        }
        interface["tx"] = {
            "queues": [
                {
                    "name": f"{suffix}_TX_Queue",
                    "id": 0,
                    "cpu_core": tx_core,
                    "batch_size": 1,
                    "memory_regions": [region_name],
                }
            ]
        }

    document: dict[str, Any] = {
        "daqiri": {
            "cfg": {
                "version": 1,
                "stream_type": "socket",
                "master_core": master_core,
                "debug": False,
                "log_level": "info",
                "memory_regions": memory_regions,
                "interfaces": [interface],
            }
        }
    }

    if spec.include_benchmark:
        bench: dict[str, Any] = {
            "cpu_core": worker_core,
            "server": not is_client,
            "send": is_client,
            "receive": not is_client,
            "message_size": spec.message_size,
            "server_address": spec.server_address,
            "server_port": spec.server_port,
        }
        if spec.transport == "roce":
            bench["rx_depth"] = spec.rx_depth if spec.rx_depth is not None else 128
            bench["tx_depth"] = spec.tx_depth if spec.tx_depth is not None else 128
            if is_client:
                bench["client_address"] = spec.client_address
            document[f"rdma_bench_{mode}"] = bench
        else:
            bench["iterations"] = (
                spec.iterations if spec.iterations is not None else 1_000_000_000
            )
            if is_client:
                bench["client_address"] = spec.client_address
            document[f"socket_bench_{mode}"] = bench

    return document


def generate_socket_pair(spec: SocketPairSpec) -> dict[str, dict[str, Any]]:
    """Generate concrete TX/client and RX/server documents."""

    return {
        "tx": _socket_role_document(spec, "tx"),
        "rx": _socket_role_document(spec, "rx"),
    }


@dataclass(frozen=True)
class RawPairSpec:
    """A raw-Ethernet TX/RX pair, optionally multi-queue or transformed."""

    tx_address: str
    rx_address: str
    master_core: int
    tx_queue_cores: tuple[int, ...]
    rx_queue_cores: tuple[int, ...]
    tx_worker_cores: tuple[int, ...]
    rx_worker_cores: tuple[int, ...]
    eth_dst_addr: str
    ip_src_addr: str = "1.1.1.1"
    ip_dst_addr: str = "2.2.2.2"
    udp_port_base: int = 4096
    payload_size: int = 8000
    header_size: int = 64
    batch_size: int = 10240
    num_bufs: int = 51200
    affinity: int = 0
    memory_kind: str = "device"
    engine: str | None = None
    transform: str = "none"
    vlan_id: int = 100
    outer_eth_src: str = "02:00:00:00:00:01"
    outer_eth_dst: str = "02:00:00:00:00:02"
    outer_ipv4_src: str = "192.0.2.1"
    outer_ipv4_dst: str = "192.0.2.2"
    tunnel_id: int = 100
    include_benchmark: bool = True

    def __post_init__(self) -> None:
        if not self.tx_queue_cores or not self.rx_queue_cores:
            raise ConfigError("raw-pair requires at least one TX and one RX queue")
        if self.include_benchmark and len(self.tx_worker_cores) != len(self.tx_queue_cores):
            raise ConfigError("one tx_worker_core is required per TX queue")
        if self.include_benchmark and len(self.rx_worker_cores) != len(self.rx_queue_cores):
            raise ConfigError("one rx_worker_core is required per RX queue")
        if not self.include_benchmark and self.tx_worker_cores and (
            len(self.tx_worker_cores) != len(self.tx_queue_cores)
        ):
            raise ConfigError("tx_worker_cores must be empty or match the TX queue count")
        if not self.include_benchmark and self.rx_worker_cores and (
            len(self.rx_worker_cores) != len(self.rx_queue_cores)
        ):
            raise ConfigError("rx_worker_cores must be empty or match the RX queue count")
        if len(self.tx_queue_cores) > 1 and len(self.rx_queue_cores) > 1:
            if len(self.tx_queue_cores) != len(self.rx_queue_cores):
                raise ConfigError(
                    "raw-pair supports unequal queue counts only when one side has one queue"
                )
        if self.transform not in SUPPORTED_TRANSFORMS:
            raise ConfigError(
                f"transform must be one of {', '.join(SUPPORTED_TRANSFORMS)}"
            )
        if self.transform != "none" and (
            len(self.tx_queue_cores) != 1 or len(self.rx_queue_cores) != 1
        ):
            raise ConfigError(
                "raw transform profiles require exactly one TX and one RX queue"
            )
        if self.transform != "none" and self.engine is None:
            raise ConfigError(
                "raw transform profiles require an explicit dpdk or ibverbs engine"
            )
        if self.engine not in (None, "dpdk", "ibverbs"):
            raise ConfigError("raw-pair engine must be dpdk, ibverbs, or omitted")
        if self.memory_kind not in ("huge", "device", "host_pinned", "host"):
            raise ConfigError("unsupported memory_kind")
        for name in ("payload_size", "header_size", "batch_size", "num_bufs"):
            _require_positive(name, getattr(self, name))
        if self.num_bufs < self.batch_size:
            raise ConfigError("num_bufs must be at least batch_size")
        if self.engine in (None, "dpdk") and self.num_bufs < 2 * self.batch_size:
            raise ConfigError("DPDK raw profiles require num_bufs to be at least twice batch_size")
        if self.include_benchmark:
            if self.header_size < UDP_IPV4_HEADER_SIZE:
                raise ConfigError(
                    f"benchmark raw header_size must be at least {UDP_IPV4_HEADER_SIZE} bytes"
                )
            if (
                self.header_size + self.payload_size - ETHERNET_HEADER_SIZE
                > IPV4_MAX_TOTAL_LENGTH
            ):
                raise ConfigError(
                    "benchmark raw header_size + payload_size exceeds the IPv4 "
                    "total-length limit"
                )
        for name in ("master_core", "tx_queue_cores", "rx_queue_cores"):
            values = getattr(self, name)
            values = values if isinstance(values, tuple) else (values,)
            if any(value < 0 for value in values):
                raise ConfigError(f"{name} must contain non-negative CPU indices")
        for name in ("tx_worker_cores", "rx_worker_cores"):
            if any(value < -1 for value in getattr(self, name)):
                raise ConfigError(f"{name} must contain -1 or non-negative CPU indices")
        if self.affinity < 0:
            raise ConfigError("affinity must not be negative")
        _require_mac("eth_dst_addr", self.eth_dst_addr)
        _require_ip("ip_src_addr", self.ip_src_addr)
        _require_ip("ip_dst_addr", self.ip_dst_addr)
        if self.transform in ("vxlan", "gre", "nvgre"):
            _require_mac("outer_eth_src", self.outer_eth_src)
            _require_mac("outer_eth_dst", self.outer_eth_dst)
            _require_ip("outer_ipv4_src", self.outer_ipv4_src)
            _require_ip("outer_ipv4_dst", self.outer_ipv4_dst)
        if not 0 <= self.vlan_id <= 4095:
            raise ConfigError("vlan_id must be in the range 0..4095")
        if not 0 <= self.tunnel_id <= 0xFFFFFF:
            raise ConfigError("tunnel_id must be in the range 0..16777215")
        _require_port("udp_port_base", self.udp_port_base)
        flow_count = max(len(self.tx_queue_cores), len(self.rx_queue_cores))
        if self.udp_port_base + flow_count - 1 > 65535:
            raise ConfigError("raw-pair UDP port range exceeds 65535")


def _raw_action(transform: str, direction: str, spec: RawPairSpec) -> dict[str, Any]:
    if transform == "vlan":
        if direction == "tx":
            return {
                "type": "vlan_push",
                "vlan_id": spec.vlan_id,
                "pcp": 0,
                "dei": 0,
                "ethertype": 0x8100,
            }
        return {"type": "vlan_pop"}

    tunnel: dict[str, Any] = {
        "type": transform,
        "outer_eth_src": spec.outer_eth_src,
        "outer_eth_dst": spec.outer_eth_dst,
        "outer_ipv4_src": spec.outer_ipv4_src,
        "outer_ipv4_dst": spec.outer_ipv4_dst,
    }
    if transform == "vxlan":
        tunnel.update(
            {
                "outer_udp_src": 49152,
                "outer_udp_dst": 4789,
                "vni": spec.tunnel_id,
            }
        )
    elif transform == "gre":
        tunnel["gre_protocol"] = 0x0800
    elif transform == "nvgre":
        tunnel.update({"tni": spec.tunnel_id, "flow_id": 0})
    return {
        "type": "tunnel_encap" if direction == "tx" else "tunnel_decap",
        "tunnel": tunnel,
    }


def _raw_transform_rx_match(spec: RawPairSpec) -> dict[str, Any]:
    """Return the backend-appropriate match surrounding an RX transform."""

    # DPDK derives the outer tunnel/VLAN pattern from the decap action and
    # treats the explicit match as an optional inner-packet match.  Keep the
    # map present so the C++ YAML decoder does not discard the RX section.
    if spec.engine == "dpdk":
        return {}
    if spec.transform == "vlan":
        return {"udp_src": spec.udp_port_base, "udp_dst": spec.udp_port_base}
    if spec.transform == "vxlan":
        return {"udp_src": 49152, "udp_dst": 4789}
    return {
        "ipv4_src": spec.outer_ipv4_src,
        "ipv4_dst": spec.outer_ipv4_dst,
    }


def _raw_regions(prefix: str, count: int, spec: RawPairSpec) -> list[dict[str, Any]]:
    numbered = count > 1
    return [
        {
            "name": f"Data_{prefix}_GPU" + (f"_{index}" if numbered else ""),
            "kind": spec.memory_kind,
            "affinity": spec.affinity,
            "num_bufs": spec.num_bufs,
            "buf_size": spec.payload_size + spec.header_size,
        }
        for index in range(count)
    ]


def generate_raw_pair(spec: RawPairSpec) -> dict[str, Any]:
    """Generate one concrete raw-Ethernet benchmark or production document."""

    tx_count = len(spec.tx_queue_cores)
    rx_count = len(spec.rx_queue_cores)
    flow_count = max(tx_count, rx_count)
    tx_regions = _raw_regions("TX", tx_count, spec)
    rx_regions = _raw_regions("RX", rx_count, spec)

    tx_queues = []
    for index, core in enumerate(spec.tx_queue_cores):
        tx_queues.append(
            {
                "name": f"tx_q_{index}",
                "id": index,
                "batch_size": spec.batch_size,
                "cpu_core": core,
                "memory_regions": [tx_regions[index]["name"]],
                "offloads": ["tx_eth_src"],
            }
        )

    rx_queues = []
    for index, core in enumerate(spec.rx_queue_cores):
        rx_queues.append(
            {
                "name": f"rx_q_{index}",
                "id": index,
                "cpu_core": core,
                "batch_size": spec.batch_size,
                "memory_regions": [rx_regions[index]["name"]],
            }
        )

    rx_flows = []
    for index in range(flow_count):
        flow: dict[str, Any] = {
            "name": f"flow_{index}",
            # The ibverbs raw engine reserves zero as an invalid flow ID. Keep
            # generated profiles portable across both raw engines.
            "id": index + 1,
            "action": {"type": "queue", "id": index % rx_count},
            "match": {
                "udp_src": spec.udp_port_base + index,
                "udp_dst": spec.udp_port_base + index,
            },
        }
        if spec.transform != "none":
            flow["name"] = f"{spec.transform}_decap"
            flow["id"] = 100 + SUPPORTED_TRANSFORMS.index(spec.transform)
            flow.pop("action")
            flow["match"] = _raw_transform_rx_match(spec)
            flow["actions"] = [
                _raw_action(spec.transform, "rx", spec),
                {"type": "queue", "id": index % rx_count},
            ]
        rx_flows.append(flow)

    tx: dict[str, Any] = {"queues": tx_queues}
    if spec.transform != "none":
        tx["flows"] = [
            {
                "name": f"{spec.transform}_encap",
                "id": 100 + SUPPORTED_TRANSFORMS.index(spec.transform),
                "actions": [_raw_action(spec.transform, "tx", spec)],
                "match": {
                    "udp_src": spec.udp_port_base,
                    "udp_dst": spec.udp_port_base,
                },
            }
        ]

    config: dict[str, Any] = {
        "version": 1,
        "stream_type": "raw",
    }
    if spec.engine is not None:
        config["engine"] = spec.engine
    config.update({
        "master_core": spec.master_core,
        "debug": False,
        "log_level": "info",
        "loopback": "",
        "memory_regions": [*tx_regions, *rx_regions],
        "interfaces": [
            {"name": "tx_port", "address": spec.tx_address, "tx": tx},
            {
                "name": "rx_port",
                "address": spec.rx_address,
                "rx": {
                    "flow_isolation": True,
                    "queues": rx_queues,
                    "flows": rx_flows,
                },
            },
        ],
    })

    document: dict[str, Any] = {"daqiri": {"cfg": config}}
    if spec.include_benchmark:
        document["bench_rx"] = [
            {
                "interface_name": "rx_port",
                "queue_id": index,
                "cpu_core": spec.rx_worker_cores[index],
            }
            for index in range(rx_count)
        ]
        bench_tx = []
        for index in range(tx_count):
            first_port = spec.udp_port_base + index
            if tx_count == 1 and flow_count > 1:
                port: int | str = f"{spec.udp_port_base}-{spec.udp_port_base + flow_count - 1}"
            else:
                port = first_port
            bench_tx.append(
                {
                    "interface_name": "tx_port",
                    "queue_id": index,
                    "cpu_core": spec.tx_worker_cores[index],
                    "batch_size": spec.batch_size,
                    "payload_size": spec.payload_size,
                    "header_size": spec.header_size,
                    "eth_dst_addr": spec.eth_dst_addr,
                    "ip_src_addr": spec.ip_src_addr,
                    "ip_dst_addr": spec.ip_dst_addr,
                    "udp_src_port": port,
                    "udp_dst_port": port,
                }
            )
        document["bench_tx"] = bench_tx

    return document


def generate_raw_roles(spec: RawPairSpec) -> dict[str, dict[str, Any]]:
    """Split a raw pair into independently runnable TX-only and RX-only documents."""

    combined = generate_raw_pair(spec)
    config = combined["daqiri"]["cfg"]
    tx_interface, rx_interface = config["interfaces"]
    tx_region_names = {
        name
        for queue in tx_interface["tx"]["queues"]
        for name in queue.get("memory_regions", [])
    }
    rx_region_names = {
        name
        for queue in rx_interface["rx"]["queues"]
        for name in queue.get("memory_regions", [])
    }

    def role_document(
        interface: dict[str, Any], region_names: set[str], bench_key: str
    ) -> dict[str, Any]:
        role_config = {
            key: value
            for key, value in config.items()
            if key not in ("memory_regions", "interfaces")
        }
        role_config["memory_regions"] = [
            region for region in config["memory_regions"] if region["name"] in region_names
        ]
        role_config["interfaces"] = [interface]
        document: dict[str, Any] = {"daqiri": {"cfg": role_config}}
        if spec.include_benchmark:
            document[bench_key] = combined[bench_key]
        return document

    return {
        "tx": role_document(tx_interface, tx_region_names, "bench_tx"),
        "rx": role_document(rx_interface, rx_region_names, "bench_rx"),
    }
