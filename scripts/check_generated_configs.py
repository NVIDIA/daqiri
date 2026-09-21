#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Exercise the generated configuration matrix with the C++ runtime decoder."""

from __future__ import annotations

import argparse
import copy
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import yaml

from daqiri_config import (
    RawPairSpec,
    SocketPairSpec,
    generate_raw_pair,
    generate_raw_roles,
    generate_socket_pair,
    render_document,
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
        rx_num_bufs=512 if transport == "roce" else None,
        tx_num_bufs=128 if transport == "roce" else None,
        rx_batch_size=32 if transport == "udp" else None,
        memory_kind="host_pinned" if transport == "roce" else "host",
        rx_depth=512 if transport == "roce" else None,
        tx_depth=128 if transport == "roce" else None,
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
    documents["raw-production-no-tx-offload"] = generate_raw_pair(
        _raw_spec(include_benchmark=False, tx_eth_src=False, buffer_size=8064)
    )
    return documents


def _explicit_engine(document: dict[str, Any]) -> str | None:
    return document.get("daqiri", {}).get("cfg", {}).get("engine")


def _rendered_matrix() -> bytes:
    output: list[str] = []
    for name, document in sorted(generated_matrix().items()):
        output.append(f"# {name}\n")
        output.append(render_document(document))
    return "".join(output).encode("utf-8")


def _generate_matrix_in_subprocess(hash_seed: int) -> bytes:
    environment = os.environ.copy()
    environment["PYTHONHASHSEED"] = str(hash_seed)
    result = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "--emit-matrix"],
        check=True,
        capture_output=True,
        env=environment,
    )
    return result.stdout


def _cpp_rejection_documents(
    documents: dict[str, dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    """Return focused regressions for distinct runtime-decoder failure paths."""

    invalid: dict[str, dict[str, Any]] = {}
    base_name = "socket-udp-tx"

    def replace(name: str, path: tuple[str | int, ...], value: Any) -> None:
        document = copy.deepcopy(documents[base_name])
        parent: Any = document
        for component in path[:-1]:
            parent = parent[component]
        parent[path[-1]] = value
        invalid[name] = document

    unknown_queue_key = copy.deepcopy(documents[base_name])
    queue = unknown_queue_key["daqiri"]["cfg"]["interfaces"][0]["rx"]["queues"][0]
    queue["batch_sise"] = queue.pop("batch_size")
    invalid["unknown-queue-key"] = unknown_queue_key

    config_path: tuple[str | int, ...] = ("daqiri", "cfg")
    interface_path = (*config_path, "interfaces", 0)
    tx_flow_path = (*interface_path, "tx", "flows")
    memory_region_path = (*config_path, "memory_regions", 0)
    queue_path = (*interface_path, "tx", "queues", 0)

    replace(
        "integer-overflow",
        (*interface_path, "rx", "dynamic_flow_capacity"),
        1 << 32,
    )
    replace("unknown-offload", (*queue_path, "offloads"), ["tx_eth_scr"])
    replace("malformed-tx-flows", tx_flow_path, "typo")
    for name, match in {
        "malformed-ecpri": {"ecpri": "ecpri_typo"},
        "malformed-ipv4": {"ipv4_src": ["10.0.0.1"]},
    }.items():
        replace(
            name,
            tx_flow_path,
            [
                {
                    "name": name,
                    "id": 1,
                    "match": match,
                    "action": {"type": "queue", "id": 0},
                }
            ],
        )
    replace("invalid-memory-kind", (*memory_region_path, "kind"), "devcie")
    replace("invalid-memory-access", (*memory_region_path, "access"), ["locla"])
    replace("invalid-log-level", (*config_path, "log_level"), "verbose")
    replace("malformed-endpoint-host", (*interface_path, "socket_config", "local_addr"),
            "udp://not-an-ip:5002")
    replace("malformed-endpoint-port", (*interface_path, "socket_config", "local_addr"),
            "udp://10.250.0.1:5002junk")

    missing_memory_region = copy.deepcopy(documents[base_name])
    missing_memory_region["daqiri"]["cfg"]["interfaces"][0]["tx"]["queues"][0][
        "memory_regions"
    ][0] = "missing-region"
    invalid["missing-memory-region"] = missing_memory_region

    return invalid


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--validator",
        type=Path,
        help="built daqiri_config_validate binary used for authoritative validation",
    )
    parser.add_argument(
        "--exclude-dpdk",
        action="store_true",
        help="skip profiles that explicitly require DPDK",
    )
    parser.add_argument("--emit-matrix", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.emit_matrix:
        sys.stdout.buffer.write(_rendered_matrix())
        return 0
    if args.validator is None:
        parser.error(
            "--validator is required; configuration validation uses the C++ decoder"
        )

    first_generation = _generate_matrix_in_subprocess(1)
    second_generation = _generate_matrix_in_subprocess(2)
    if first_generation != second_generation:
        raise RuntimeError(
            "non-deterministic generation across independent processes"
        )

    documents = {
        name: document
        for name, document in generated_matrix().items()
        if not args.exclude_dpdk or _explicit_engine(document) != "dpdk"
    }
    with tempfile.TemporaryDirectory(prefix="daqiri-generated-configs-") as temp_dir:
        paths: dict[str, Path] = {}
        for name, document in documents.items():
            rendered = render_document(document)
            path = Path(temp_dir) / f"{name}.yaml"
            path.write_text(rendered, encoding="utf-8")
            paths[name] = path

        subprocess.run(
            [str(args.validator), *(str(path) for path in paths.values())], check=True
        )

        for name, document in _cpp_rejection_documents(documents).items():
            path = Path(temp_dir) / f"invalid-{name}.yaml"
            path.write_text(
                "%YAML 1.2\n---\n"
                + yaml.safe_dump(document, sort_keys=False, width=1000),
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(args.validator), str(path)],
                capture_output=True,
                text=True,
            )
            if result.returncode != 1:
                raise RuntimeError(
                    f"C++ decoder did not cleanly reject invalid configuration {name} "
                    f"(exit {result.returncode})\n{result.stdout}{result.stderr}"
                )

    print(
        f"Validated {len(documents)} generated configurations with the C++ runtime decoder."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
