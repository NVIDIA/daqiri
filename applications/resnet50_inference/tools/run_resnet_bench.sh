#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Sweep catalogued ResNet and MobileNetV3 inference throughput through the
# ibverbs raw-engine path and emit a CSV. Use a real cabled loopback only.
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-./build}
APP_DIR="${BUILD_DIR}/applications/resnet50_inference"
BIN=${BIN:-${APP_DIR}/daqiri_resnet50_inference}
CONFIG=${CONFIG:-${APP_DIR}/configs/resnet50_bench_spark.yaml}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPORTER="${SCRIPT_DIR}/export_resnet_onnx.py"
MODELS=${MODELS:-$(python3 "${EXPORTER}" --list-models)}
SECONDS_PER=${SECONDS_PER:-30}
IMAGES_PER_BATCH=${IMAGES_PER_BATCH:-32}
REPEATS=${REPEATS:-3}
OUT=${OUT:-resnet-bench.csv}

if [[ -z "${ETH_DST_ADDR:-}" ]]; then
  echo "ETH_DST_ADDR not set. Export the RX port MAC, e.g.:" >&2
  echo "  export ETH_DST_ADDR=\$(cat /sys/class/net/<rx-iface>/address)" >&2
  exit 1
fi
if [[ ! -x "${BIN}" ]]; then
  echo "Binary not found: ${BIN} (build with -DDAQIRI_BUILD_APPLICATIONS=ON)" >&2
  exit 1
fi

echo "model,feature_dim,images_per_batch,seconds,rep,img_s,inference_batches,lat_p50_ms,lat_p99_ms,rx_drops" > "${OUT}"

for model in ${MODELS}; do
  onnx="$(python3 "${EXPORTER}" --model "${model}" --print-field onnx_path)"
  feature_dim="$(python3 "${EXPORTER}" --model "${model}" --print-field feature_dim)"
  if [[ ! -f "${onnx}" ]]; then
    echo "Exporting ONNX for ${model} ..."
    python3 "${EXPORTER}" --model "${model}" --output "${onnx}"
  fi
  for rep in $(seq 1 "${REPEATS}"); do
    echo "=== ${model} rep ${rep}/${REPEATS} (ibverbs) ==="
    log=$(mktemp)
    "${BIN}" "${CONFIG}" --model "${model}" --images-per-batch "${IMAGES_PER_BATCH}" \
      --seconds "${SECONDS_PER}" 2>&1 | tee "${log}" || true
    img_s=$(grep -oE '=> [0-9.]+ img/s' "${log}" | grep -oE '[0-9.]+' | head -1 || true)
    batches=$(grep -oE '[0-9]+ inference batches' "${log}" | grep -oE '[0-9]+' | head -1 || true)
    lat_p50=$(grep -oE 'p50=[0-9.]+' "${log}" | head -1 | cut -d= -f2 || true)
    lat_p99=$(grep -oE 'p99=[0-9.]+' "${log}" | head -1 | cut -d= -f2 || true)
    drops=$(grep -oE 'total: [0-9]+' "${log}" | grep -oE '[0-9]+' | tail -1 || true)
    echo "${model},${feature_dim},${IMAGES_PER_BATCH},${SECONDS_PER},${rep},${img_s:-NA},${batches:-NA},${lat_p50:-NA},${lat_p99:-NA},${drops:-0}" >> "${OUT}"
    rm -f "${log}"
  done
done

echo "Wrote ${OUT}"
column -s, -t "${OUT}" || cat "${OUT}"
