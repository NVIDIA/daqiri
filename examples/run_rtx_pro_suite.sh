#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Run the standard RTX PRO 6000 benchmark matrix and emit plots where available.
#
# Usage (root, privileged container):
#   ./examples/run_rtx_pro_suite.sh
#
# Environment:
#   RUN_SECONDS       per MQ cell (default 20; nic/sw modes use 30)
#   SKIP_MQ=1         skip multi-queue sweep
#   SKIP_WIRE=1       skip wire-backed dpdk modes (nic-smoke, sweep)
#   SKIP_SW=1         skip software loopback smoke

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUN_SECONDS="${RUN_SECONDS:-20}"
WIRE_SECONDS="${WIRE_SECONDS:-30}"

run_mq() {
  echo "========== RTX PRO multi-queue sweep =========="
  RUN_SECONDS="$RUN_SECONDS" "$SCRIPT_DIR/run_rtx_pro_mq_bench.sh"
}

run_wire_smoke() {
  echo "========== RTX PRO dpdk nic-smoke =========="
  "$SCRIPT_DIR/run_rtx_pro_bench.sh" dpdk nic-smoke --seconds "$WIRE_SECONDS"
}

run_sw_smoke() {
  echo "========== RTX PRO dpdk sw-smoke =========="
  "$SCRIPT_DIR/run_rtx_pro_bench.sh" dpdk sw-smoke --seconds "$WIRE_SECONDS"
}

run_sweep() {
  echo "========== RTX PRO dpdk sweep =========="
  "$SCRIPT_DIR/run_rtx_pro_bench.sh" dpdk sweep --seconds "$WIRE_SECONDS"
}

[[ "${SKIP_MQ:-0}" == "1" ]] || run_mq
[[ "${SKIP_WIRE:-0}" == "1" ]] || run_wire_smoke
[[ "${SKIP_SW:-0}" == "1" ]] || run_sw_smoke
[[ "${SKIP_SWEEP:-1}" == "1" ]] || run_sweep

echo
echo "Suite complete. Results under: ${DAQIRI_BENCH_RESULTS_DIR:-$REPO_ROOT/bench-results}/"
