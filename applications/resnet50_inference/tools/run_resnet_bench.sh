#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Sweep ResNet-18/34/50/101/152 inference throughput in the DAQIRI receive path
# and emit a CSV. Platform-agnostic: drive it via env vars so the same script
# runs on DGX Spark, IGX, or an RTX Pro server. Physical wire loopback only
# (synthetic benchmark mode): the dq_wire_* netns must be DOWN and ETH_DST_ADDR
# must be exported (the RX port MAC), exactly like the raw/DPDK bench.
#
#   export ETH_DST_ADDR=$(cat /sys/class/net/<rx-iface>/address)
#   BUILD_DIR=./build ./tools/run_resnet_bench.sh
#
# Env knobs: BUILD_DIR, BIN, CONFIG, MODELS, SECONDS_PER, IMAGES_PER_BATCH,
#            REPEATS, OUT, MODEL_DIR.
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-./build}
APP_DIR="${BUILD_DIR}/applications/resnet50_inference"
BIN=${BIN:-${APP_DIR}/daqiri_resnet50_inference}
CONFIG=${CONFIG:-${APP_DIR}/configs/resnet50_bench_spark.yaml}
MODELS=${MODELS:-"resnet18 resnet34 resnet50 resnet101 resnet152"}
SECONDS_PER=${SECONDS_PER:-30}
IMAGES_PER_BATCH=${IMAGES_PER_BATCH:-32}
REPEATS=${REPEATS:-3}
MODEL_DIR=${MODEL_DIR:-models}
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

declare -A FEATURE_DIM=( [resnet18]=512 [resnet34]=512 [resnet50]=2048 \
                         [resnet101]=2048 [resnet152]=2048 )

echo "model,feature_dim,images_per_batch,seconds,rep,img_s,inference_batches,lat_p50_ms,lat_p99_ms,rx_drops" > "${OUT}"

for model in ${MODELS}; do
  onnx="${MODEL_DIR}/${model}_features.onnx"
  if [[ ! -f "${onnx}" ]]; then
    echo "Exporting ONNX for ${model} ..."
    python3 "$(dirname "$0")/export_resnet_onnx.py" --model "${model}" --output "${onnx}"
  fi
  for rep in $(seq 1 "${REPEATS}"); do
    echo "=== ${model} rep ${rep}/${REPEATS} ==="
    log=$(mktemp)
    # First rep builds + caches the engine; later reps load from cache.
    "${BIN}" "${CONFIG}" --model "${model}" --images-per-batch "${IMAGES_PER_BATCH}" \
      --seconds "${SECONDS_PER}" 2>&1 | tee "${log}" || true
    # `|| true` on each: grep exits non-zero when a field is absent (e.g. a
    # drop-free run has no "total:" line), which under `set -euo pipefail` would
    # otherwise abort the whole sweep at the assignment.
    img_s=$(grep -oE '=> [0-9.]+ img/s' "${log}" | grep -oE '[0-9.]+' | head -1 || true)
    batches=$(grep -oE '[0-9]+ inference batches' "${log}" | grep -oE '[0-9]+' | head -1 || true)
    lat_p50=$(grep -oE 'p50=[0-9.]+' "${log}" | head -1 | cut -d= -f2 || true)
    lat_p99=$(grep -oE 'p99=[0-9.]+' "${log}" | head -1 | cut -d= -f2 || true)
    # Cumulative RX drop total from the periodic drop logger; absent => 0.
    drops=$(grep -oE 'total: [0-9]+' "${log}" | grep -oE '[0-9]+' | tail -1 || true)
    echo "${model},${FEATURE_DIM[$model]:-2048},${IMAGES_PER_BATCH},${SECONDS_PER},${rep},${img_s:-NA},${batches:-NA},${lat_p50:-NA},${lat_p99:-NA},${drops:-0}" >> "${OUT}"
    rm -f "${log}"
  done
done

echo "Wrote ${OUT}"
column -s, -t "${OUT}" || cat "${OUT}"
