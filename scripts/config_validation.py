# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import subprocess
from collections.abc import Iterable, Mapping
from pathlib import Path
from typing import Any

import yaml


KNOWN_ENGINES = frozenset(("socket", "dpdk", "ibverbs"))
ENGINE_QUERY_ERROR = "Cannot determine the validator's compiled engines."
NO_SUPPORTED_CONFIGURATIONS = "No configurations are supported by this validator."


def query_compiled_engines(validator: Path) -> frozenset[str]:
    try:
        result = subprocess.run(
            [str(validator), "--list-engines"],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise RuntimeError from error
    if result.returncode != 0:
        raise RuntimeError
    lines = result.stdout.splitlines()
    if len(lines) != 1:
        raise RuntimeError
    engines = result.stdout.split()
    if not engines or len(engines) != len(set(engines)):
        raise RuntimeError
    if not set(engines).issubset(KNOWN_ENGINES):
        raise RuntimeError
    return frozenset(engines)


def _config_mapping(document: Any) -> Mapping[str, Any] | None:
    if not isinstance(document, Mapping):
        return None
    daqiri = document.get("daqiri")
    if daqiri is not None:
        if not isinstance(daqiri, Mapping) or not isinstance(daqiri.get("cfg"), Mapping):
            return None
        return daqiri["cfg"]
    return document


def required_engines(document: Any) -> frozenset[str] | None:
    config = _config_mapping(document)
    if config is None:
        return None

    stream_type = config.get("stream_type")
    explicit_engine = config.get("engine")
    if stream_type == "raw":
        if explicit_engine in (None, "", "default"):
            return frozenset(("dpdk", "ibverbs"))
        if explicit_engine in ("dpdk", "ibverbs"):
            return frozenset((explicit_engine,))
        return None

    if stream_type != "socket":
        return None
    if explicit_engine == "ibverbs":
        return frozenset(("ibverbs",))
    if explicit_engine not in (None, "", "default", "socket"):
        return None

    interfaces = config.get("interfaces")
    if not isinstance(interfaces, Iterable) or isinstance(interfaces, (str, bytes)):
        return None
    protocols: set[str] = set()
    for interface in interfaces:
        if not isinstance(interface, Mapping):
            return None
        socket_config = interface.get("socket_config")
        if not isinstance(socket_config, Mapping):
            return None
        addresses = [socket_config.get("local_addr"), socket_config.get("remote_addr")]
        for address in addresses:
            if not isinstance(address, str) or "://" not in address:
                continue
            protocols.add(address.split("://", 1)[0].lower())
    if not protocols or not protocols.issubset({"udp", "tcp", "roce"}):
        return None
    if "roce" in protocols:
        return frozenset(("ibverbs",))
    return frozenset(("socket",))


def required_engines_from_path(path: Path) -> frozenset[str] | None:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None
    return required_engines_from_text(text)


def required_engines_from_text(text: str) -> frozenset[str] | None:
    try:
        document = yaml.safe_load(text)
    except yaml.YAMLError:
        return None
    return required_engines(document)


def supports_engines(document: Any, available_engines: frozenset[str]) -> bool:
    required = required_engines(document)
    return required is None or bool(required & available_engines)


def select_supported_paths(
    paths: Iterable[Path], available_engines: frozenset[str]
) -> list[Path]:
    selected: list[Path] = []
    for path in paths:
        required = required_engines_from_path(path)
        if required is None or required & available_engines:
            selected.append(path)
    return selected
