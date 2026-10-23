#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
# Catalog-driven Spark->Thor DAQIRI-only and end-to-end campaign.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CATALOG_TOOL="${SCRIPT_DIR}/export_resnet_onnx.py"
MODELS=${MODELS:-$(python3 "${CATALOG_TOOL}" --list-models)}
MODES=${MODES:-"daqiri_only e2e"}
SECONDS_PER=${SECONDS_PER:-30}
REPEATS=${REPEATS:-3}
MAX_ATTEMPTS=${MAX_ATTEMPTS:-5}
RESULTS_DIR=${RESULTS_DIR:-${REPO_ROOT}/resnet-results}
OUT=${OUT:-${RESULTS_DIR}/thor_seven_model_raw.csv}
mkdir -p "${RESULTS_DIR}" "$(dirname "${OUT}")"

sv() { awk -F, -v k="$2" '$1==k {print $2; exit}' "$1" 2>/dev/null || true; }
pv() { awk -F= -v k="$2" '$1==k {print $2; exit}' "$1" 2>/dev/null || true; }
na() { [[ -n "$1" ]] && printf '%s' "$1" || printf 'NA'; }

HEADER="model,feature_dim,mode,images_per_batch,seconds,rep,img_s,inference_batches,lat_p50_ms,lat_p99_ms,tx_rc,rx_rc,tx_sent_frames,rx_packets_phy,remainder_dropped,partial_bursts,queue_full,non_reordered_bursts,rx_discards_phy,wire_gbps,wire_pps,clock_sync_ok,tx_to_first_delivery_ms,tx_to_first_result_ms,delivery_to_result_p50_ms,reorder_gpu_kernel_total_us,reorder_gpu_kernel_share_pct,run_id"
[[ -f "${OUT}" ]] || echo "${HEADER}" > "${OUT}"

for model in ${MODELS}; do
  feature_dim="$(python3 "${CATALOG_TOOL}" --model "${model}" --print-field feature_dim)"
  onnx="$(python3 "${CATALOG_TOOL}" --model "${model}" --print-field onnx_path)"
  engine="$(python3 "${CATALOG_TOOL}" --model "${model}" --print-field engine_path)"
  [[ -f "${onnx}" ]] || { echo "ERROR: missing ${onnx}" >&2; exit 1; }
  [[ -f "${engine}" ]] || { echo "ERROR: missing prebuilt ${engine}" >&2; exit 1; }

  for mode in ${MODES}; do
    mode_args=()
    [[ "${mode}" == "daqiri_only" ]] && mode_args=(--no-inference)
    for rep in $(seq 1 "${REPEATS}"); do
      accepted=0
      for attempt in $(seq 1 "${MAX_ATTEMPTS}"); do
        run_id="$(date -u +%Y%m%dT%H%M%SZ)-${model}-${mode}-r${rep}-a${attempt}"
        echo "=== ${model} ${mode} rep ${rep}/${REPEATS} attempt ${attempt}/${MAX_ATTEMPTS} ==="
        rc=0
        RUN_ID="${run_id}" RESULTS_DIR="${RESULTS_DIR}" \
          "${SCRIPT_DIR}/run_xhost_with_phy.sh" --model "${model}" --rep "${rep}" \
          --seconds "${SECONDS_PER}" "${mode_args[@]}" || rc=$?
        sum="${RESULTS_DIR}/${run_id}.summary"
        phy="${RESULTS_DIR}/${run_id}.phy"
        tx_rc="$(sv "${sum}" tx_rc)"; rx_rc="$(sv "${sum}" rx_rc)"
        tx_frames="$(sv "${sum}" tx_sent_frames)"; rx_frames="$(pv "${phy}" rx_packets_phy_delta)"
        pushed_images="$(sv "${sum}" pushed_images)"
        remainder="$(sv "${sum}" remainder_dropped)"; partial="$(sv "${sum}" partial_bursts)"
        qfull="$(sv "${sum}" queue_full)"; nonreorder="$(sv "${sum}" non_reordered_bursts)"
        discards="$(pv "${phy}" rx_discards_phy_delta)"
        if [[ "${rc}" == 0 && "${tx_rc}" == 0 && "${rx_rc}" == 0 && \
              "${tx_frames}" == "${rx_frames}" && "$((pushed_images * 128))" == "${rx_frames}" && \
              "${remainder}" == 0 && \
              "${partial}" == 0 && "${qfull}" == 0 && "${nonreorder}" == 0 && \
              "${discards}" == 0 ]]; then
          accepted=1
          printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${model}" "${feature_dim}" "${mode}" "$(na "$(sv "${sum}" images_per_batch)")" \
            "${SECONDS_PER}" "${rep}" "$(na "$(sv "${sum}" img_s)")" \
            "$(na "$(sv "${sum}" inference_batches)")" "$(na "$(sv "${sum}" latency_p50_ms)")" \
            "$(na "$(sv "${sum}" latency_p99_ms)")" "${tx_rc}" "${rx_rc}" "${tx_frames}" \
            "${rx_frames}" "${remainder}" "${partial}" "${qfull}" "${nonreorder}" "${discards}" \
            "$(na "$(pv "${phy}" wire_gbps_over_wall)")" "$(na "$(pv "${phy}" wire_pps_over_wall)")" \
            "$(na "$(sv "${sum}" clock_sync_ok)")" "$(na "$(sv "${sum}" tx_start_to_first_delivery_ms)")" \
            "$(na "$(sv "${sum}" tx_start_to_first_result_ms)")" "$(na "$(sv "${sum}" delivery_to_result_p50_ms)")" \
            "$(na "$(sv "${sum}" reorder_gpu_kernel_total_us)")" \
            "$(na "$(sv "${sum}" reorder_gpu_kernel_share_pct)")" "${run_id}" >> "${OUT}"
          break
        fi
        echo "REJECT ${run_id}: rc=${rc} tx_rc=${tx_rc:-NA} rx_rc=${rx_rc:-NA} tx/rx=${tx_frames:-NA}/${rx_frames:-NA} pushed_images=${pushed_images:-NA} remainder=${remainder:-NA} partial=${partial:-NA} queue_full=${qfull:-NA} non_reordered=${nonreorder:-NA} discards=${discards:-NA}" >&2
      done
      [[ "${accepted}" == 1 ]] || { echo "ERROR: no accepted run for ${model} ${mode} rep ${rep}" >&2; exit 1; }
    done
  done
done

echo "Accepted cross-host rows: $(( $(wc -l < "${OUT}") - 1 )) in ${OUT}"
