# SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Deterministic DAQIRI configuration generation."""

from .core import (
    ConfigError,
    apply_overrides,
    load_document,
    render_document,
)
from .profiles import (
    RawPairSpec,
    SocketPairSpec,
    generate_raw_pair,
    generate_raw_roles,
    generate_socket_pair,
)

__all__ = [
    "ConfigError",
    "RawPairSpec",
    "SocketPairSpec",
    "apply_overrides",
    "generate_raw_pair",
    "generate_raw_roles",
    "generate_socket_pair",
    "load_document",
    "render_document",
]
