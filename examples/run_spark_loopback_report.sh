#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Compatibility entry point for the consolidated benchmark controller. New
# invocations should call scripts/run_crosshost_bench.sh directly.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONTROLLER="$SCRIPT_DIR/../scripts/run_crosshost_bench.sh"

: "${LOOPBACK_TX_NETDEV:?LOOPBACK_TX_NETDEV must name the cabled transmit port}"
: "${LOOPBACK_RX_NETDEV:?LOOPBACK_RX_NETDEV must name the cabled receive port}"

exec "$CONTROLLER" \
  --suite spark-loopback-report \
  --topology loopback \
  --loopback-tx-netdev "$LOOPBACK_TX_NETDEV" \
  --loopback-rx-netdev "$LOOPBACK_RX_NETDEV" \
  "$@"
