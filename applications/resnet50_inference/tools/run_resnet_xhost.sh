#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Cross-host ResNet runner: one host transmits a CIFAR pcap, the other receives,
# reorders on the GPU and runs TensorRT inference.
#
# Defaults describe the Spark-to-Spark pair (TX spark-stacked-01, RX
# spark-stacked-02) with a shared NFS checkout. Every host-specific value is
# overridable, so an asymmetric pair -- different repo paths, different configs,
# one side in a container -- needs no edits to this script. See
# tools/env/ for ready-made environment files.
#
# Usage:
#   ./tools/run_resnet_xhost.sh [--replay-once|--seconds N] [--dataset PATH]
#   RX_ENV=tools/env/thor-rx.env ./tools/run_resnet_xhost.sh --seconds 30
#
# Per-side overrides (TX_* / RX_*):
#   {TX,RX}_HOST      ssh destination
#   {TX,RX}_REPO      repo root ON THAT HOST (they need not match)
#   {TX,RX}_BUILD     build dir on that host
#   {TX,RX}_BIN       binary path as the launcher sees it
#   {TX,RX}_CFG       config path as the launcher sees it
#   {TX,RX}_LAUNCHER  command prefix before the binary. Default "sudo".
#                     Set to a full `docker run ... <image>` to run in a
#                     container; then _BIN/_CFG are container-side paths.
#   RX_IFACE          netdev whose MAC becomes the TX destination
#
set -euo pipefail

# Optional env files, sourced before defaults so they can set any of the above.
[[ -n "${TX_ENV:-}" ]] && source "${TX_ENV}"
[[ -n "${RX_ENV:-}" ]] && source "${RX_ENV}"

# Run a command on a host, or locally when that host IS this machine. Without
# this, a single-host or same-host-as-runner setup would need loopback ssh
# (key plus known_hosts) purely to talk to itself.
on_host() {
  local host="$1"; shift
  case "${host}" in
    localhost|127.0.0.1|"") bash -c "$*" ;;
    *)                      ssh "${host}" "$*" ;;
  esac
}

TX_HOST="${TX_HOST:-spark-stacked-01}"
RX_HOST="${RX_HOST:-spark-stacked-02}"
RX_IFACE="${RX_IFACE:-det1}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${APP_DIR}/../.." && pwd)"

# Per-side paths default to this checkout's layout, which is correct when the
# hosts share a mount. Asymmetric pairs override the ones that differ.
TX_REPO="${TX_REPO:-${REPO_ROOT}}"
RX_REPO="${RX_REPO:-${REPO_ROOT}}"
BUILD_DIR="${BUILD_DIR:-build-resnet}"
TX_BUILD="${TX_BUILD:-${TX_REPO}/${BUILD_DIR#/}}"
RX_BUILD="${RX_BUILD:-${RX_REPO}/${BUILD_DIR#/}}"

APP_SUBDIR="applications/resnet50_inference"
TX_BIN="${TX_BIN:-${TX_BUILD}/${APP_SUBDIR}/daqiri_resnet50_inference}"
RX_BIN="${RX_BIN:-${RX_BUILD}/${APP_SUBDIR}/daqiri_resnet50_inference}"
TX_CFG="${TX_CFG:-${TX_BUILD}/${APP_SUBDIR}/configs/resnet50_tx_spark_xhost.yaml}"
RX_CFG="${RX_CFG:-${RX_BUILD}/${APP_SUBDIR}/configs/resnet50_rx_spark_xhost.yaml}"

# Prefix before the binary. "sudo" for bare metal; a full `docker run` for a
# containerized side (GPU/NIC device flags belong here, not in this script).
# TX must forward ETH_DST_ADDR from the environment: `sudo -E` bare metal, or
# `-e ETH_DST_ADDR` in a docker launcher.
TX_LAUNCHER="${TX_LAUNCHER:-sudo -E}"
RX_LAUNCHER="${RX_LAUNCHER:-sudo}"

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
    # Rewrites both sides. Use TX_BUILD/RX_BUILD directly for an asymmetric pair.
    --build-dir) TX_BUILD="${TX_REPO}/${2#/}"; RX_BUILD="${RX_REPO}/${2#/}"
                 TX_BIN="${TX_BUILD}/${APP_SUBDIR}/daqiri_resnet50_inference"
                 RX_BIN="${RX_BUILD}/${APP_SUBDIR}/daqiri_resnet50_inference"
                 TX_CFG="${TX_BUILD}/${APP_SUBDIR}/configs/resnet50_tx_spark_xhost.yaml"
                 RX_CFG="${RX_BUILD}/${APP_SUBDIR}/configs/resnet50_rx_spark_xhost.yaml"
                 shift 2 ;;
    --tx-cfg) TX_CFG="$2"; shift 2 ;;
    --rx-cfg) RX_CFG="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

# Check on the host that will run it, not locally: the paths may be
# container-side, and with an asymmetric pair they do not exist here at all.
for side in TX RX; do
  eval "host=\${${side}_HOST} bin=\${${side}_BIN} launcher=\${${side}_LAUNCHER}"
  # A containerized launcher resolves its own paths; only check bare-metal ones.
  if [[ "${launcher}" == sudo* ]] && ! on_host "${host}" "test -x '${bin}'"; then
    echo "${side} binary not found or not executable on ${host}: ${bin}" >&2
    echo "Build with -DDAQIRI_BUILD_APPLICATIONS=ON first." >&2
    exit 1
  fi
done

echo "Resolving RX MAC on ${RX_HOST} (${RX_IFACE})..."
RX_MAC="$(on_host "${RX_HOST}" "cat /sys/class/net/${RX_IFACE}/address")"
echo "RX MAC=${RX_MAC}"
echo "TX ${TX_HOST}: ${TX_LAUNCHER} ${TX_BIN} ${TX_CFG}"
echo "RX ${RX_HOST}: ${RX_LAUNCHER} ${RX_BIN} ${RX_CFG}"

RX_LOG="$(mktemp /tmp/resnet-rx.XXXXXX.log)"
TX_LOG="$(mktemp /tmp/resnet-tx.XXXXXX.log)"
cleanup() {
  on_host "${RX_HOST}" "pkill -f daqiri_resnet50_inference || true" 2>/dev/null || true
  on_host "${TX_HOST}" "pkill -f daqiri_resnet50_inference || true" 2>/dev/null || true
}
trap cleanup EXIT

echo "Starting RX on ${RX_HOST}..."
# shellcheck disable=SC2029
on_host "${RX_HOST}" "cd '${RX_REPO}' && ${RX_LAUNCHER} '${RX_BIN}' '${RX_CFG}' --mode rx ${DATASET_ARGS[*]} ${SECONDS_ARGS[*]} ${MODE_ARGS[*]}" \
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
on_host "${TX_HOST}" "cd '${TX_REPO}' && export ETH_DST_ADDR='${RX_MAC}' && ${TX_LAUNCHER} '${TX_BIN}' '${TX_CFG}' --mode tx ${DATASET_ARGS[*]} ${SECONDS_ARGS[*]} ${MODE_ARGS[*]}" \
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
