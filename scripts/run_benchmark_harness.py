#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Stable entrypoint for the DAQIRI reproducible benchmark harness."""

from benchmark_harness.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
