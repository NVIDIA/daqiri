#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Spark-to-Spark ResNet xhost runner.
#   TX: spark-stacked-01 (ncg-spark-0177)
#   RX: spark-stacked-02 (ncg-spark-7013)
#
# Assumes passwordless SSH and a shared NFS checkout. Start from either host.
#
# Usage:
#   ./tools/run_resnet_xhost.sh [--replay-once|--seconds N] [--dataset PATH]
#
set -euo pipefail

TX_HOST="${TX_HOST:-spark-stacked-01}"
RX_HOST="${RX_HOST:-spark-stacked-02}"
RX_IFACE="${RX_IFACE:-det1}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${APP_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-resnet}"
BIN="${BUILD_DIR}/applications/resnet50_inference/daqiri_resnet50_inference"
TX_CFG="${BUILD_DIR}/applications/resnet50_inference/configs/resnet50_tx_spark_xhost.yaml"
RX_CFG="${BUILD_DIR}/applications/resnet50_inference/configs/resnet50_rx_spark_xhost.yaml"

RESULTS_DIR="${RESULTS_DIR:-${REPO_ROOT}/resnet-results}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RUN_SECONDS=""

MODE_ARGS=(--replay-once)
DATASET_ARGS=()
SECONDS_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --replay-once) MODE_ARGS=(--replay-once); shift ;;
    --loop) MODE_ARGS=(--loop); shift ;;
    --seconds) SECONDS_ARGS=(--seconds "$2"); MODE_ARGS=(--loop); RUN_SECONDS="$2"; shift 2 ;;
    --results-dir) RESULTS_DIR="$2"; shift 2 ;;
    --run-id) RUN_ID="$2"; shift 2 ;;
    --dataset) DATASET_ARGS=(--dataset "$2"); shift 2 ;;
    --build-dir) BUILD_DIR="$2"; BIN="${BUILD_DIR}/applications/resnet50_inference/daqiri_resnet50_inference"
                 TX_CFG="${BUILD_DIR}/applications/resnet50_inference/configs/resnet50_tx_spark_xhost.yaml"
                 RX_CFG="${BUILD_DIR}/applications/resnet50_inference/configs/resnet50_rx_spark_xhost.yaml"
                 shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -x "${BIN}" ]]; then
  echo "Binary not found: ${BIN}" >&2
  echo "Build with -DDAQIRI_BUILD_APPLICATIONS=ON first." >&2
  exit 1
fi

echo "Resolving RX MAC on ${RX_HOST} (${RX_IFACE})..."
RX_MAC="$(ssh "${RX_HOST}" "cat /sys/class/net/${RX_IFACE}/address")"
echo "RX MAC=${RX_MAC}"
echo "Reminder: run scripts/setup_spark_xhost_net.sh on both hosts if not already done."

RX_LOG="$(mktemp /tmp/resnet-rx.XXXXXX.log)"
TX_LOG="$(mktemp /tmp/resnet-tx.XXXXXX.log)"
cleanup() {
  ssh "${RX_HOST}" "pkill -f daqiri_resnet50_inference || true" 2>/dev/null || true
  ssh "${TX_HOST}" "pkill -f daqiri_resnet50_inference || true" 2>/dev/null || true
}
trap cleanup EXIT

echo "Starting RX on ${RX_HOST}..."
# shellcheck disable=SC2029
ssh "${RX_HOST}" "cd '${REPO_ROOT}' && sudo '${BIN}' '${RX_CFG}' --mode rx ${DATASET_ARGS[*]} ${SECONDS_ARGS[*]} ${MODE_ARGS[*]}" \
  >"${RX_LOG}" 2>&1 &
RX_SSH_PID=$!

echo "Waiting for set_reorder_cuda_stream and TrtRunner ready..."
for _ in $(seq 1 600); do
  if grep -q "set_reorder_cuda_stream OK" "${RX_LOG}" 2>/dev/null && \
     grep -q "TrtRunner ready" "${RX_LOG}" 2>/dev/null; then
    break
  fi
  if ! kill -0 "${RX_SSH_PID}" 2>/dev/null; then
    echo "RX exited early; log:" >&2
    cat "${RX_LOG}" >&2
    exit 1
  fi
  sleep 1
done

if ! grep -q "set_reorder_cuda_stream OK" "${RX_LOG}" 2>/dev/null || \
   ! grep -q "TrtRunner ready" "${RX_LOG}" 2>/dev/null; then
  echo "Timed out waiting for RX ready; log:" >&2
  cat "${RX_LOG}" >&2
  exit 1
fi

echo "Starting TX on ${TX_HOST}..."
# `|| TX_RC=$?` is required: under `set -e` a bare failing ssh terminates the
# script before the log dump below, which is exactly when the logs are needed.
TX_RC=0
# shellcheck disable=SC2029
ssh "${TX_HOST}" "cd '${REPO_ROOT}' && export ETH_DST_ADDR='${RX_MAC}' && sudo -E '${BIN}' '${TX_CFG}' --mode tx ${DATASET_ARGS[*]} ${SECONDS_ARGS[*]} ${MODE_ARGS[*]}" \
  >"${TX_LOG}" 2>&1 || TX_RC=$?

echo "Waiting for RX to finish..."
wait "${RX_SSH_PID}" || true

echo "=== TX log (${TX_LOG}) ==="
cat "${TX_LOG}"
echo "=== RX log (${RX_LOG}) ==="
cat "${RX_LOG}"

# Persist the run so a sweep has something to aggregate. Previously the harness
# only streamed to /tmp logs and committed nothing, so batch counts, latency
# percentiles and drop reasons had to be read off a terminal by hand.
mkdir -p "${RESULTS_DIR}"
{
  # Fields the results table needs to be reproducible: what was run, for how
  # long, how many batches it actually completed, and what it dropped.
  echo "run_id,${RUN_ID}"
  echo "rx_host,${RX_HOST}"
  echo "tx_host,${TX_HOST}"
  echo "rx_config,${RX_CFG}"
  echo "tx_config,${TX_CFG}"
  echo "seconds_requested,${RUN_SECONDS:-}"
  echo "tx_rc,${TX_RC}"
  grep -E "^inference_consumer_worker: [0-9]+ inference batches" "${RX_LOG}" |
    sed -E 's/^inference_consumer_worker: ([0-9]+) inference batches in ([0-9.]+) s.*/inference_batches,\1\nelapsed_s,\2/' || true
  grep -E "^inference latency \(ms\)" "${RX_LOG}" |
    sed -E 's/.*mean=([0-9.]+) p50=([0-9.]+) p99=([0-9.]+) .*n=([0-9]+).*/latency_mean_ms,\1\nlatency_p50_ms,\2\nlatency_p99_ms,\3\nlatency_samples,\4/' || true
  grep -E "^rx_producer_worker: delivered_bursts" "${RX_LOG}" || true
  grep -E "^rx_producer_worker: partial_bursts" "${RX_LOG}" || true
} >"${RESULTS_DIR}/${RUN_ID}.summary"

# Per-interval throughput, for locating the warmup knee and checking the rate
# held rather than decayed.
grep -E "^inference_consumer_worker: t=" "${RX_LOG}" >"${RESULTS_DIR}/${RUN_ID}.timeseries" || true

cp "${RX_LOG}" "${RESULTS_DIR}/${RUN_ID}.rx.log"
cp "${TX_LOG}" "${RESULTS_DIR}/${RUN_ID}.tx.log"
echo "=== results written to ${RESULTS_DIR}/${RUN_ID}.* ==="
cat "${RESULTS_DIR}/${RUN_ID}.summary"

exit "${TX_RC}"
