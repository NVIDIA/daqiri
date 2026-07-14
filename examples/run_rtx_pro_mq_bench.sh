#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# RTX PRO 6000 DPDK multi-queue core-scaling + payload sweep.
#
# Sweeps the four (TX, RX) poller-count cells at multiple payload sizes to show
# how wire throughput scales as cores are added and how that varies with packet
# size. Cells:
#
#   cell  TX pollers  RX pollers
#   1t1r  1           1
#   1t2r  1           2
#   2t1r  2           1
#   2t2r  2           2
#
# Usage (as root, privileged container):
#   source scripts/discover_rtx_pro_topology.sh
#   ./examples/run_rtx_pro_mq_bench.sh
#
# Environment:
#   RUN_SECONDS   per cell (default 20)
#   PAYLOADS      space-separated payload bytes (default "64 256 1024 4096 8000")
#   REPEATS       repeats per (cell, payload) (default 1)
#   RTX_TX_BDF, RTX_RX_BDF, RTX_RX_IFACE, ETH_DST_ADDR from discovery
#   RTX_TX_GPU, RTX_RX_GPU, RTX_TX_GPU2, RTX_RX_GPU2, RTX_CPU_CORES (discovery)
#
# Plot afterwards:
#   scripts/plot_rtx_pro_bench.py bench-results/<ts>-rtx-pro-mq/runs.csv

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$REPO_ROOT/build}"
DISCOVER="$REPO_ROOT/scripts/discover_rtx_pro_topology.sh"
BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
RUN_SECONDS="${RUN_SECONDS:-20}"
PAYLOADS="${PAYLOADS:-64 256 1024 4096 8000}"
REPEATS="${REPEATS:-1}"

MQ_BASE="$SCRIPT_DIR/daqiri_bench_raw_tx_rx_rtx_pro_6000_mq.yaml"
MQ_GEN="$REPO_ROOT/scripts/gen_rtx_pro_mq_config.py"
PLOT_SCRIPT="$REPO_ROOT/scripts/plot_rtx_pro_bench.py"

export LD_LIBRARY_PATH="/opt/daqiri/lib:$BUILD_DIR:${LD_LIBRARY_PATH:-}"
export CUDA_DEVICE_ORDER="${CUDA_DEVICE_ORDER:-PCI_BUS_ID}"

if [[ -x "$DISCOVER" ]]; then
  # shellcheck disable=SC1090
  source "$DISCOVER"
fi

TX_BDF="${RTX_TX_BDF:-}"
RX_BDF="${RTX_RX_BDF:-}"
TX_GPU="${TX_GPU:-${RTX_TX_GPU:-0}}"
RX_GPU="${RX_GPU:-${RTX_RX_GPU:-1}}"
TX_GPU2="${TX_GPU2:-${RTX_TX_GPU2:-$TX_GPU}}"
RX_GPU2="${RX_GPU2:-${RTX_RX_GPU2:-$RX_GPU}}"

# master, TX q0 poll/work, TX q1 poll/work, RX q0 poll/work, RX q1 poll/work
if [[ -n "${RTX_CPU_CORES:-}" ]]; then
  read -r -a CPU_CORES <<< "$RTX_CPU_CORES"
else
  CPU_CORES=(3 11 10 12 13 9 8 14 15)
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${DAQIRI_BENCH_RESULTS_DIR:-$REPO_ROOT/bench-results}/$TS-rtx-pro-mq"
mkdir -p "$OUT_DIR"

CSV="$OUT_DIR/runs.csv"
echo "cell,tx_cores,rx_cores,payload,rep,gbps,rx_gbps,pps,drops,cpu_master,cpu_tx_q0_poll,cpu_tx_q0_work,cpu_tx_q1_poll,cpu_tx_q1_work,cpu_rx_q0_poll,cpu_rx_q0_work,cpu_rx_q1_poll,cpu_rx_q1_work,gpu_sm,gpu_mem" > "$CSV"

if [[ -x "$SCRIPT_DIR/bench_capture_environment.sh" ]]; then
  "$SCRIPT_DIR/bench_capture_environment.sh" "$OUT_DIR" || true
fi

if [[ ! -x "$BENCH_BIN" ]]; then
  echo "ERROR: bench binary not found: $BENCH_BIN" >&2
  exit 1
fi
if [[ ! -f "$MQ_BASE" || ! -f "$MQ_GEN" ]]; then
  echo "ERROR: MQ base/generator missing: $MQ_BASE / $MQ_GEN" >&2
  exit 1
fi
if [[ -z "${ETH_DST_ADDR:-}" ]]; then
  echo "ERROR: ETH_DST_ADDR must be set (source scripts/discover_rtx_pro_topology.sh)" >&2
  exit 1
fi
if [[ -z "$TX_BDF" || -z "$RX_BDF" ]]; then
  echo "ERROR: RTX_TX_BDF and RTX_RX_BDF must be set (discovery or env)" >&2
  exit 1
fi

CPU_CORES_CSV="$(IFS=,; echo "${CPU_CORES[*]}")"

CELLS=(
  "1t1r 1 1"
  "1t2r 1 2"
  "2t1r 2 1"
  "2t2r 2 2"
)

FAILURES=0

sum_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" 2>/dev/null \
    | grep -oE " $field=[0-9]+" \
    | sed -E "s/.* $field=//" \
    | awk '{ s += $1 } END { printf "%d", s+0 }'
}

max_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" 2>/dev/null \
    | grep -oE " $field=[0-9.]+" \
    | sed -E "s/.* $field=//" \
    | awk '{ if ($1+0 > m+0) m = $1 } END { printf "%s", (m == "" ? 0 : m) }'
}

parse_dpdk_drops() {
  local log="$1"
  local sum=0 v
  for key in imissed ierrors rx_nombuf; do
    v="$(grep -oE "$key=[0-9]+" "$log" 2>/dev/null | tail -n1 | sed -E "s/.*=//" || true)"
    [[ -n "${v:-}" ]] && sum=$((sum + v))
  done
  v="$(grep -oE 'total: [0-9]+' "$log" 2>/dev/null | tail -n1 | grep -oE '[0-9]+$' || true)"
  [[ -n "${v:-}" ]] && sum=$((sum + v))
  echo "$sum"
}

max_phy_counter() {
  local key="$1" file="$2"
  grep -oE "${key}:[[:space:]]*[0-9]+" "$file" 2>/dev/null \
    | grep -oE '[0-9]+$' \
    | sort -n \
    | tail -n1
}

snapshot_cpu_stat() {
  awk '/^cpu[0-9]+/ {
    total = $2+$3+$4+$5+$6+$7+$8
    busy  = total - $5 - $6
    print $1, total, busy
  }' /proc/stat > "$1"
}

cpu_busy_pct() {
  local before="$1" after="$2" cpu_idx="$3"
  awk -v cpu="cpu$cpu_idx" '
    NR == FNR { b_total[$1] = $2; b_busy[$1] = $3; next }
              { a_total[$1] = $2; a_busy[$1] = $3 }
    END {
      dt = a_total[cpu] - b_total[cpu]
      db = a_busy[cpu]  - b_busy[cpu]
      if (dt > 0) printf "%.1f", (db * 100.0) / dt
      else        printf "0.0"
    }
  ' "$before" "$after"
}

gpu_sample() {
  local out="$1"
  nvidia-smi --query-gpu=utilization.gpu,utilization.memory \
    --format=csv,noheader,nounits -i "$TX_GPU,$TX_GPU2,$RX_GPU,$RX_GPU2" 2>/dev/null \
    | awk -F', ' '{
        sm += $1; mem += $2; n++
      } END {
        if (n > 0) printf "%.1f,%.1f", sm/n, mem/n
        else       printf "0.0,0.0"
      }' > "$out" || echo "0.0,0.0" > "$out"
}

run_cell() {
  local cell="$1" tx_count="$2" rx_count="$3" payload="$4" rep="${5:-1}"
  local tx_cores rx_cores
  [[ "$tx_count" == 2 ]] && tx_cores="${CPU_CORES[1]}|${CPU_CORES[3]}" || tx_cores="${CPU_CORES[1]}"
  [[ "$rx_count" == 2 ]] && rx_cores="${CPU_CORES[5]}|${CPU_CORES[7]}" || rx_cores="${CPU_CORES[5]}"
  local run_dir="$OUT_DIR/$cell/p$payload/r$rep"
  mkdir -p "$run_dir"

  local tmp_cfg="$run_dir/config.yaml"
  if ! python3 "$MQ_GEN" "$MQ_BASE" --tx "$tx_count" --rx "$rx_count" \
        --payload "$payload" --eth-dst "$ETH_DST_ADDR" \
        --tx-bdf "$TX_BDF" --rx-bdf "$RX_BDF" \
        --tx-gpu "$TX_GPU" --rx-gpu "$RX_GPU" \
        --tx-gpu2 "$TX_GPU2" --rx-gpu2 "$RX_GPU2" \
        --cpu-cores "$CPU_CORES_CSV" \
        > "$tmp_cfg" 2> "$run_dir/gen.err"; then
    echo "ERROR: $cell p$payload config generation failed" >&2
    cat "$run_dir/gen.err" >&2
    return 1
  fi

  local stdout="$run_dir/stdout.txt"
  local stderr="$run_dir/stderr.txt"

  snapshot_cpu_stat "$run_dir/cpu_stat.before"
  local phy_before phy_after
  phy_before="$(max_phy_counter 'rx_phy_packets' /dev/null)"
  phy_before="${phy_before:-0}"

  local bench_rc=0
  "$BENCH_BIN" "$tmp_cfg" --seconds "$RUN_SECONDS" > "$stdout" 2> "$stderr" || bench_rc=$?

  phy_after="$(max_phy_counter 'rx_phy_packets' "$stderr")"
  phy_after="${phy_after:-0}"
  snapshot_cpu_stat "$run_dir/cpu_stat.after"

  local tx_pkts tx_bytes rx_pkts rx_bytes secs
  tx_pkts="$(sum_field 'TX complete' packets "$stdout")"
  tx_bytes="$(sum_field 'TX complete' bytes "$stdout")"
  rx_pkts="$(sum_field 'RX complete' packets "$stdout")"
  rx_bytes="$(sum_field 'RX complete' bytes "$stdout")"
  secs="$(max_field 'RX complete' seconds "$stdout")"

  if [[ "$bench_rc" -ne 0 || "${rx_pkts:-0}" -eq 0 || -z "${secs:-}" || "$secs" == "0" ]]; then
    echo "ERROR: $cell p$payload failed (rc=$bench_rc rx_pkts=${rx_pkts:-0})" >&2
    return 1
  fi

  local pps tx_gbps rx_gbps drops
  pps="$(awk -v p="$rx_pkts" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.0f", p/s; else print 0 }')"
  tx_gbps="$(awk -v b="$tx_bytes" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
  rx_gbps="$(awk -v b="$rx_bytes" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
  drops="$(parse_dpdk_drops "$stderr")"

  local phy_delta=$(( phy_after - phy_before ))
  if [[ "${rx_pkts:-0}" -gt 1000 && "${phy_after:-0}" -lt 100 ]]; then
    echo "WARN: $cell p$payload rx_phy_packets=${phy_after:-0} flat -- check cable" >&2
  elif [[ "$phy_delta" -gt 0 ]]; then
    echo "INFO: $cell p$payload wire OK -- rx_phy_packets +$phy_delta" >&2
  fi

  local cpu_vals=()
  local c
  for c in "${CPU_CORES[@]}"; do
    cpu_vals+=("$(cpu_busy_pct "$run_dir/cpu_stat.before" "$run_dir/cpu_stat.after" "$c")")
  done
  local cpu_csv gpu_csv
  cpu_csv="$(IFS=,; echo "${cpu_vals[*]}")"
  gpu_csv="$(gpu_sample "$run_dir/gpu.txt")"

  echo "$cell,$tx_cores,$rx_cores,$payload,$rep,$tx_gbps,$rx_gbps,$pps,$drops,$cpu_csv,$gpu_csv" | tee -a "$CSV"
}

echo "RTX PRO 6000 multi-queue sweep -- ${RUN_SECONDS}s per (cell, payload)"
echo "TX BDF=$TX_BDF RX BDF=$RX_BDF ETH_DST=$ETH_DST_ADDR"
echo "TX GPU=$TX_GPU,$TX_GPU2  RX GPU=$RX_GPU,$RX_GPU2  cores=${CPU_CORES[*]}"
echo "Payloads: $PAYLOADS"
echo "Output:   $OUT_DIR"
echo

for entry in "${CELLS[@]}"; do
  read -r cell tx_count rx_count <<< "$entry"
  for payload in $PAYLOADS; do
    for rep in $(seq 1 "$REPEATS"); do
      run_cell "$cell" "$tx_count" "$rx_count" "$payload" "$rep" \
        || FAILURES=$((FAILURES + 1))
    done
  done
done

echo
echo "==================== RTX PRO multi-queue sweep ===================="
printf "%-6s %-9s %-9s %8s %10s %10s %8s\n" "cell" "tx_cores" "rx_cores" "payload" "tx_Gbps" "rx_Gbps" "drops"
printf "%-6s %-9s %-9s %8s %10s %10s %8s\n" "----" "--------" "--------" "-------" "-------" "-------" "-----"
while IFS=, read -r cell tx_cores rx_cores payload rep tx_gbps rx_gbps pps drops _rest; do
  printf "%-6s %-9s %-9s %8s %10s %10s %8s\n" "$cell" "$tx_cores" "$rx_cores" "$payload" "$tx_gbps" "$rx_gbps" "$drops"
done < <(tail -n +2 "$CSV")
echo "==================================================================="
echo
echo "Results in: $OUT_DIR"
echo "CSV:        $CSV"

if [[ -f "$PLOT_SCRIPT" ]]; then
  python3 "$PLOT_SCRIPT" "$CSV" "$OUT_DIR/plots" && echo "Plots:      $OUT_DIR/plots/"
fi

if [[ "$FAILURES" -ne 0 ]]; then
  echo "Failed cells: $FAILURES" >&2
  exit 1
fi
