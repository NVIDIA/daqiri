#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Exercise the generated configuration matrix and optionally the C++ decoder."""

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
    load_document,
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
    return documents


def _rendered_matrix() -> bytes:
    output: list[str] = []
    for name, document in sorted(generated_matrix().items()):
        validate_document(document)
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
    invalid: dict[str, dict[str, Any]] = {}

    unknown_queue_key = copy.deepcopy(documents["socket-udp-tx"])
    queue = unknown_queue_key["daqiri"]["cfg"]["interfaces"][0]["rx"]["queues"][0]
    queue["batch_sise"] = queue.pop("batch_size")
    invalid["unknown-queue-key"] = unknown_queue_key

    dynamic_flow_overflow = copy.deepcopy(documents["socket-udp-tx"])
    dynamic_flow_overflow["daqiri"]["cfg"]["interfaces"][0]["rx"][
        "dynamic_flow_capacity"
    ] = 1 << 32
    invalid["dynamic-flow-capacity-overflow"] = dynamic_flow_overflow

    metadata_overflow = copy.deepcopy(documents["socket-udp-tx"])
    metadata_overflow["daqiri"]["cfg"]["tx_meta_buffers"] = 1 << 32
    invalid["metadata-overflow"] = metadata_overflow

    min_ipg_overflow = copy.deepcopy(documents["socket-udp-rx"])
    min_ipg_overflow["daqiri"]["cfg"]["interfaces"][0]["socket_config"][
        "min_ipg_ns"
    ] = 1 << 32
    invalid["min-ipg-overflow"] = min_ipg_overflow

    affinity_overflow = copy.deepcopy(documents["socket-udp-tx"])
    affinity_overflow["daqiri"]["cfg"]["memory_regions"][0]["affinity"] = 1 << 16
    invalid["affinity-overflow"] = affinity_overflow

    unknown_offload = copy.deepcopy(documents["socket-udp-tx"])
    unknown_offload["daqiri"]["cfg"]["interfaces"][0]["tx"]["queues"][0][
        "offloads"
    ] = ["tx_eth_scr"]
    invalid["unknown-offload"] = unknown_offload

    malformed_tx_flows = copy.deepcopy(documents["socket-udp-tx"])
    malformed_tx_flows["daqiri"]["cfg"]["interfaces"][0]["tx"]["flows"] = "typo"
    invalid["malformed-tx-flows"] = malformed_tx_flows

    malformed_ecpri = copy.deepcopy(documents["socket-udp-tx"])
    malformed_ecpri["daqiri"]["cfg"]["interfaces"][0]["tx"]["flows"] = [
        {
            "name": "malformed_ecpri",
            "id": 0,
            "match": {"ecpri": "ecpri_typo"},
            "action": {"type": "queue", "id": 0},
        }
    ]
    invalid["malformed-ecpri"] = malformed_ecpri

    malformed_ipv4 = copy.deepcopy(documents["socket-udp-tx"])
    malformed_ipv4["daqiri"]["cfg"]["interfaces"][0]["tx"]["flows"] = [
        {
            "name": "malformed_ipv4",
            "id": 0,
            "match": {"ipv4_src": ["10.0.0.1"]},
            "action": {"type": "queue", "id": 0},
        }
    ]
    invalid["malformed-ipv4"] = malformed_ipv4

    invalid_memory_kind = copy.deepcopy(documents["socket-udp-tx"])
    invalid_memory_kind["daqiri"]["cfg"]["memory_regions"][0]["kind"] = "devcie"
    invalid["invalid-memory-kind"] = invalid_memory_kind

    invalid_memory_access = copy.deepcopy(documents["socket-udp-tx"])
    invalid_memory_access["daqiri"]["cfg"]["memory_regions"][0]["access"] = ["locla"]
    invalid["invalid-memory-access"] = invalid_memory_access

    quoted_integer = copy.deepcopy(documents["socket-udp-tx"])
    quoted_integer["daqiri"]["cfg"]["memory_regions"][0]["num_bufs"] = "128"
    invalid["quoted-integer"] = quoted_integer

    quoted_boolean = copy.deepcopy(documents["socket-udp-tx"])
    quoted_boolean["daqiri"]["cfg"]["debug"] = "false"
    invalid["quoted-boolean"] = quoted_boolean

    return invalid


def _replace_first_num_bufs(document: dict[str, Any], value: str) -> str:
    rendered = render_document(document)
    count = document["daqiri"]["cfg"]["memory_regions"][0]["num_bufs"]
    marker = f"num_bufs: {count}"
    if marker not in rendered:
        raise RuntimeError(f"generated profile no longer contains {marker!r}")
    return rendered.replace(marker, f"num_bufs: {value}", 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--validator",
        type=Path,
        help="also parse every document with the built daqiri_config_validate binary",
    )
    parser.add_argument(
        "--validator-socket-only",
        action="store_true",
        help="limit C++ validation to TCP/UDP for builds without optional engines",
    )
    parser.add_argument("--emit-matrix", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.emit_matrix:
        sys.stdout.buffer.write(_rendered_matrix())
        return 0

    first_generation = _generate_matrix_in_subprocess(1)
    second_generation = _generate_matrix_in_subprocess(2)
    if first_generation != second_generation:
        raise RuntimeError(
            "non-deterministic generation across independent processes"
        )

    documents = generated_matrix()
    with tempfile.TemporaryDirectory(prefix="daqiri-generated-configs-") as temp_dir:
        paths: dict[str, Path] = {}
        for name, document in documents.items():
            validate_document(document)
            rendered = render_document(document)
            path = Path(temp_dir) / f"{name}.yaml"
            path.write_text(rendered, encoding="utf-8")
            paths[name] = path

        if args.validator:
            validator_paths = [
                path
                for name, path in paths.items()
                if not args.validator_socket_only
                or name.startswith(("socket-udp-", "socket-tcp-"))
            ]
            subprocess.run(
                [str(args.validator), *(str(path) for path in validator_paths)], check=True
            )

            explicit_octal_path = Path(temp_dir) / "valid-explicit-octal.yaml"
            octal_base = documents["socket-udp-tx"]
            count = octal_base["daqiri"]["cfg"]["memory_regions"][0]["num_bufs"]
            explicit_octal_path.write_text(
                _replace_first_num_bufs(octal_base, f"0o{count:o}"),
                encoding="utf-8",
            )
            validate_document(load_document(explicit_octal_path))
            subprocess.run([str(args.validator), str(explicit_octal_path)], check=True)

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

            leading_zero_path = Path(temp_dir) / "invalid-leading-zero.yaml"
            leading_zero_path.write_text(
                _replace_first_num_bufs(documents["socket-udp-tx"], "010"),
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(args.validator), str(leading_zero_path)],
                capture_output=True,
                text=True,
            )
            if result.returncode != 1:
                raise RuntimeError(
                    "C++ decoder did not cleanly reject invalid configuration leading-zero "
                    f"(exit {result.returncode})\n{result.stdout}{result.stderr}"
                )

    suffix = " and the C++ decoder rejection checks" if args.validator else ""
    print(f"Validated {len(documents)} generated configurations with JSON Schema{suffix}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
