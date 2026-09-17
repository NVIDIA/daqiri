# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Deterministic YAML loading, overrides, and serialization."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import yaml


class ConfigError(ValueError):
    """Raised when generator input cannot be loaded, overridden, or rendered."""


class Yaml12SafeLoader(yaml.SafeLoader):
    """Safe loader with YAML 1.2-style scalar resolution.

    In particular, YAML 1.1's sexagesimal float rule turns an unquoted PCI BDF
    such as ``0000:01:00.0`` into ``60.0``. DAQIRI configs declare YAML 1.2,
    where that scalar is a string.
    """


Yaml12SafeLoader.yaml_implicit_resolvers = {
    key: list(resolvers)
    for key, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
}
for first_character, resolvers in Yaml12SafeLoader.yaml_implicit_resolvers.items():
    Yaml12SafeLoader.yaml_implicit_resolvers[first_character] = [
        resolver
        for resolver in resolvers
        if resolver[0]
        not in (
            "tag:yaml.org,2002:bool",
            "tag:yaml.org,2002:float",
            "tag:yaml.org,2002:int",
        )
    ]
Yaml12SafeLoader.add_implicit_resolver(
    "tag:yaml.org,2002:bool", re.compile(r"^(?:true|false)$", re.IGNORECASE), list("tTfF")
)
Yaml12SafeLoader.add_implicit_resolver(
    "tag:yaml.org,2002:int",
    re.compile(r"^[-+]?(?:0|[1-9][0-9]*|0o[0-7]+|0x[0-9a-fA-F]+)$"),
    list("-+0123456789"),
)
Yaml12SafeLoader.add_implicit_resolver(
    "tag:yaml.org,2002:float",
    re.compile(
        r"^[-+]?(?:(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?|"
        r"[0-9]+[eE][-+]?[0-9]+|\.inf|\.Inf|\.INF|\.nan|\.NaN|\.NAN)$"
    ),
    list("-+0123456789."),
)


def load_document(path: str | Path) -> dict[str, Any]:
    """Load a YAML document from *path*."""

    try:
        with Path(path).open(encoding="utf-8") as stream:
            document = yaml.load(stream, Loader=Yaml12SafeLoader)
    except (OSError, yaml.YAMLError) as exc:
        raise ConfigError(f"cannot load {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise ConfigError(f"{path} must contain a YAML mapping")
    return document


def apply_overrides(
    document: dict[str, Any], assignments: list[str]
) -> dict[str, Any]:
    """Replace existing values using JSON Pointer ``/path/to/value=YAML`` inputs."""

    for assignment in assignments:
        if "=" not in assignment:
            raise ConfigError(f"override must have the form /path=value: {assignment}")
        pointer, raw_value = assignment.split("=", 1)
        if not pointer.startswith("/"):
            raise ConfigError(f"override path must be a JSON Pointer: {pointer}")
        tokens = [
            token.replace("~1", "/").replace("~0", "~")
            for token in pointer.removeprefix("/").split("/")
        ]
        try:
            value = yaml.load(raw_value, Loader=Yaml12SafeLoader)
        except yaml.YAMLError as exc:
            raise ConfigError(f"invalid YAML value for {pointer}: {exc}") from exc

        def list_index(token: str, length: int) -> int:
            if re.fullmatch(r"0|[1-9][0-9]*", token) is None:
                raise ConfigError(f"override path does not exist: {pointer}")
            index = int(token)
            if index >= length:
                raise ConfigError(f"override path does not exist: {pointer}")
            return index

        parent: Any = document
        for token in tokens[:-1]:
            if isinstance(parent, dict):
                if token not in parent:
                    raise ConfigError(f"override path does not exist: {pointer}")
                parent = parent[token]
            elif isinstance(parent, list):
                parent = parent[list_index(token, len(parent))]
            else:
                raise ConfigError(f"override path does not exist: {pointer}")

        leaf = tokens[-1]
        if isinstance(parent, dict):
            if leaf not in parent:
                raise ConfigError(f"override path does not exist: {pointer}")
            parent[leaf] = value
        elif isinstance(parent, list):
            parent[list_index(leaf, len(parent))] = value
        else:
            raise ConfigError(f"override path does not exist: {pointer}")
    return document


def _placeholder_paths(value: Any, path: str = "") -> list[str]:
    paths: list[str] = []
    if isinstance(value, dict):
        for key, child in value.items():
            paths.extend(_placeholder_paths(child, f"{path}/{key}"))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            paths.extend(_placeholder_paths(child, f"{path}/{index}"))
    elif isinstance(value, str) and re.search(r"<[^<>]+>", value):
        paths.append(path or "/")
    return paths


def render_document(document: dict[str, Any]) -> str:
    """Serialize a concrete document with stable ordering and formatting."""

    placeholders = _placeholder_paths(document)
    if placeholders:
        detail = ", ".join(placeholders)
        raise ConfigError(f"unresolved angle-bracket placeholder(s): {detail}")
    body = yaml.safe_dump(
        document,
        sort_keys=False,
        default_flow_style=False,
        allow_unicode=False,
        width=1000,
    )
    return "%YAML 1.2\n---\n" + body
