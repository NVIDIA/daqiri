#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# DGX Spark DPDK multi-queue core-scaling sweep (issue #15, "Phase 5").
#
# Runs daqiri_bench_raw_gpudirect across the four multi-queue cells, at each of a
# set of payload sizes, to show raw-Ethernet throughput scaling as TX/RX CPU
# cores are added and how that scaling varies with payload. The matrix cells are
# (TX cores, RX cores) = (1,1), (1,2), (2,1), (2,2); the goal is to demonstrate
# (2,2) > (1,1) when a single core is the bottleneck.
#
#   cell  TX cores  RX cores
#   1t1r  16        18
#   1t2r  16        18,19
#   2t1r  16,17     18
#   2t2r  16,17     18,19
#
# All four are derived from the single checked-in base
# examples/daqiri_bench_raw_tx_rx_spark_mq.yaml (the balanced 2,2 superset) by
# scripts/gen_spark_mq_config.py, which prunes queues/flows/memory-regions/bench
# entries down to each cell. They share host_pinned memory, an over-the-wire
# loopback (tx 0000:01:00.0 -> rx 0002:01:00.1), and master_core 8. The native
# shape is an 8000 B payload; this script sweeps PAYLOADS (default 64..8000 B),
# generating a fresh config per (cell, payload) -- it NEVER edits the base.
#
# Set ETH_DST_ADDR in the current shell to fill the rx_port MAC into the
# generated configs (cat /sys/class/net/<rx-iface>/address); the RX queue runs
# with flow_isolation, so unmatched-MAC packets are dropped.
#
# Per (cell, payload) it captures: aggregate App RX Gbps and pps (summed across
# all bench RX queues), DPDK drops (imissed+ierrors+rx_nombuf from the manager
# log), per-core busy% for cores 8/16/17/18/19, and a wire-traffic check via the
# NIC *_phy SerDes counters. One combined CSV row per (cell, payload) is written
# to bench-results/<ts>-dpdk-mq/runs.csv. Render the line plot afterwards with:
#   scripts/plot_mq_payload_sweep.py bench-results/<ts>-dpdk-mq/runs.csv
#
# === RUN AS ROOT IN THE PRIVILEGED PROJECT CONTAINER (per AGENTS.md). ===
#
# === TEAR DOWN THE dq_wire_* NETNS FIRST. ===
# This is a DPDK run in the DEFAULT namespace: the PMD binds the two physical
# ports directly (0000:01:00.0 / 0002:01:00.1). If the netns wire loopback is
# still up, the kernel owns those netdevs and the PMD cannot bind them. Tear it
# down before running:
#
#   scripts/setup_spark_wire_loopback_netns.sh down
#
# Build + install first so /opt/daqiri/lib is not stale (see the container
# stale-lib trap in AGENTS.md):
#
#   cmake --build build -j && cmake --install build --prefix /opt/daqiri
#
# Usage:
#   ./run_spark_mq_bench.sh
#
# Optional environment in the current shell:
#   DAQIRI_BUILD_DIR — path to the cmake build dir (defaults to ../build).
#   RUN_SECONDS      — per-(cell,payload) run length in seconds (default 30).
#   PAYLOADS         — space-separated payload byte sizes (default "64 256 1024 4096 8000").
#   ETH_DST_ADDR     — rx_port MAC filled into the generated configs (see above).
#   REPEATS          — repeats per (cell, payload) for error bars (default 1; use 3
#                      for the published re-run). Each rep is an independent run + row.

set -u
set -o pipefail

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$SCRIPT_DIR/../build}"
BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
RUN_SECONDS="${RUN_SECONDS:-30}"
PAYLOADS="${PAYLOADS:-64 256 1024 4096 8000}"
# Repeats per (cell, payload) for error bars. Each rep is an independent run with
# its own dir (<cell>/p<payload>/r<rep>) and CSV row; report mean +/- std across
# reps. Default 1; set REPEATS=3 for the published re-run.
REPEATS="${REPEATS:-1}"

# Single checked-in base + the generator that prunes it to each cell.
MQ_BASE="$SCRIPT_DIR/daqiri_bench_raw_tx_rx_spark_mq.yaml"
MQ_GEN="$SCRIPT_DIR/../scripts/gen_spark_mq_config.py"

# Match run_spark_bench.sh: prefer the installed shared libs, falling back to
# the build tree. Keep both so a fresh build dir still resolves.
export LD_LIBRARY_PATH="/opt/daqiri/lib:$BUILD_DIR:${LD_LIBRARY_PATH:-}"

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$SCRIPT_DIR/../bench-results/$TS-dpdk-mq"
mkdir -p "$OUT_DIR"

CSV="$OUT_DIR/runs.csv"
echo "cell,tx_cores,rx_cores,payload,rep,gbps,pps,drops,cpu8,cpu16,cpu17,cpu18,cpu19,cpu5,cpu6,cpu7,cpu9" > "$CSV"

# Capture slow-moving environment state once per result set (mirrors
# run_spark_bench.sh). Best-effort -- skip if the helper is unavailable.
if [[ -x "$SCRIPT_DIR/bench_capture_environment.sh" ]]; then
  "$SCRIPT_DIR/bench_capture_environment.sh" "$OUT_DIR" || true
fi

if [[ ! -x "$BENCH_BIN" ]]; then
  echo "ERROR: bench binary not found: $BENCH_BIN" >&2
  echo "       build + install first:" >&2
  echo "         cmake --build build -j && cmake --install build --prefix /opt/daqiri" >&2
  exit 1
fi

if [[ ! -f "$MQ_BASE" || ! -f "$MQ_GEN" ]]; then
  echo "ERROR: multi-queue base/generator missing: $MQ_BASE / $MQ_GEN" >&2
  exit 1
fi

# RX netdev whose *_phy counters prove traffic crossed the cable. rx_port is the
# PCIe address 0002:01:00.1; resolve its netdev name for ethtool -S. Override
# with RX_NETDEV if auto-detection fails.
RX_PCI="0002:01:00.1"
RX_NETDEV="${RX_NETDEV:-}"
if [[ -z "$RX_NETDEV" ]]; then
  RX_NETDEV="$(ls "/sys/bus/pci/devices/$RX_PCI/net" 2>/dev/null | head -n1 || true)"
fi

# cell name -> "tx_queue_count rx_queue_count". The CSV's tx_cores/rx_cores
# display columns are derived from these counts (TX -> cores 16[,17], RX ->
# 18[,19]); multi-core lists use '|' (not ',') so they stay single CSV fields --
# a comma would split the row and misalign every column after it.
CELLS=(
  "1t1r 1 1"
  "1t2r 1 2"
  "2t1r 2 1"
  "2t2r 2 2"
)

# Cores to sample busy% for, in CSV column order.
# Sample master (8), the queue pollers (16-19), and the bench workers (5,6,7,9)
# under the poller/worker split. Order must match the CSV header above.
CPU_CORES=(8 16 17 18 19 5 6 7 9)

FAILURES=0

# --------------------------------------------------------------------------
# Helpers (conventions shared with run_spark_bench.sh)
# --------------------------------------------------------------------------

# Sum a numeric field across EVERY matching "<prefix> ..." stdout line. The
# multi-queue bench prints one "RX complete ... queue=N packets=.. bytes=.."
# line per RX queue (and likewise one "TX complete" per TX queue), so aggregate
# App throughput is the sum across all queue lines on a side.
sum_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" 2>/dev/null \
    | grep -oE " $field=[0-9]+" \
    | sed -E "s/.* $field=//" \
    | awk '{ s += $1 } END { printf "%d", s+0 }'
}

# The per-queue "seconds=" value is the run length; take the max across lines.
max_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" 2>/dev/null \
    | grep -oE " $field=[0-9.]+" \
    | sed -E "s/.* $field=//" \
    | awk '{ if ($1+0 > m+0) m = $1 } END { printf "%s", (m == "" ? 0 : m) }'
}

# Sum DPDK drop counters from the manager log emitted via DAQIRI_LOG_INFO.
parse_dpdk_drops() {
  local log="$1"
  local sum=0 v
  for key in imissed ierrors rx_nombuf; do
    v="$(grep -oE "$key=[0-9]+" "$log" 2>/dev/null | tail -n1 | sed -E "s/.*=//" || true)"
    [[ -n "${v:-}" ]] && sum=$((sum + v))
  done
  echo "$sum"
}

# Snapshot /proc/stat per-cpu counters to a file (same logic as run_spark_bench.sh).
snapshot_cpu_stat() {
  awk '/^cpu[0-9]+/ {
    total = $2+$3+$4+$5+$6+$7+$8
    busy  = total - $5 - $6
    print $1, total, busy
  }' /proc/stat > "$1"
}

# Compute busy% for a single cpu index between two /proc/stat snapshots.
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

# Sum the RX-side *_phy packet counter (proves traffic crossed the cable, not an
# on-chip vport short-cut -- see scripts/setup_spark_wire_loopback_netns.sh).
phy_rx_packets() {
  [[ -z "$RX_NETDEV" ]] && { echo 0; return; }
  ethtool -S "$RX_NETDEV" 2>/dev/null \
    | awk -F'[: ]+' '$2 ~ /rx_packets_phy/ { s += $3 } END { printf "%d", s+0 }'
}

# --------------------------------------------------------------------------
# Run one (cell, payload)
# --------------------------------------------------------------------------

run_cell() {
  local cell="$1" tx_count="$2" rx_count="$3" payload="$4" rep="${5:-1}"
  # CSV display columns: TX -> 16[,17], RX -> 18[,19] per queue count. '|' keeps
  # multi-core lists in a single CSV field.
  local tx_cores rx_cores
  [[ "$tx_count" == 2 ]] && tx_cores="16|17" || tx_cores="16"
  [[ "$rx_count" == 2 ]] && rx_cores="18|19" || rx_cores="18"
  local run_dir="$OUT_DIR/$cell/p$payload/r$rep"
  mkdir -p "$run_dir"

  # Generate the cell from the single base -- never touch the base. Fill the
  # rx_port MAC from ETH_DST_ADDR when set (the RX queue runs flow_isolation).
  local tmp_cfg="$run_dir/config.yaml"
  local eth_dst_arg=()
  [[ -n "${ETH_DST_ADDR:-}" ]] && eth_dst_arg=(--eth-dst "$ETH_DST_ADDR")
  if ! python3 "$MQ_GEN" "$MQ_BASE" --tx "$tx_count" --rx "$rx_count" \
        --payload "$payload" "${eth_dst_arg[@]}" > "$tmp_cfg" 2> "$run_dir/gen.err"; then
    echo "ERROR: $cell p$payload config generation failed" >&2
    cat "$run_dir/gen.err" >&2
    return 1
  fi

  local stdout="$run_dir/stdout.txt"
  local stderr="$run_dir/stderr.txt"

  snapshot_cpu_stat "$run_dir/cpu_stat.before"
  local phy_before; phy_before="$(phy_rx_packets)"

  local bench_rc=0
  "$BENCH_BIN" "$tmp_cfg" --seconds "$RUN_SECONDS" > "$stdout" 2> "$stderr" || bench_rc=$?

  local phy_after; phy_after="$(phy_rx_packets)"
  snapshot_cpu_stat "$run_dir/cpu_stat.after"

  # Aggregate RX is authoritative throughput; sum across all RX-queue lines.
  local pkts bytes secs
  pkts="$(sum_field 'RX complete' packets "$stdout")"
  bytes="$(sum_field 'RX complete' bytes "$stdout")"
  secs="$(max_field 'RX complete' seconds "$stdout")"

  if [[ "$bench_rc" -ne 0 || "${pkts:-0}" -eq 0 || -z "${secs:-}" || "$secs" == "0" ]]; then
    [[ "$bench_rc" -ne 0 ]] && echo "ERROR: $cell p$payload bench exited with status $bench_rc" >&2
    [[ "${pkts:-0}" -eq 0 ]] && echo "ERROR: $cell p$payload produced no parseable RX completion stats" >&2
    echo "       stdout: $stdout" >&2
    echo "       stderr: $stderr" >&2
    return 1
  fi

  local pps gbps
  pps="$(awk -v p="$pkts" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.0f", p/s; else print 0 }')"
  gbps="$(awk -v b="$bytes" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"

  local drops; drops="$(parse_dpdk_drops "$stderr")"

  # Wire-traffic confirmation: rx_packets_phy must advance over the run.
  local phy_delta=$(( phy_after - phy_before ))
  if [[ -z "$RX_NETDEV" ]]; then
    echo "WARN: $cell p$payload could not resolve RX netdev for $RX_PCI; skipped wire (*_phy) check" >&2
  elif [[ "$phy_delta" -le 0 ]]; then
    echo "WARN: $cell p$payload rx_packets_phy did not advance ($phy_delta) -- traffic may not have crossed the wire" >&2
  else
    echo "INFO: $cell p$payload wire OK -- rx_packets_phy +$phy_delta" >&2
  fi

  # Per-core busy% over the bench window, in CSV column order (8,16,17,18,19).
  local cpu_vals=()
  local c
  for c in "${CPU_CORES[@]}"; do
    cpu_vals+=("$(cpu_busy_pct "$run_dir/cpu_stat.before" "$run_dir/cpu_stat.after" "$c")")
  done

  local cpu_csv; cpu_csv="$(IFS=,; echo "${cpu_vals[*]}")"
  echo "$cell,$tx_cores,$rx_cores,$payload,$rep,$gbps,$pps,$drops,$cpu_csv" | tee -a "$CSV"
}

# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

echo "Multi-queue payload sweep -- ${RUN_SECONDS}s per (cell, payload)"
echo "Payloads: $PAYLOADS"
echo "Build dir: $BUILD_DIR"
echo "RX netdev for wire (*_phy) check: ${RX_NETDEV:-<unresolved>} ($RX_PCI)"
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

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

echo
echo "==================== Multi-queue payload sweep ===================="
printf "%-6s %-9s %-9s %8s %4s %10s %14s %8s\n" "cell" "tx_cores" "rx_cores" "payload" "rep" "Gbps" "pps" "drops"
printf "%-6s %-9s %-9s %8s %4s %10s %14s %8s\n" "----" "--------" "--------" "-------" "---" "----" "---" "-----"
# Re-read the CSV (skip header) so the table reflects exactly what was recorded.
while IFS=, read -r cell tx_cores rx_cores payload rep gbps pps drops _rest; do
  printf "%-6s %-9s %-9s %8s %4s %10s %14s %8s\n" "$cell" "$tx_cores" "$rx_cores" "$payload" "$rep" "$gbps" "$pps" "$drops"
done < <(tail -n +2 "$CSV")
echo "==================================================================="
echo
echo "Results in: $OUT_DIR"
echo "CSV:        $CSV"
echo "Plot:       scripts/plot_mq_payload_sweep.py $CSV"

if [[ "$FAILURES" -ne 0 ]]; then
  echo "Failed cells: $FAILURES" >&2
  exit 1
fi
