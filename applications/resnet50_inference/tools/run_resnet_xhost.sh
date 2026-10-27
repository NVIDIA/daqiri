#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Cross-host vision-model runner. Host paths, launchers, and configs are
# overrideable so asymmetric Spark->Thor runs use the same code as Spark pairs.
#
# Usage:
#   ./tools/run_resnet_xhost.sh [--replay-once|--seconds N] [--dataset PATH]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${APP_DIR}/../.." && pwd)"

[[ -n "${TX_ENV:-}" ]] && source "${TX_ENV}"
[[ -n "${RX_ENV:-}" ]] && source "${RX_ENV}"

on_host() {
  local host="$1"; shift
  case "${host}" in
    localhost|127.0.0.1|"") bash -c "$*" ;;
    *) ssh -o BatchMode=yes "${host}" "$*" ;;
  esac
}

TX_HOST="${TX_HOST:-spark-stacked-01}"
RX_HOST="${RX_HOST:-spark-stacked-02}"
RX_IFACE="${RX_IFACE:-det1}"
TX_REPO="${TX_REPO:-${REPO_ROOT}}"
RX_REPO="${RX_REPO:-${REPO_ROOT}}"
BUILD_DIR="${BUILD_DIR:-build-resnet}"
TX_BUILD="${TX_BUILD:-${TX_REPO}/${BUILD_DIR#/}}"
RX_BUILD="${RX_BUILD:-${RX_REPO}/${BUILD_DIR#/}}"
APP_SUBDIR="${APP_SUBDIR:-applications/resnet50_inference}"
APP_BIN="${APP_BIN:-daqiri_resnet50_inference}"
TX_BIN="${TX_BIN:-${TX_BUILD}/${APP_SUBDIR}/${APP_BIN}}"
RX_BIN="${RX_BIN:-${RX_BUILD}/${APP_SUBDIR}/${APP_BIN}}"
TX_CFG="${TX_CFG:-${TX_BUILD}/${APP_SUBDIR}/configs/resnet50_tx_spark_xhost.yaml}"
RX_CFG="${RX_CFG:-${RX_BUILD}/${APP_SUBDIR}/configs/resnet50_rx_spark_xhost.yaml}"
TX_LAUNCHER="${TX_LAUNCHER:-sudo -E}"
RX_LAUNCHER="${RX_LAUNCHER:-sudo}"
CATALOG_TOOL="${SCRIPT_DIR}/export_resnet_onnx.py"

RESULTS_DIR="${RESULTS_DIR:-${REPO_ROOT}/resnet-results}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RUN_SECONDS=""
MODEL="${MODEL:-resnet50}"
PASS_MODEL="${PASS_MODEL:-1}"
TX_PACING_MBPS="${TX_PACING_MBPS:-}"
REP="${REP:-}"
RX_NO_INFERENCE="${RX_NO_INFERENCE:-0}"
CLOCK_SYNC_MAX_MS="${CLOCK_SYNC_MAX_MS:-50}"

MODE_ARGS=(--replay-once)
DATASET_ARGS=()
SECONDS_ARGS=()
RX_SECONDS_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --replay-once) MODE_ARGS=(--replay-once); shift ;;
    --loop) MODE_ARGS=(--loop); shift ;;
    --seconds) SECONDS_ARGS=(--seconds "$2"); RX_SECONDS_ARGS=(--seconds "$(( $2 + 15 ))")
               MODE_ARGS=(--loop); RUN_SECONDS="$2"; shift 2 ;;
    --results-dir) RESULTS_DIR="$2"; shift 2 ;;
    --run-id) RUN_ID="$2"; shift 2 ;;
    --dataset) DATASET_ARGS=(--dataset "$2"); shift 2 ;;
    --build-dir) TX_BUILD="${TX_REPO}/${2#/}"; RX_BUILD="${RX_REPO}/${2#/}"
                 TX_BIN="${TX_BUILD}/${APP_SUBDIR}/${APP_BIN}"
                 RX_BIN="${RX_BUILD}/${APP_SUBDIR}/${APP_BIN}"
                 shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --rep) REP="$2"; shift 2 ;;
    --no-inference) RX_NO_INFERENCE=1; shift ;;
    --tx-cfg) TX_CFG="$2"; shift 2 ;;
    --rx-cfg) RX_CFG="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

RX_MODEL_ARGS=()
[[ "${PASS_MODEL}" == "1" ]] && RX_MODEL_ARGS=(--model "${MODEL}")
# TX packetization is identical across catalog models. Omitting --model also
# lets a generated pacing config live outside the application config directory.
TX_MODEL_ARGS=()
NOINF_ARGS=()
[[ "${RX_NO_INFERENCE}" == "1" ]] && NOINF_ARGS=(--no-inference)
FEATURE_DIM="$(python3 "${CATALOG_TOOL}" --model "${MODEL}" --print-field feature_dim)"
MODEL_ONNX="$(python3 "${CATALOG_TOOL}" --model "${MODEL}" --print-field onnx_path)"
MODEL_ENGINE="$(python3 "${CATALOG_TOOL}" --model "${MODEL}" --print-field engine_path)"

clock_sample() {
  local host="$1" t0 remote t1 midpoint remote_ns ntp
  t0="$(date +%s%N)"
  remote="$(on_host "${host}" "date +%s%N; timedatectl show -p NTPSynchronized --value 2>/dev/null || echo unknown")"
  t1="$(date +%s%N)"
  remote_ns="$(printf '%s\n' "${remote}" | sed -n '1p')"
  ntp="$(printf '%s\n' "${remote}" | sed -n '2p')"
  midpoint=$(( (t0 + t1) / 2 ))
  printf '%s,%s,%s\n' "$((remote_ns - midpoint))" "${ntp}" "$((t1 - t0))"
}

IFS=, read -r TX_CLOCK_SKEW_NS TX_NTP_SYNC TX_CLOCK_RTT_NS < <(clock_sample "${TX_HOST}")
IFS=, read -r RX_CLOCK_SKEW_NS RX_NTP_SYNC RX_CLOCK_RTT_NS < <(clock_sample "${RX_HOST}")
CLOCK_PAIR_SKEW_MS="$(awk -v tx="${TX_CLOCK_SKEW_NS}" -v rx="${RX_CLOCK_SKEW_NS}" \
  'BEGIN { d=(tx-rx)/1e6; if (d<0) d=-d; printf "%.3f", d }')"
CLOCK_SYNC_OK="$(awk -v d="${CLOCK_PAIR_SKEW_MS}" -v max="${CLOCK_SYNC_MAX_MS}" \
  -v tx="${TX_NTP_SYNC}" -v rx="${RX_NTP_SYNC}" \
  'BEGIN { print (tx=="yes" && rx=="yes" && d<=max) ? 1 : 0 }')"

echo "Resolving RX MAC on ${RX_HOST} (${RX_IFACE})..."
RX_MAC="$(on_host "${RX_HOST}" "cat /sys/class/net/${RX_IFACE}/address")"
echo "RX MAC=${RX_MAC}"
echo "Clock preflight: synchronized=${CLOCK_SYNC_OK} estimated_pair_skew_ms=${CLOCK_PAIR_SKEW_MS}"
echo "TX ${TX_HOST}: ${TX_LAUNCHER} ${TX_BIN} ${TX_CFG}"
echo "RX ${RX_HOST}: ${RX_LAUNCHER} ${RX_BIN} ${RX_CFG}"

TX_RUN_CFG="${TX_CFG}"
TX_GENERATED_CFG=""
if [[ -n "${TX_PACING_MBPS}" ]]; then
  tx_source_host="${TX_CFG/#\/workspace\/daqiri/${TX_REPO}}"
  tx_generated_host="/tmp/${RUN_ID}.tx.yaml"
  on_host "${TX_HOST}" "sed -E 's/^([[:space:]]*)pacing_mbps:.*/\\1pacing_mbps: ${TX_PACING_MBPS}/' '${tx_source_host}' > '${tx_generated_host}'"
  TX_RUN_CFG="/campaign/${RUN_ID}.tx.yaml"
  TX_GENERATED_CFG="${tx_generated_host}"
  echo "TX pacing: ${TX_PACING_MBPS} Mb/s per path (${TX_RUN_CFG})"
fi

RX_LOG="$(mktemp /tmp/resnet-rx.XXXXXX.log)"
TX_LOG="$(mktemp /tmp/resnet-tx.XXXXXX.log)"
cleanup() {
  on_host "${RX_HOST}" "pkill -f '${APP_BIN}' || true" 2>/dev/null || true
  on_host "${TX_HOST}" "pkill -f '${APP_BIN}' || true" 2>/dev/null || true
  [[ -z "${TX_GENERATED_CFG}" ]] || on_host "${TX_HOST}" "rm -f -- '${TX_GENERATED_CFG}'" || true
}
trap cleanup EXIT

echo "Starting RX on ${RX_HOST}..."
# shellcheck disable=SC2029
on_host "${RX_HOST}" "cd '${RX_REPO}' && ${RX_LAUNCHER} '${RX_BIN}' '${RX_CFG}' --mode rx ${DATASET_ARGS[*]} ${RX_SECONDS_ARGS[*]} ${MODE_ARGS[*]} ${RX_MODEL_ARGS[*]} ${NOINF_ARGS[*]}" \
  >"${RX_LOG}" 2>&1 &
RX_SSH_PID=$!

echo "Waiting for RX reorder and consumer ready..."
for _ in $(seq 1 600); do
  if grep -q "RX reorder ready" "${RX_LOG}" 2>/dev/null && \
     grep -qE "TrtRunner ready|no-inference mode" "${RX_LOG}" 2>/dev/null; then
    break
  fi
  if ! kill -0 "${RX_SSH_PID}" 2>/dev/null; then
    echo "RX exited early; log tail:" >&2
    tail -n "${LOG_TAIL_LINES:-200}" "${RX_LOG}" >&2
    exit 1
  fi
  sleep 1
done

if ! grep -q "RX reorder ready" "${RX_LOG}" 2>/dev/null || \
   ! grep -qE "TrtRunner ready|no-inference mode" "${RX_LOG}" 2>/dev/null; then
  echo "Timed out waiting for RX ready; log tail:" >&2
  tail -n "${LOG_TAIL_LINES:-200}" "${RX_LOG}" >&2
  exit 1
fi

echo "Starting TX on ${TX_HOST}..."
# `|| TX_RC=$?` is required: under `set -e` a bare failing ssh terminates the
# script before the log dump below, which is exactly when the logs are needed.
TX_RC=0
# shellcheck disable=SC2029
on_host "${TX_HOST}" "cd '${TX_REPO}' && export ETH_DST_ADDR='${RX_MAC}' && ${TX_LAUNCHER} '${TX_BIN}' '${TX_RUN_CFG}' --mode tx ${DATASET_ARGS[*]} ${SECONDS_ARGS[*]} ${MODE_ARGS[*]} ${TX_MODEL_ARGS[*]}" \
  >"${TX_LOG}" 2>&1 || TX_RC=$?

echo "Waiting for RX to finish..."
RX_RC=0
wait "${RX_SSH_PID}" || RX_RC=$?

echo "=== TX log (${TX_LOG}) ==="
tail -n "${LOG_TAIL_LINES:-200}" "${TX_LOG}"
echo "=== RX log (${RX_LOG}) ==="
tail -n "${LOG_TAIL_LINES:-200}" "${RX_LOG}"

mkdir -p "${RESULTS_DIR}"

TX_START_NS="$(grep -oE 'tx_start unix_ns=[0-9]+' "${TX_LOG}" | cut -d= -f2 | sort -n | head -1 || true)"
# The two TX workers can interleave their final stdout lines.  The per-queue
# ibverbs statistics are emitted serially and report the same posted-frame
# total, so use those for the campaign count.
TX_SENT_FRAMES="$(grep -oE 'ibverbs TX port [0-9]+ q[0-9]+ \([^)]*\): posted=[0-9]+' "${TX_LOG}" |
  sed -E 's/.*posted=//' | awk '{s += $1} END {print s+0}')"
TX_ACTIVE_SECONDS="$(grep -oE 'frames in [0-9.]+ s' "${TX_LOG}" | awk '{print $3}' | sort -nr | head -1 || true)"
[[ -n "${TX_ACTIVE_SECONDS}" ]] || TX_ACTIVE_SECONDS="${RUN_SECONDS}"
FIRST_DELIVERY_NS="$(grep -oE 'batch_delivered index=0 unix_ns=[0-9]+' "${RX_LOG}" | head -1 | sed -E 's/.*=//' || true)"
FIRST_RESULT_NS="$(grep -oE 'first_application_result unix_ns=[0-9]+' "${RX_LOG}" | head -1 | cut -d= -f2 || true)"
TX_TO_DELIVERY_MS=NA
TX_TO_RESULT_MS=NA
if [[ "${CLOCK_SYNC_OK}" == "1" && -n "${TX_START_NS}" ]]; then
  [[ -n "${FIRST_DELIVERY_NS}" ]] && TX_TO_DELIVERY_MS="$(awk -v a="${TX_START_NS}" -v b="${FIRST_DELIVERY_NS}" 'BEGIN {printf "%.3f", (b-a)/1e6}')"
  [[ -n "${FIRST_RESULT_NS}" ]] && TX_TO_RESULT_MS="$(awk -v a="${TX_START_NS}" -v b="${FIRST_RESULT_NS}" 'BEGIN {printf "%.3f", (b-a)/1e6}')"
fi
ACTIVE_SECONDS="$(grep -E '^active_seconds=' "${RX_LOG}" | tail -1 | sed -E 's/^active_seconds=([0-9.]+).*/\1/' || true)"
RX_REPORTED_IMG_S="$(grep -E '^active_seconds=' "${RX_LOG}" | tail -1 | sed -E 's/.* => ([0-9.]+) img\/s.*/\1/' || true)"
PUSHED_IMAGES="$(grep -oE 'pushed_images=[0-9]+' "${RX_LOG}" | head -1 | cut -d= -f2 || true)"
CAMPAIGN_IMG_S=NA
if [[ -n "${PUSHED_IMAGES}" && -n "${TX_ACTIVE_SECONDS}" ]]; then
  CAMPAIGN_IMG_S="$(awk -v n="${PUSHED_IMAGES}" -v s="${TX_ACTIVE_SECONDS}" 'BEGIN {if(s>0) printf "%.2f", n/s; else print "NA"}')"
fi
KERNEL_TOTAL_US="$(grep -E 'ibverbs reorder GPU profile' "${RX_LOG}" | tail -1 | grep -oE 'total_kernel_us=[0-9.]+' | cut -d= -f2 || true)"
KERNEL_SHARE_PCT=NA
if [[ -n "${ACTIVE_SECONDS}" && -n "${KERNEL_TOTAL_US}" ]]; then
  KERNEL_SHARE_PCT="$(awk -v us="${KERNEL_TOTAL_US}" -v s="${ACTIVE_SECONDS}" 'BEGIN {if(s>0) printf "%.6f", us/(s*1e6)*100; else print "NA"}')"
fi
CONVERSION_TOTAL_US="$(grep -E '^post-reorder INT8->FP16 profile:' "${RX_LOG}" | tail -1 | grep -oE 'total_kernel_us=[0-9.eE+-]+' | cut -d= -f2 || true)"
CONVERSION_MEAN_US="$(grep -E '^post-reorder INT8->FP16 profile:' "${RX_LOG}" | tail -1 | grep -oE 'mean_kernel_us=[0-9.eE+-]+' | cut -d= -f2 || true)"
CONVERSION_SHARE_PCT="$(grep -E '^post-reorder INT8->FP16 profile:' "${RX_LOG}" | tail -1 | grep -oE 'active_share_pct=[0-9.eE+-]+' | cut -d= -f2 || true)"
REORDER_ENGINE="$(grep -oE 'RX reorder ready \([^,]+, engine=[^)]+\)' "${RX_LOG}" | head -1 | sed -E 's/.*engine=([^)]+).*/\1/' || true)"

rx_counter() {
  grep -oE "$1=[0-9]+" "${RX_LOG}" | head -1 | cut -d= -f2 || true
}

{
  echo "run_id,${RUN_ID}"
  echo "engine,ibverbs"
  echo "reorder_engine,${REORDER_ENGINE}"
  echo "rx_host,${RX_HOST}"
  echo "tx_host,${TX_HOST}"
  echo "rx_config,${RX_CFG}"
  echo "tx_config,${TX_CFG}"
  echo "tx_pacing_mbps_per_path,${TX_PACING_MBPS}"
  echo "seconds_requested,${RUN_SECONDS:-}"
  echo "tx_rc,${TX_RC}"
  echo "rx_rc,${RX_RC}"
  echo "model,${MODEL}"
  echo "feature_dim,${FEATURE_DIM}"
  echo "model_onnx,${MODEL_ONNX}"
  echo "model_engine,${MODEL_ENGINE}"
  echo "rep,${REP}"
  echo "no_inference,${RX_NO_INFERENCE}"
  [[ "${RX_NO_INFERENCE}" == "1" ]] && echo "mode,daqiri_only" || echo "mode,e2e"
  grep -oE 'images_per_batch=[0-9]+' "${RX_LOG}" | head -1 | sed 's/=/,/' || true
  grep -oE 'packets_per_image=[0-9]+' "${RX_LOG}" | head -1 | sed 's/=/,/' || true
  echo "active_seconds,${ACTIVE_SECONDS}"
  echo "rx_reported_img_s,${RX_REPORTED_IMG_S}"
  echo "tx_active_seconds,${TX_ACTIVE_SECONDS}"
  echo "img_s,${CAMPAIGN_IMG_S}"
  grep -E "^inference_consumer_worker: [0-9]+ inference batches" "${RX_LOG}" |
    sed -E 's/^inference_consumer_worker: ([0-9]+) inference batches in ([0-9.]+) s.*/inference_batches,\1\nelapsed_s,\2/' || true
  grep -E "^inference latency \(ms\)" "${RX_LOG}" |
    sed -E 's/.*mean=([0-9.]+) p50=([0-9.]+) p99=([0-9.]+) .*n=([0-9]+).*/latency_mean_ms,\1\nlatency_p50_ms,\2\nlatency_p99_ms,\3\nlatency_samples,\4/' || true
  grep -E '^no-inference reorder wait \(ms\)' "${RX_LOG}" |
    sed -E 's/.*mean=([0-9.]+) p50=([0-9.]+) p99=([0-9.]+).*/reorder_wait_mean_ms,\1\nreorder_wait_p50_ms,\2\nreorder_wait_p99_ms,\3/' || true
  grep -E '^batch delivery->inference submit' "${RX_LOG}" |
    sed -E 's/.*first=([0-9.]+) mean=([0-9.]+) p50=([0-9.]+) p99=([0-9.]+).*/delivery_to_submit_first_us,\1\ndelivery_to_submit_mean_us,\2\ndelivery_to_submit_p50_us,\3\ndelivery_to_submit_p99_us,\4/' || true
  grep -E '^batch delivery->application-visible result' "${RX_LOG}" |
    sed -E 's/.*first=([0-9.]+) mean=([0-9.]+) p50=([0-9.]+) p99=([0-9.]+).*/delivery_to_result_first_ms,\1\ndelivery_to_result_mean_ms,\2\ndelivery_to_result_p50_ms,\3\ndelivery_to_result_p99_ms,\4/' || true
  echo "clock_sync_ok,${CLOCK_SYNC_OK}"
  echo "clock_pair_skew_ms,${CLOCK_PAIR_SKEW_MS}"
  echo "tx_start_unix_ns,${TX_START_NS:-NA}"
  echo "tx_sent_frames,${TX_SENT_FRAMES:-0}"
  echo "first_delivery_unix_ns,${FIRST_DELIVERY_NS:-NA}"
  echo "first_result_unix_ns,${FIRST_RESULT_NS:-NA}"
  echo "tx_start_to_first_delivery_ms,${TX_TO_DELIVERY_MS}"
  echo "tx_start_to_first_result_ms,${TX_TO_RESULT_MS}"
  echo "delivered_bursts,$(rx_counter delivered_bursts)"
  echo "pushed_images,${PUSHED_IMAGES}"
  echo "dropped_bursts,$(rx_counter dropped_bursts)"
  echo "queue_full,$(rx_counter queue_full)"
  echo "partial_bursts,$(rx_counter partial_bursts)"
  echo "partial_packets,$(rx_counter partial_packets)"
  echo "remainder_dropped,$(rx_counter remainder_dropped)"
  echo "non_reordered_bursts,$(rx_counter non_reordered_bursts)"
  echo "reorder_gpu_kernel_total_us,${KERNEL_TOTAL_US:-0}"
  echo "reorder_gpu_kernel_share_pct,${KERNEL_SHARE_PCT}"
  echo "post_reorder_conversion_total_us,${CONVERSION_TOTAL_US:-0}"
  echo "post_reorder_conversion_mean_us,${CONVERSION_MEAN_US:-0}"
  echo "post_reorder_conversion_share_pct,${CONVERSION_SHARE_PCT:-0}"
  grep -E "ibverbs reorder GPU profile" "${RX_LOG}" | tail -1 || true
} >"${RESULTS_DIR}/${RUN_ID}.summary"

# Per-interval throughput, for locating the warmup knee and checking the rate
# held rather than decayed.
grep -E "^inference_consumer_worker: t=" "${RX_LOG}" >"${RESULTS_DIR}/${RUN_ID}.timeseries" || true

cp "${RX_LOG}" "${RESULTS_DIR}/${RUN_ID}.rx.log"
cp "${TX_LOG}" "${RESULTS_DIR}/${RUN_ID}.tx.log"
echo "=== results written to ${RESULTS_DIR}/${RUN_ID}.* ==="
cat "${RESULTS_DIR}/${RUN_ID}.summary"

if [[ "${TX_RC}" != "0" ]]; then exit "${TX_RC}"; fi
exit "${RX_RC}"
