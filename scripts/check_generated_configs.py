#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Exercise the generated configuration matrix and optionally the C++ decoder."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from daqiri_config import (
    RawPairSpec,
    SocketPairSpec,
    generate_raw_pair,
    generate_raw_roles,
    generate_socket_pair,
    render_document,
    validate_document,
)


def _socket_spec(transport: str) -> SocketPairSpec:
    message_size = {"udp": 8000, "tcp": 1_048_576, "roce": 8_000_000}[transport]
    return SocketPairSpec(
        transport=transport,
        client_address="10.250.0.1",
        server_address="10.250.0.2",
        client_port=5002,
        server_port=5001,
        client_master_core=8,
        server_master_core=8,
        client_rx_core=18,
        client_tx_core=17,
        server_rx_core=19,
        server_tx_core=16,
        client_worker_core=18,
        server_worker_core=19,
        message_size=message_size,
        buffer_size=message_size if transport != "udp" else 65536,
        num_bufs=128,
        rx_num_bufs=512,
        tx_num_bufs=128,
        rx_batch_size=32 if transport == "udp" else 1,
        memory_kind="host_pinned" if transport == "roce" else "host",
        rx_depth=512,
        tx_depth=128,
    )


def _raw_spec(**overrides: Any) -> RawPairSpec:
    values: dict[str, Any] = {
        "tx_address": "0005:03:00.0",
        "rx_address": "0005:03:00.1",
        "master_core": 3,
        "tx_queue_cores": (4,),
        "rx_queue_cores": (5,),
        "tx_worker_cores": (6,),
        "rx_worker_cores": (7,),
        "eth_dst_addr": "48:b0:2d:f4:04:24",
        "engine": "ibverbs",
    }
    values.update(overrides)
    return RawPairSpec(**values)


def generated_matrix() -> dict[str, dict[str, Any]]:
    documents: dict[str, dict[str, Any]] = {}
    for transport in ("udp", "tcp", "roce"):
        for role, document in generate_socket_pair(_socket_spec(transport)).items():
            documents[f"socket-{transport}-{role}"] = document

    for engine in ("dpdk", "ibverbs"):
        for transform in ("none", "vlan", "vxlan", "gre", "nvgre"):
            documents[f"raw-{engine}-{transform}"] = generate_raw_pair(
                _raw_spec(engine=engine, transform=transform)
            )

    for tx_count, rx_count in ((1, 1), (1, 2), (2, 1), (2, 2)):
        documents[f"raw-mq-{tx_count}x{rx_count}"] = generate_raw_pair(
            _raw_spec(
                engine="dpdk",
                memory_kind="host_pinned",
                tx_queue_cores=tuple(range(10, 10 + tx_count)),
                rx_queue_cores=tuple(range(20, 20 + rx_count)),
                tx_worker_cores=tuple(range(30, 30 + tx_count)),
                rx_worker_cores=tuple(range(40, 40 + rx_count)),
            )
        )

    for role, document in generate_raw_roles(_raw_spec()).items():
        documents[f"raw-xhost-{role}"] = document
    return documents


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--validator",
        type=Path,
        help="also parse every document with the built daqiri_config_validate binary",
    )
    args = parser.parse_args()

    documents = generated_matrix()
    with tempfile.TemporaryDirectory(prefix="daqiri-generated-configs-") as temp_dir:
        paths = []
        for name, document in documents.items():
            validate_document(document)
            rendered = render_document(document)
            if rendered != render_document(document):
                raise RuntimeError(f"non-deterministic rendering for {name}")
            path = Path(temp_dir) / f"{name}.yaml"
            path.write_text(rendered, encoding="utf-8")
            paths.append(path)

        if args.validator:
            subprocess.run(
                [str(args.validator), *(str(path) for path in paths)], check=True
            )

    suffix = " and the C++ decoder" if args.validator else ""
    print(f"Validated {len(documents)} generated configurations with JSON Schema{suffix}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
