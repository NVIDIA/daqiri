#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Generate deterministic production and benchmark DAQIRI configurations."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# CMake replaces this token in the installed launcher with either a path
# relative to CMAKE_INSTALL_BINDIR or an absolute GNUInstallDirs data path.
# In the source tree the unresolved token is ignored and Python finds the
# sibling daqiri_config package normally.
CONFIGURED_MODULE_HINT = "@DAQIRI_CONFIG_GENERATOR_MODULE_HINT@"
UNCONFIGURED_MODULE_HINT = "@" + "DAQIRI_CONFIG_GENERATOR_MODULE_HINT" + "@"
if CONFIGURED_MODULE_HINT != UNCONFIGURED_MODULE_HINT:
    installed_modules = Path(CONFIGURED_MODULE_HINT)
    if not installed_modules.is_absolute():
        installed_modules = Path(__file__).resolve().parent / installed_modules
    sys.path.insert(0, str(installed_modules.resolve()))

from daqiri_config import (
    ConfigError,
    RawPairSpec,
    SocketPairSpec,
    apply_overrides,
    generate_raw_pair,
    generate_raw_roles,
    generate_socket_pair,
    load_document,
    render_document,
)


def _cores(value: str) -> tuple[int, ...]:
    try:
        cores = tuple(int(item) for item in value.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a comma-separated CPU list") from exc
    if not cores:
        raise argparse.ArgumentTypeError("CPU list must not be empty")
    return cores


def _add_output_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("-o", "--output", help="write one generated document to this path")


def _write(text: str, output: str | None) -> None:
    if output:
        Path(output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


def _socket_parser(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser(
        "socket-pair",
        help="generate a unidirectional TCP, UDP, or RoCE TX/RX pair",
    )
    parser.add_argument("--transport", choices=("udp", "tcp", "roce"), required=True)
    parser.add_argument("--client-address", required=True, help="TX/client IP address")
    parser.add_argument("--server-address", required=True, help="RX/server IP address")
    parser.add_argument("--client-port", type=int, required=True)
    parser.add_argument("--server-port", type=int, required=True)
    parser.add_argument("--client-master-core", type=int, required=True)
    parser.add_argument("--server-master-core", type=int, required=True)
    parser.add_argument("--client-rx-core", type=int, required=True)
    parser.add_argument("--client-tx-core", type=int, required=True)
    parser.add_argument("--server-rx-core", type=int, required=True)
    parser.add_argument("--server-tx-core", type=int, required=True)
    parser.add_argument("--client-worker-core", type=int)
    parser.add_argument("--server-worker-core", type=int)
    parser.add_argument("--message-size", type=int, required=True)
    parser.add_argument("--buffer-size", type=int, required=True)
    parser.add_argument("--num-bufs", type=int, required=True)
    parser.add_argument(
        "--rx-num-bufs", type=int, help="RoCE RX memory-region buffer count"
    )
    parser.add_argument(
        "--tx-num-bufs", type=int, help="RoCE TX memory-region buffer count"
    )
    parser.add_argument(
        "--rx-batch-size", type=int, help="TCP/UDP RX batch size (default: 1)"
    )
    parser.add_argument("--affinity", type=int, default=0)
    parser.add_argument("--memory-kind", choices=("huge", "device", "host_pinned", "host"))
    parser.add_argument(
        "--iterations", type=int, help="TCP/UDP benchmark iterations (default: 1000000000)"
    )
    parser.add_argument("--rx-depth", type=int, help="RoCE RX depth (default: 128)")
    parser.add_argument("--tx-depth", type=int, help="RoCE TX depth (default: 128)")
    parser.add_argument(
        "--roce-transport-mode",
        choices=("RC", "UC", "UD"),
        help="RoCE transport mode (default: RC)",
    )
    parser.add_argument(
        "--role",
        choices=("tx", "rx", "both"),
        default="both",
        help="emit one role to stdout/output, or both roles to --output-dir",
    )
    parser.add_argument("--output-dir", help="directory for tx.yaml and rx.yaml")
    parser.add_argument(
        "--daqiri-only",
        action="store_true",
        help="omit benchmark-owned top-level sections",
    )
    _add_output_argument(parser)


def _raw_parser(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser(
        "raw-pair", help="generate a raw-Ethernet TX/RX configuration"
    )
    parser.add_argument("--tx-address", required=True, help="TX PCI BDF or interface")
    parser.add_argument("--rx-address", required=True, help="RX PCI BDF or interface")
    parser.add_argument("--master-core", type=int, required=True)
    parser.add_argument("--tx-queue-cores", type=_cores, required=True)
    parser.add_argument("--rx-queue-cores", type=_cores, required=True)
    parser.add_argument("--tx-worker-cores", type=_cores, default=())
    parser.add_argument("--rx-worker-cores", type=_cores, default=())
    parser.add_argument("--eth-dst-addr", required=True)
    parser.add_argument("--ip-src-addr", default="1.1.1.1")
    parser.add_argument("--ip-dst-addr", default="2.2.2.2")
    parser.add_argument("--udp-port-base", type=int, default=4096)
    parser.add_argument("--payload-size", type=int, default=8000)
    parser.add_argument("--header-size", type=int, default=64)
    parser.add_argument(
        "--buffer-size",
        "--buf-size",
        type=int,
        help="packet-buffer capacity (default: header size + payload size)",
    )
    parser.add_argument("--batch-size", type=int, default=10240)
    parser.add_argument("--num-bufs", type=int, default=51200)
    parser.add_argument("--affinity", type=int, default=0)
    parser.add_argument(
        "--memory-kind",
        choices=("huge", "device", "host_pinned", "host"),
        default="device",
    )
    parser.add_argument("--engine", choices=("dpdk", "ibverbs"))
    parser.add_argument(
        "--tx-eth-src",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable NIC insertion of the Ethernet source address (default: enabled)",
    )
    parser.add_argument(
        "--transform", choices=("none", "vlan", "vxlan", "gre", "nvgre"), default="none"
    )
    parser.add_argument("--vlan-id", type=int, default=100)
    parser.add_argument("--outer-eth-src", default="02:00:00:00:00:01")
    parser.add_argument("--outer-eth-dst", default="02:00:00:00:00:02")
    parser.add_argument("--outer-ipv4-src", default="192.0.2.1")
    parser.add_argument("--outer-ipv4-dst", default="192.0.2.2")
    parser.add_argument("--tunnel-id", type=int, default=100)
    parser.add_argument(
        "--role",
        choices=("loopback", "tx", "rx", "both"),
        default="loopback",
        help="emit a combined loopback document, one role, or a TX/RX pair",
    )
    parser.add_argument("--output-dir", help="directory for tx.yaml and rx.yaml")
    parser.add_argument(
        "--daqiri-only",
        action="store_true",
        help="omit benchmark-owned top-level sections",
    )
    _add_output_argument(parser)


def _render_parser(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser(
        "render", help="deterministically render an arbitrary DAQIRI document"
    )
    parser.add_argument("input", help="YAML document or bare daqiri.cfg mapping")
    parser.add_argument(
        "--set",
        dest="overrides",
        action="append",
        default=[],
        metavar="/JSON/POINTER=VALUE",
        help="replace an existing value before rendering; repeat as needed",
    )
    _add_output_argument(parser)


def _socket_spec(args: argparse.Namespace) -> SocketPairSpec:
    return SocketPairSpec(
        transport=args.transport,
        client_address=args.client_address,
        server_address=args.server_address,
        client_port=args.client_port,
        server_port=args.server_port,
        client_master_core=args.client_master_core,
        server_master_core=args.server_master_core,
        client_rx_core=args.client_rx_core,
        client_tx_core=args.client_tx_core,
        server_rx_core=args.server_rx_core,
        server_tx_core=args.server_tx_core,
        client_worker_core=args.client_worker_core,
        server_worker_core=args.server_worker_core,
        message_size=args.message_size,
        buffer_size=args.buffer_size,
        num_bufs=args.num_bufs,
        rx_num_bufs=args.rx_num_bufs,
        tx_num_bufs=args.tx_num_bufs,
        rx_batch_size=args.rx_batch_size,
        affinity=args.affinity,
        memory_kind=args.memory_kind,
        iterations=args.iterations,
        rx_depth=args.rx_depth,
        tx_depth=args.tx_depth,
        roce_transport_mode=args.roce_transport_mode,
        include_benchmark=not args.daqiri_only,
    )


def _raw_spec(args: argparse.Namespace) -> RawPairSpec:
    return RawPairSpec(
        tx_address=args.tx_address,
        rx_address=args.rx_address,
        master_core=args.master_core,
        tx_queue_cores=args.tx_queue_cores,
        rx_queue_cores=args.rx_queue_cores,
        tx_worker_cores=args.tx_worker_cores,
        rx_worker_cores=args.rx_worker_cores,
        eth_dst_addr=args.eth_dst_addr,
        ip_src_addr=args.ip_src_addr,
        ip_dst_addr=args.ip_dst_addr,
        udp_port_base=args.udp_port_base,
        payload_size=args.payload_size,
        header_size=args.header_size,
        buffer_size=args.buffer_size,
        batch_size=args.batch_size,
        num_bufs=args.num_bufs,
        affinity=args.affinity,
        memory_kind=args.memory_kind,
        engine=args.engine,
        tx_eth_src=args.tx_eth_src,
        transform=args.transform,
        vlan_id=args.vlan_id,
        outer_eth_src=args.outer_eth_src,
        outer_eth_dst=args.outer_eth_dst,
        outer_ipv4_src=args.outer_ipv4_src,
        outer_ipv4_dst=args.outer_ipv4_dst,
        tunnel_id=args.tunnel_id,
        include_benchmark=not args.daqiri_only,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    _render_parser(subparsers)
    _socket_parser(subparsers)
    _raw_parser(subparsers)
    args = parser.parse_args(argv)

    try:
        if args.command == "render":
            document = apply_overrides(load_document(args.input), args.overrides)
            _write(render_document(document), args.output)
            return 0

        if args.command == "raw-pair":
            spec = _raw_spec(args)
            if args.role == "loopback":
                if args.output_dir:
                    parser.error("raw-pair --role loopback does not use --output-dir")
                _write(render_document(generate_raw_pair(spec)), args.output)
                return 0
            documents = generate_raw_roles(spec)
            if args.role == "both":
                if args.output or not args.output_dir:
                    parser.error(
                        "raw-pair --role both requires --output-dir and does not use --output"
                    )
                output_dir = Path(args.output_dir)
                output_dir.mkdir(parents=True, exist_ok=True)
                for role, document in documents.items():
                    (output_dir / f"{role}.yaml").write_text(
                        render_document(document), encoding="utf-8"
                    )
            else:
                if args.output_dir:
                    parser.error("--output-dir is only valid with --role both")
                _write(render_document(documents[args.role]), args.output)
            return 0

        documents = generate_socket_pair(_socket_spec(args))
        if args.role == "both":
            if args.output or not args.output_dir:
                parser.error(
                    "socket-pair --role both requires --output-dir and does not use --output"
                )
            output_dir = Path(args.output_dir)
            output_dir.mkdir(parents=True, exist_ok=True)
            for role, document in documents.items():
                (output_dir / f"{role}.yaml").write_text(
                    render_document(document), encoding="utf-8"
                )
        else:
            if args.output_dir:
                parser.error("--output-dir is only valid with --role both")
            _write(render_document(documents[args.role]), args.output)
        return 0
    except ConfigError as exc:
        parser.exit(2, f"error: {exc}\n")


if __name__ == "__main__":
    sys.exit(main())
