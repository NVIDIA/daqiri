# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
GENERATED_CHECK = REPOSITORY_ROOT / "scripts/check_generated_configs.py"
CHECKED_IN_CHECK = REPOSITORY_ROOT / "scripts/check_daqiri_configs.py"

FAKE_VALIDATOR = """\
#!/usr/bin/env python3
import os
import sys
from pathlib import Path

log = Path(os.environ["FAKE_VALIDATOR_LOG"])
if sys.argv[1:] == ["--list-engines"]:
    with log.open("a", encoding="utf-8") as stream:
        stream.write("QUERY\\n")
    if os.environ.get("FAKE_VALIDATOR_QUERY_FAILURE"):
        raise SystemExit(7)
    print(os.environ["FAKE_VALIDATOR_ENGINES"])
    raise SystemExit(0)

invalid_markers = (
    "batch_sise",
    "4294967296",
    "tx_eth_scr",
    "flows: typo",
    "ecpri_typo",
    "ipv4_src:\\n",
    "devcie",
    "locla",
    'log_level: "verbose"',
    "not-an-ip",
    "5001junk",
    "missing-region",
    "id: 999",
    "duplicate_flow_id",
    "UNSUPPORTED_MARKER",
)
return_code = 0
for argument in sys.argv[1:]:
    path = Path(argument)
    text = path.read_text(encoding="utf-8")
    kind = "invalid" if any(marker in text for marker in invalid_markers) else "valid"
    with log.open("a", encoding="utf-8") as stream:
        stream.write(f"VALIDATE\\t{path.name}\\t{kind}\\n")
    if kind == "invalid":
        return_code = 1
raise SystemExit(return_code)
"""


def fake_validator(tmp_path: Path) -> tuple[Path, Path]:
    validator = tmp_path / "fake-validator"
    validator.write_text(FAKE_VALIDATOR, encoding="utf-8")
    validator.chmod(0o755)
    log = tmp_path / "validator.log"
    return validator, log


def run_script(
    script: Path,
    validator: Path,
    log: Path,
    engines: str,
    *extra: str,
    query_failure: bool = False,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["FAKE_VALIDATOR_ENGINES"] = engines
    environment["FAKE_VALIDATOR_LOG"] = str(log)
    if query_failure:
        environment["FAKE_VALIDATOR_QUERY_FAILURE"] = "1"
    return subprocess.run(
        [sys.executable, str(script), "--validator", str(validator), *extra],
        cwd=REPOSITORY_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )


def validation_names(log: Path) -> list[str]:
    return [
        line.split("\t", 2)[1]
        for line in log.read_text().splitlines()
        if line.startswith("VALIDATE")
    ]


@pytest.mark.parametrize(
    ("engines", "required_present", "required_absent"),
    [
        (
            "socket dpdk ibverbs",
            {
                "socket-udp-tx.yaml",
                "socket-tcp-tx.yaml",
                "socket-roce-tx.yaml",
                "raw-dpdk-none.yaml",
                "raw-ibverbs-none.yaml",
                "raw-xhost-tx.yaml",
            },
            set(),
        ),
        (
            "socket dpdk",
            {"socket-udp-tx.yaml", "socket-tcp-tx.yaml", "raw-dpdk-none.yaml"},
            {"socket-roce-tx.yaml", "raw-ibverbs-none.yaml", "raw-xhost-tx.yaml"},
        ),
        (
            "socket ibverbs",
            {
                "socket-udp-tx.yaml",
                "socket-tcp-tx.yaml",
                "socket-roce-tx.yaml",
                "raw-ibverbs-none.yaml",
                "raw-xhost-tx.yaml",
            },
            {"raw-dpdk-none.yaml"},
        ),
        (
            "socket",
            {"socket-udp-tx.yaml", "socket-tcp-tx.yaml"},
            {"socket-roce-tx.yaml", "raw-dpdk-none.yaml", "raw-ibverbs-none.yaml"},
        ),
    ],
)
def test_generated_check_selects_only_supported_engine_profiles(
    tmp_path: Path,
    engines: str,
    required_present: set[str],
    required_absent: set[str],
) -> None:
    validator, log = fake_validator(tmp_path)
    result = run_script(GENERATED_CHECK, validator, log, engines)

    assert result.returncode == 0, result.stderr
    names = set(validation_names(log))
    assert required_present <= names
    assert names.isdisjoint(required_absent)


def test_generated_check_keeps_exclude_dpdk_as_an_additional_restriction(
    tmp_path: Path,
) -> None:
    validator, log = fake_validator(tmp_path)
    result = run_script(
        GENERATED_CHECK, validator, log, "socket dpdk ibverbs", "--exclude-dpdk"
    )

    assert result.returncode == 0, result.stderr
    names = set(validation_names(log))
    assert not any(name.startswith("raw-dpdk-") for name in names)
    assert not any(name.startswith("raw-mq-") for name in names)
    assert "raw-ibverbs-none.yaml" in names


@pytest.mark.parametrize("script", [GENERATED_CHECK, CHECKED_IN_CHECK])
def test_check_fails_when_capability_query_fails(tmp_path: Path, script: Path) -> None:
    validator, log = fake_validator(tmp_path)
    result = run_script(
        script,
        validator,
        log,
        "socket dpdk ibverbs",
        query_failure=True,
    )

    assert result.returncode == 1
    assert "Cannot determine the validator's compiled engines." in result.stderr


@pytest.mark.parametrize(
    "engines", ["socket", "socket dpdk", "socket ibverbs", "socket dpdk ibverbs"]
)
def test_checked_in_default_selects_supported_cases(tmp_path: Path, engines: str) -> None:
    validator, log = fake_validator(tmp_path)
    result = run_script(CHECKED_IN_CHECK, validator, log, engines)

    assert result.returncode == 0, result.stderr
    names = validation_names(log)
    assert any(name.endswith("daqiri_bench_socket_udp_tx_rx.yaml") for name in names)
    assert any(
        name.endswith("daqiri_bench_raw_hw_loopback_ibverbs.yaml") for name in names
    ) == ("ibverbs" in engines)
    assert any(
        name.endswith("daqiri_bench_raw_sw_loopback.yaml") for name in names
    ) == ("dpdk" in engines)
    assert any(
        name.endswith("daqiri_bench_raw_tx_rx.yaml") for name in names
    ) == ("dpdk" in engines or "ibverbs" in engines)
    assert ("zero-flow-id-per-interface.yaml" in names) == ("dpdk" in engines)
    assert "unknown-queue-key.yaml" in names
    assert "malformed-tx-flows.yaml" in names
    assert ("unknown-reorder-flow.yaml" in names) == ("ibverbs" in engines)


def test_checked_in_explicit_path_is_unfiltered_and_validator_failure_is_reported(
    tmp_path: Path,
) -> None:
    validator, log = fake_validator(tmp_path)
    config = tmp_path / "unsupported.yaml"
    config.write_text(
        "daqiri:\n  cfg:\n    stream_type: raw\n    engine: dpdk\n"
        "    UNSUPPORTED_MARKER: true\n",
        encoding="utf-8",
    )

    result = run_script(CHECKED_IN_CHECK, validator, log, "socket", str(config))

    assert result.returncode == 1
    assert any(line.endswith("\tinvalid") for line in log.read_text().splitlines())
