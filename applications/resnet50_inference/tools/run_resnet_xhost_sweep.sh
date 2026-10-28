#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
# Catalog-driven Spark->Thor DAQIRI-only and end-to-end campaign.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CATALOG_TOOL="${SCRIPT_DIR}/export_resnet_onnx.py"
MODELS=${MODELS:-$(python3 "${CATALOG_TOOL}" --list-models)}
MODES=${MODES:-"e2e"}
SECONDS_PER=${SECONDS_PER:-30}
REPEATS=${REPEATS:-3}
MAX_ATTEMPTS=${MAX_ATTEMPTS:-5}
RESULTS_DIR=${RESULTS_DIR:-${REPO_ROOT}/resnet-results}
OUT=${OUT:-${RESULTS_DIR}/thor_seven_model_raw.csv}
mkdir -p "${RESULTS_DIR}" "$(dirname "${OUT}")"

sv() { awk -F, -v k="$2" '$1==k {print $2; exit}' "$1" 2>/dev/null || true; }
pv() { awk -F= -v k="$2" '$1==k {print $2; exit}' "$1" 2>/dev/null || true; }
na() { [[ -n "$1" ]] && printf '%s' "$1" || printf 'NA'; }

pacing_for() {
  local model="$1" mode="$2"
  if [[ "${mode}" == "daqiri_only" ]]; then
    echo 50000
    return
  fi
  case "${model}" in
    resnet18) echo 23000 ;;
    resnet34) echo 12200 ;;
    resnet50) echo 9500 ;;
    resnet101) echo 5400 ;;
    resnet152) echo 3700 ;;
    mobilenet_v3_small) echo 39000 ;;
    mobilenet_v3_large) echo 25600 ;;
    *) echo "ERROR: no campaign pacing for ${model}" >&2; return 1 ;;
  esac
}

HEADER="model,feature_dim,mode,images_per_batch,seconds,rep,img_s,inference_batches,lat_p50_ms,lat_p99_ms,tx_rc,rx_rc,tx_sent_frames,rx_packets_phy,remainder_dropped,partial_bursts,queue_full,non_reordered_bursts,rx_discards_phy,wire_gbps,wire_pps,clock_sync_ok,tx_to_first_delivery_ms,tx_to_first_result_ms,delivery_to_result_p50_ms,reorder_gpu_kernel_total_us,reorder_gpu_kernel_share_pct,run_id"
[[ -f "${OUT}" ]] || echo "${HEADER}" > "${OUT}"

for model in ${MODELS}; do
  feature_dim="$(python3 "${CATALOG_TOOL}" --model "${model}" --print-field feature_dim)"
  onnx="$(python3 "${CATALOG_TOOL}" --model "${model}" --print-field onnx_path)"
  engine="$(python3 "${CATALOG_TOOL}" --model "${model}" --print-field engine_path)"
  [[ -f "${onnx}" ]] || { echo "ERROR: missing ${onnx}" >&2; exit 1; }
  [[ -f "${engine}" ]] || { echo "ERROR: missing prebuilt ${engine}" >&2; exit 1; }

  for mode in ${MODES}; do
    pacing_mbps="$(pacing_for "${model}" "${mode}")"
    mode_args=()
    [[ "${mode}" == "daqiri_only" ]] && mode_args=(--no-inference)
    for rep in $(seq 1 "${REPEATS}"); do
      accepted=0
      for attempt in $(seq 1 "${MAX_ATTEMPTS}"); do
        run_id="$(date -u +%Y%m%dT%H%M%SZ)-${model}-${mode}-r${rep}-a${attempt}"
        echo "=== ${model} ${mode} rep ${rep}/${REPEATS} attempt ${attempt}/${MAX_ATTEMPTS} ==="
        rc=0
        RUN_ID="${run_id}" RESULTS_DIR="${RESULTS_DIR}" TX_PACING_MBPS="${pacing_mbps}" \
          "${SCRIPT_DIR}/run_xhost_with_phy.sh" --model "${model}" --rep "${rep}" \
          --seconds "${SECONDS_PER}" "${mode_args[@]}" || rc=$?
        sum="${RESULTS_DIR}/${run_id}.summary"
        phy="${RESULTS_DIR}/${run_id}.phy"
        tx_rc="$(sv "${sum}" tx_rc)"; rx_rc="$(sv "${sum}" rx_rc)"
        tx_frames="$(sv "${sum}" tx_sent_frames)"; rx_frames="$(pv "${phy}" rx_packets_phy_delta)"
        pushed_images="$(sv "${sum}" pushed_images)"
        packets_per_image="$(sv "${sum}" packets_per_image)"
        dropped="$(sv "${sum}" dropped_bursts)"
        remainder="$(sv "${sum}" remainder_dropped)"; partial="$(sv "${sum}" partial_bursts)"
        qfull="$(sv "${sum}" queue_full)"; nonreorder="$(sv "${sum}" non_reordered_bursts)"
        discards="$(pv "${phy}" rx_discards_phy_delta)"
        out_of_buffer="$(pv "${phy}" rx_out_of_buffer_delta)"
        buffer_discards="$(pv "${phy}" rx_prio0_buf_discard_delta)"
        counters_match=0
        if [[ "${tx_frames}" =~ ^[0-9]+$ && "${rx_frames}" =~ ^[0-9]+$ && \
              "${pushed_images}" =~ ^[0-9]+$ && "${packets_per_image}" =~ ^[0-9]+$ ]]; then
          phy_delta=$((rx_frames - tx_frames))
          if ((phy_delta < 0)); then phy_delta=$((-phy_delta)); fi
          # The port-wide physical counter may include a few unrelated link
          # packets. The flow-matched application count must remain exact.
          if ((phy_delta <= 64 && pushed_images * packets_per_image == tx_frames)); then
            counters_match=1
          fi
        fi
        if [[ "${rc}" == 0 && "${tx_rc}" == 0 && "${rx_rc}" == 0 && \
              "${counters_match}" == 1 && \
              "${dropped}" == 0 && \
              "${remainder}" == 0 && \
              "${partial}" == 0 && "${qfull}" == 0 && "${nonreorder}" == 0 && \
              "${discards}" == 0 && "${out_of_buffer}" == 0 && "${buffer_discards}" == 0 ]]; then
          accepted=1
          printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${model}" "${feature_dim}" "${mode}" "$(na "$(sv "${sum}" images_per_batch)")" \
            "${SECONDS_PER}" "${rep}" "$(na "$(sv "${sum}" img_s)")" \
            "$(na "$(sv "${sum}" inference_batches)")" "$(na "$(sv "${sum}" latency_p50_ms)")" \
            "$(na "$(sv "${sum}" latency_p99_ms)")" "${tx_rc}" "${rx_rc}" "${tx_frames}" \
            "${rx_frames}" "${remainder}" "${partial}" "${qfull}" "${nonreorder}" "${discards}" \
            "$(na "$(pv "${phy}" wire_gbps_over_tx_active)")" "$(na "$(pv "${phy}" wire_pps_over_tx_active)")" \
            "$(na "$(sv "${sum}" clock_sync_ok)")" "$(na "$(sv "${sum}" tx_start_to_first_delivery_ms)")" \
            "$(na "$(sv "${sum}" tx_start_to_first_result_ms)")" "$(na "$(sv "${sum}" delivery_to_result_p50_ms)")" \
            "$(na "$(sv "${sum}" reorder_gpu_kernel_total_us)")" \
            "$(na "$(sv "${sum}" reorder_gpu_kernel_share_pct)")" "${run_id}" >> "${OUT}"
          break
        fi
        echo "REJECT ${run_id}: rc=${rc} tx_rc=${tx_rc:-NA} rx_rc=${rx_rc:-NA} tx/rx=${tx_frames:-NA}/${rx_frames:-NA} pushed_images=${pushed_images:-NA} packets_per_image=${packets_per_image:-NA} dropped=${dropped:-NA} remainder=${remainder:-NA} partial=${partial:-NA} queue_full=${qfull:-NA} non_reordered=${nonreorder:-NA} discards=${discards:-NA} out_of_buffer=${out_of_buffer:-NA} buffer_discards=${buffer_discards:-NA}" >&2
      done
      [[ "${accepted}" == 1 ]] || { echo "ERROR: no accepted run for ${model} ${mode} rep ${rep}" >&2; exit 1; }
    done
  done
done

echo "Accepted cross-host rows: $(( $(wc -l < "${OUT}") - 1 )) in ${OUT}"
