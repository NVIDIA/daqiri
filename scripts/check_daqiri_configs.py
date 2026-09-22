#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Validate checked-in DAQIRI configurations with the production C++ validator."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
INTEGER_PLACEHOLDER = re.compile(
    r"^(?P<indent>\s*)(?P<key>master_core|cpu_core|affinity):"
    r"\s*<[^>]+>(?P<suffix>\s*(?:#.*)?)$"
)
STRING_PLACEHOLDERS = {
    "<client-ip>": "192.0.2.1",
    "<server-ip>": "192.0.2.2",
}
SEMANTIC_FIXTURE = "examples/daqiri_bench_raw_rx_reorder_seq_batch.yaml"
ZERO_ID_FIXTURE = "examples/daqiri_bench_raw_tx_rx.yaml"
PARSER_FIXTURE = "examples/daqiri_bench_socket_udp_tx_rx.yaml"
FLOW_ID_DEFINITION = "          id: 100\n"
REORDER_SECTION = "        reorder_configs:\n"
DUPLICATE_FLOW = """\
        - name: "duplicate_flow_id"
          id: 100
          action:
            type: queue
            id: 0
          match:
            udp_dst: 4097
"""


def materialize_integer_placeholders(text: str) -> str:
    """Materialize typed values that the production parser validates syntactically."""

    lines: list[str] = []
    for line in text.splitlines(keepends=True):
        newline = "\n" if line.endswith("\n") else ""
        content = line[:-1] if newline else line
        match = INTEGER_PLACEHOLDER.fullmatch(content)
        if match:
            content = (
                f"{match.group('indent')}{match.group('key')}: 0"
                f"{match.group('suffix')}"
            )
        lines.append(content + newline)
    materialized = "".join(lines)
    for placeholder, replacement in STRING_PLACEHOLDERS.items():
        materialized = materialized.replace(placeholder, replacement)
    return materialized


def checked_in_paths() -> list[Path]:
    paths = sorted((REPOSITORY_ROOT / "examples").glob("daqiri_*.yaml"))
    paths.extend(
        sorted((REPOSITORY_ROOT / "applications").glob("**/configs/*.yaml"))
    )
    return paths


def zero_id_multi_interface_case() -> str:
    source = materialize_integer_placeholders(
        (REPOSITORY_ROOT / ZERO_ID_FIXTURE).read_text(encoding="utf-8")
    )
    stream_type = '    stream_type: "raw"\n'
    rx_interface = '    - name: "rx_port"\n'
    bench_rx = "\nbench_rx:"
    if source.count(stream_type) != 1 or source.count(rx_interface) != 1:
        raise ValueError(f"{ZERO_ID_FIXTURE}: compatibility fixture markers changed")
    start = source.index(rx_interface)
    end = source.index(bench_rx)
    first = source[start:end].replace('"rx_port"', '"rx_port_0"')
    second = (
        first.replace('"rx_port_0"', '"rx_port_1"')
        .replace('"rq_q_0"', '"rq_q_1"')
        .replace('"flow_0"', '"flow_1"')
        .replace("<0000:00:00.0>", "<0000:00:00.1>")
    )
    source = source[:start] + first + second + source[end:]
    return source.replace(stream_type, stream_type + '    engine: "dpdk"\n', 1)


def semantic_invalid_cases() -> dict[str, str]:
    source = materialize_integer_placeholders(
        (REPOSITORY_ROOT / SEMANTIC_FIXTURE).read_text(encoding="utf-8")
    )
    if source.count(FLOW_ID_DEFINITION) != 1 or source.count(REORDER_SECTION) != 1:
        raise ValueError(f"{SEMANTIC_FIXTURE}: semantic fixture markers changed")
    return {
        "unknown-reorder-flow.yaml": source.replace(
            FLOW_ID_DEFINITION, "          id: 999\n", 1
        ),
        "duplicate-static-flow-id.yaml": source.replace(
            REORDER_SECTION, DUPLICATE_FLOW + REORDER_SECTION, 1
        ),
    }


def parser_invalid_cases() -> dict[str, str]:
    """Create focused malformed inputs from one known-good socket configuration."""

    source = (REPOSITORY_ROOT / PARSER_FIXTURE).read_text(encoding="utf-8")

    def replace(name: str, old: str, new: str) -> str:
        if old not in source:
            raise ValueError(f"{PARSER_FIXTURE}: marker for {name} changed")
        return source.replace(old, new, 1)

    tx_flow_prefix = """\
      tx:
        flows:
        - name: invalid
          id: 1
          match:
"""
    tx_flow_suffix = """\
          action:
            type: queue
            id: 0
        queues:
"""

    return {
        "unknown-queue-key.yaml": replace(
            "unknown queue key",
            "          batch_size: 32\n",
            "          batch_sise: 32\n",
        ),
        "integer-overflow.yaml": replace(
            "integer overflow",
            "      rx:\n        queues:\n",
            "      rx:\n        dynamic_flow_capacity: 4294967296\n        queues:\n",
        ),
        "unknown-offload.yaml": replace(
            "unknown offload",
            "          batch_size: 1\n          memory_regions:\n",
            "          batch_size: 1\n          offloads:\n"
            "            - tx_eth_scr\n          memory_regions:\n",
        ),
        "malformed-tx-flows.yaml": replace(
            "malformed tx flows",
            "      tx:\n        queues:\n",
            "      tx:\n        flows: typo\n        queues:\n",
        ),
        "malformed-ecpri.yaml": replace(
            "malformed eCPRI match",
            "      tx:\n        queues:\n",
            tx_flow_prefix + "            ecpri: ecpri_typo\n" + tx_flow_suffix,
        ),
        "malformed-ipv4.yaml": replace(
            "malformed IPv4 match",
            "      tx:\n        queues:\n",
            tx_flow_prefix
            + "            ipv4_src:\n              - 10.0.0.1\n"
            + tx_flow_suffix,
        ),
        "invalid-memory-kind.yaml": replace(
            "invalid memory kind", '      kind: "host"\n', '      kind: "devcie"\n'
        ),
        "invalid-memory-access.yaml": replace(
            "invalid memory access",
            '      kind: "host"\n',
            '      kind: "host"\n      access:\n        - locla\n',
        ),
        "invalid-log-level.yaml": replace(
            "invalid log level",
            '    log_level: "info"\n',
            '    log_level: "verbose"\n',
        ),
        "malformed-endpoint-host.yaml": replace(
            "malformed endpoint host",
            '        local_addr: "udp://127.0.0.1:5001"\n',
            '        local_addr: "udp://not-an-ip:5001"\n',
        ),
        "malformed-endpoint-port.yaml": replace(
            "malformed endpoint port",
            '        local_addr: "udp://127.0.0.1:5001"\n',
            '        local_addr: "udp://127.0.0.1:5001junk"\n',
        ),
        "missing-memory-region.yaml": replace(
            "missing memory region",
            '            - "DATA_SOCKET_SERVER"\n\n    - name: udp_client\n',
            '            - "missing-region"\n\n    - name: udp_client\n',
        ),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--validator",
        type=Path,
        required=True,
        help="path to the built daqiri_config_validate executable",
    )
    parser.add_argument("paths", nargs="*", type=Path)
    args = parser.parse_args(argv)

    paths = args.paths or checked_in_paths()
    if not paths:
        parser.error("no configuration files were found")

    compatibility_count = 0
    invalid_count = 0
    with tempfile.TemporaryDirectory(prefix="daqiri-checked-configs-") as temp_dir:
        materialized_paths: list[Path] = []
        for index, path in enumerate(paths):
            try:
                text = path.read_text(encoding="utf-8")
            except OSError as error:
                print(f"{path}: {error}", file=sys.stderr)
                return 1
            output_path = Path(temp_dir) / f"{index}-{path.name}"
            output_path.write_text(
                materialize_integer_placeholders(text), encoding="utf-8"
            )
            materialized_paths.append(output_path)

        result = subprocess.run(
            [str(args.validator), *(str(path) for path in materialized_paths)],
            check=False,
        )
        if result.returncode != 0:
            return result.returncode

        if not args.paths:
            try:
                compatibility_case = zero_id_multi_interface_case()
                invalid_cases = {**semantic_invalid_cases(), **parser_invalid_cases()}
            except (OSError, ValueError) as error:
                print(error, file=sys.stderr)
                return 1
            compatibility_path = Path(temp_dir) / "zero-flow-id-per-interface.yaml"
            compatibility_path.write_text(compatibility_case, encoding="utf-8")
            result = subprocess.run(
                [str(args.validator), str(compatibility_path)], check=False
            )
            if result.returncode != 0:
                print(
                    "zero-flow-id-per-interface.yaml: expected validator exit status 0, "
                    f"got {result.returncode}",
                    file=sys.stderr,
                )
                return 1
            compatibility_count = 1
            invalid_count = len(invalid_cases)
            for name, text in invalid_cases.items():
                invalid_path = Path(temp_dir) / name
                invalid_path.write_text(text, encoding="utf-8")
                result = subprocess.run(
                    [str(args.validator), str(invalid_path)], check=False
                )
                if result.returncode != 1:
                    print(
                        f"{name}: expected validator exit status 1, "
                        f"got {result.returncode}",
                        file=sys.stderr,
                    )
                    return 1

    compatibility_label = "case" if compatibility_count == 1 else "cases"
    print(
        f"Validated {len(materialized_paths)} checked-in configurations and "
        f"{compatibility_count} compatibility {compatibility_label}; rejected "
        f"{invalid_count} invalid configurations without hardware initialization."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
