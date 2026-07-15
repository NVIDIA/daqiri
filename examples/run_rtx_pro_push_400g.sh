#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# RTX PRO 6000 — push toward 400 Gbps using the documented levers:
#   1) MQ 2t2r + larger TX/RX batches
#   2) Dual parallel wire loops (two bench processes, different EAL prefixes)
#   3) Baseline 1t1r for comparison
#
# Usage (root, privileged container):
#   source scripts/discover_rtx_pro_topology.sh
#   sudo -E env RTX_TX_BDF="$RTX_TX_BDF" RTX_RX_BDF="$RTX_RX_BDF" \
#     ETH_DST_ADDR="$ETH_DST_ADDR" RUN_SECONDS=20 \
#     ./examples/run_rtx_pro_push_400g.sh
#
# Output: bench-results/<ts>-rtx-pro-push400g/{runs.csv,plots/}

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$REPO_ROOT/build}"
DISCOVER="$REPO_ROOT/scripts/discover_rtx_pro_topology.sh"
BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
MQ_BASE="$SCRIPT_DIR/daqiri_bench_raw_tx_rx_rtx_pro_6000_mq.yaml"
MQ_GEN="$REPO_ROOT/scripts/gen_rtx_pro_mq_config.py"
NIC_BASE="$SCRIPT_DIR/daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml"
PLOT_SCRIPT="$REPO_ROOT/scripts/plot_rtx_pro_bench.py"

RUN_SECONDS="${RUN_SECONDS:-20}"
TX_GPU="${TX_GPU:-0}"
RX_GPU="${RX_GPU:-1}"

export LD_LIBRARY_PATH="/opt/daqiri/lib:$BUILD_DIR:${LD_LIBRARY_PATH:-}"

if [[ -x "$DISCOVER" ]]; then
  # shellcheck disable=SC1090
  source "$DISCOVER"
fi

TX_BDF="${RTX_TX_BDF:-0000:75:00.0}"
RX_BDF="${RTX_RX_BDF:-0000:05:00.0}"
TX_IF="${RTX_TX_IFACE:-}"
RX_IF="${RTX_RX_IFACE:-}"

[[ -z "${ETH_DST_ADDR:-}" ]] && { echo "ERROR: ETH_DST_ADDR required" >&2; exit 1; }

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${DAQIRI_BENCH_RESULTS_DIR:-$REPO_ROOT/bench-results}/$TS-rtx-pro-push400g"
mkdir -p "$OUT_DIR"

CSV="$OUT_DIR/runs.csv"
echo "experiment,cell,tx_cores,rx_cores,payload,batch,rep,tx_gbps,rx_gbps,pps,drops,notes" > "$CSV"

FAILURES=0

sum_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" 2>/dev/null \
    | grep -oE " $field=[0-9]+" | sed -E "s/.* $field=//" \
    | awk '{ s += $1 } END { printf "%d", s+0 }'
}

max_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" 2>/dev/null \
    | grep -oE " $field=[0-9.]+" | sed -E "s/.* $field=//" \
    | awk '{ if ($1+0 > m+0) m = $1 } END { printf "%s", (m == "" ? 0 : m) }'
}

parse_dpdk_drops() {
  local log="$1" sum=0 v
  for key in imissed ierrors rx_nombuf; do
    v="$(grep -oE "$key=[0-9]+" "$log" 2>/dev/null | tail -n1 | sed -E 's/.*=//' || true)"
    [[ -n "${v:-}" ]] && sum=$((sum + v))
  done
  echo "$sum"
}

max_phy() {
  grep -oE "${1}:[[:space:]]*[0-9]+" "$2" 2>/dev/null \
    | grep -oE '[0-9]+$' | sort -n | tail -n1
}

gen_nic_yaml() {
  local out="$1" txbdf="$2" rxbdf="$3" rxmac="$4" payload="${5:-8000}" batch="${6:-10240}"
  sed -E \
    -e "s|address: 0000:61:00.0|address: $txbdf|g" \
    -e "s|address: 0000:61:00.1|address: $rxbdf|g" \
    -e "s|<00:00:00:00:00:00>|$rxmac|g" \
    -e "s|^( *payload_size: ).*|\1$payload|" \
    -e "s|^( *batch_size: ).*|\1$batch|g" \
    "$NIC_BASE" > "$out"
  awk -v txg="$TX_GPU" -v rxg="$RX_GPU" '
    /^    - name: "Data_TX_GPU"/ { in_tx=1; in_rx=0 }
    /^    - name: "Data_RX_GPU"/ { in_rx=1; in_tx=0 }
    in_tx && /^      affinity:/ { sub(/[0-9]+$/, txg); in_tx=0 }
    in_rx && /^      affinity:/ { sub(/[0-9]+$/, rxg); in_rx=0 }
    { print }
  ' "$out" > "${out}.tmp" && mv "${out}.tmp" "$out"
}

record_run() {
  local exp="$1" cell="$2" tx_cores="$3" rx_cores="$4" payload="$5" batch="$6"
  local stdout="$7" stderr="$8" notes="$9"
  local tx_pkts rx_pkts tx_bytes rx_bytes secs tx_gbps rx_gbps pps drops
  tx_pkts="$(sum_field 'TX complete' packets "$stdout")"
  rx_pkts="$(sum_field 'RX complete' packets "$stdout")"
  tx_bytes="$(sum_field 'TX complete' bytes "$stdout")"
  rx_bytes="$(sum_field 'RX complete' bytes "$stdout")"
  secs="$(max_field 'RX complete' seconds "$stdout")"
  if [[ "${rx_pkts:-0}" -eq 0 || -z "${secs:-}" || "$secs" == "0" ]]; then
    echo "FAIL: $exp $cell p$payload b$batch — rx_pkts=$rx_pkts" >&2
    FAILURES=$((FAILURES + 1))
    return 1
  fi
  tx_gbps="$(awk -v b="$tx_bytes" -v s="$secs" 'BEGIN { printf "%.3f", (b*8.0)/s/1e9 }')"
  rx_gbps="$(awk -v b="$rx_bytes" -v s="$secs" 'BEGIN { printf "%.3f", (b*8.0)/s/1e9 }')"
  pps="$(awk -v p="$rx_pkts" -v s="$secs" 'BEGIN { printf "%.0f", p/s }')"
  drops="$(parse_dpdk_drops "$stderr")"
  echo "$exp,$cell,$tx_cores,$rx_cores,$payload,$batch,1,$tx_gbps,$rx_gbps,$pps,$drops,$notes" | tee -a "$CSV"
}

run_mq_cell() {
  local exp="$1" tx_count="$2" rx_count="$3" payload="$4" batch="$5"
  local cell notes tx_cores rx_cores
  if [[ "$tx_count" == 2 && "$rx_count" == 2 ]]; then cell="2t2r"
  elif [[ "$tx_count" == 1 && "$rx_count" == 2 ]]; then cell="1t2r"
  elif [[ "$tx_count" == 2 && "$rx_count" == 1 ]]; then cell="2t1r"
  else cell="1t1r"; fi
  if [[ "$tx_count" == 2 ]]; then tx_cores="11|12"; else tx_cores="11"; fi
  if [[ "$rx_count" == 2 ]]; then rx_cores="9|14"; else rx_cores="9"; fi
  notes="batch=$batch mq"
  local run_dir="$OUT_DIR/mq-${cell}-p${payload}-b${batch}"
  mkdir -p "$run_dir"
  local cfg="$run_dir/config.yaml"
  python3 "$MQ_GEN" "$MQ_BASE" --tx "$tx_count" --rx "$rx_count" \
    --payload "$payload" --batch "$batch" --eth-dst "$ETH_DST_ADDR" \
    --tx-bdf "$TX_BDF" --rx-bdf "$RX_BDF" \
    --tx-gpu "$TX_GPU" --rx-gpu "$RX_GPU" > "$cfg" 2>"$run_dir/gen.err" || return 1
  # Patch queue batch_size in daqiri cfg (generator sets bench_tx only).
  awk -v b="$batch" '
    /^          batch_size:/ { sub(/[0-9]+$/, b); print; next }
    { print }
  ' "$cfg" > "${cfg}.tmp" && mv "${cfg}.tmp" "$cfg"
  "$BENCH_BIN" "$cfg" --seconds "$RUN_SECONDS" > "$run_dir/stdout.txt" 2>"$run_dir/stderr.txt" || true
  record_run "$exp" "$cell" "$tx_cores" "$rx_cores" "$payload" "$batch" \
    "$run_dir/stdout.txt" "$run_dir/stderr.txt" "$notes"
}

run_nic_loop() {
  local exp="$1" txbdf="$2" rxbdf="$3" rxmac="$4" label="$5"
  local payload="${6:-8000}" batch="${7:-10240}"
  local run_dir="$OUT_DIR/${exp}-${label}"
  mkdir -p "$run_dir"
  gen_nic_yaml "$run_dir/config.yaml" "$txbdf" "$rxbdf" "$rxmac" "$payload" "$batch"
  "$BENCH_BIN" "$run_dir/config.yaml" --seconds "$RUN_SECONDS" \
    > "$run_dir/stdout.txt" 2>"$run_dir/stderr.txt" || true
  record_run "$exp" "1t1r" "11" "9" "$payload" "$batch" \
    "$run_dir/stdout.txt" "$run_dir/stderr.txt" "$label"
}

echo "=== RTX PRO push-400G experiments (${RUN_SECONDS}s each) ==="
echo "Primary loop: TX $TX_BDF -> RX $RX_BDF"
echo "Output: $OUT_DIR"
echo

# Lever 1+5: MQ core scaling + batch tuning at 8 KB
for batch in 10240 16384 20480; do
  run_mq_cell "mq-tune" 2 2 8000 "$batch" || true
done
run_mq_cell "mq-tune" 1 1 8000 10240 || true

# Lever 4: single-queue batch sweep
for batch in 10240 16384 20480 32768; do
  run_nic_loop "batch-tune" "$TX_BDF" "$RX_BDF" "$ETH_DST_ADDR" "loop1-b${batch}" 8000 "$batch" || true
done

# Lever 2: small-packet MQ 2t2r (pps stress)
run_mq_cell "small-pkt" 2 2 64 10240 || true
run_mq_cell "small-pkt" 2 2 256 10240 || true

# Lever 3: second wire pair probe (ens21 <-> ens20)
PAIR2_TX_BDF="0000:f5:00.0"
PAIR2_RX_BDF="0000:85:00.0"
PAIR2_RX_MAC="cc:40:f3:c6:ec:d8"
run_nic_loop "dual-loop-probe" "$PAIR2_TX_BDF" "$PAIR2_RX_BDF" "$PAIR2_RX_MAC" \
  "ens21-to-ens20" 8000 10240 || true
run_nic_loop "dual-loop-probe" "$PAIR2_RX_BDF" "$PAIR2_TX_BDF" \
  "$(cat /sys/class/net/ens21f0np0/address 2>/dev/null || echo cc:40:f3:c6:e5:30)" \
  "ens20-to-ens21" 8000 10240 || true

# Lever 3b: dual parallel loops (primary + pair2 if pair2 works)
PAIR2_DIR="$OUT_DIR/dual-parallel"
mkdir -p "$PAIR2_DIR"
gen_nic_yaml "$PAIR2_DIR/loop1.yaml" "$TX_BDF" "$RX_BDF" "$ETH_DST_ADDR" 8000 16384
gen_nic_yaml "$PAIR2_DIR/loop2.yaml" "$PAIR2_TX_BDF" "$PAIR2_RX_BDF" "$PAIR2_RX_MAC" 8000 16384

echo "Starting dual parallel run (loop1 + loop2)..." >&2
"$BENCH_BIN" "$PAIR2_DIR/loop1.yaml" --seconds "$RUN_SECONDS" > "$PAIR2_DIR/l1.stdout" 2>"$PAIR2_DIR/l1.stderr" &
pid1=$!
"$BENCH_BIN" "$PAIR2_DIR/loop2.yaml" --seconds "$RUN_SECONDS" > "$PAIR2_DIR/l2.stdout" 2>"$PAIR2_DIR/l2.stderr" &
pid2=$!
wait "$pid1" || true
wait "$pid2" || true

l1_rx="$(sum_field 'RX complete' bytes "$PAIR2_DIR/l1.stdout")"
l2_rx="$(sum_field 'RX complete' bytes "$PAIR2_DIR/l2.stdout")"
l1_secs="$(max_field 'RX complete' seconds "$PAIR2_DIR/l1.stdout")"
l2_secs="$(max_field 'RX complete' seconds "$PAIR2_DIR/l2.stdout")"
agg_gbps="$(awk -v b1="$l1_rx" -v b2="$l2_rx" -v s1="$l1_secs" -v s2="$l2_secs" '
  BEGIN {
    g1 = (s1+0>0) ? (b1*8.0)/s1/1e9 : 0
    g2 = (s2+0>0) ? (b2*8.0)/s2/1e9 : 0
    printf "%.3f", g1+g2
  }')"
l1_gbps="$(awk -v b="$l1_rx" -v s="$l1_secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
l2_gbps="$(awk -v b="$l2_rx" -v s="$l2_secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
echo "dual-parallel,aggregate,|,|,8000,16384,1,$l1_gbps,$l2_gbps,0,0,loop1=${l1_gbps}G loop2=${l2_gbps}G sum=${agg_gbps}G" | tee -a "$CSV"
echo "dual-parallel,sum,,,8000,16384,1,0,${agg_gbps},0,0,aggregate_rx_gbps" | tee -a "$CSV"

echo
echo "==================== Results ===================="
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"
echo
best="$(awk -F, 'NR>1 && $9+0>max {max=$9+0; line=$0} END {print line}' "$CSV")"
echo "Best single RX Gbps: $best"
best_agg="$(awk -F, '$1=="dual-parallel" && $2=="sum" {print $9; exit}' "$CSV")"
echo "Dual-loop aggregate RX Gbps: ${best_agg:-n/a}"

# Plot MQ-style if we have enough rows
if [[ -f "$PLOT_SCRIPT" ]]; then
  awk -F, 'NR==1 {print "cell,tx_cores,rx_cores,payload,rep,gbps,rx_gbps,pps,drops"; next}
    $1 ~ /^mq-tune/ {print $2","$3","$4","$5",1,"$7","$8","$9","$10}' "$CSV" \
    > "$OUT_DIR/mq_subset.csv"
  if [[ $(wc -l < "$OUT_DIR/mq_subset.csv") -gt 2 ]]; then
    python3 "$PLOT_SCRIPT" "$OUT_DIR/mq_subset.csv" "$OUT_DIR/plots" || true
  fi
fi

echo "Results: $OUT_DIR"
[[ "$FAILURES" -gt 0 ]] && echo "Failures: $FAILURES" >&2
exit 0
