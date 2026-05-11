#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Sweep wrapper for DAQIRI benchmarks on DGX Spark. Runs the bench across a
# matrix of (payload/message size, batch size, target-gbps), captures per-run
# CPU/GPU/NIC counters, and emits one CSV row per cell into bench-results/.
#
# Drop sources per backend (per the report methodology):
#   DPDK    : grep imissed/ierrors/rx_nombuf from bench log (DAQIRI_LOG_INFO).
#   RDMA    : grep "CQ error" lines from bench log (DAQIRI_LOG_ERROR).
#   socket  : diff /proc/net/udp drops column (UDP); nstat -a (TCP retransmits).
#
# Usage:
#   ./run_spark_bench.sh <backend> [mode]
#     backend ∈ {dpdk, rdma, socket-udp, socket-tcp}
#     mode    ∈ {smoke, sweep, drop-curve}  (default: smoke)
#
# Required environment in current shell:
#   DAQIRI_BUILD_DIR — path to the cmake build dir (defaults to ../build).
#   ETH_DST_ADDR     — required for dpdk backend (the RX iface MAC).
#   RX_IFACE         — kernel name of the RX interface for /proc/net/udp diff
#                       (e.g. enP2p1s0f0np0); required for socket-udp.
#
# Run inside the project container as root (per AGENTS.md).

set -u
set -o pipefail

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

BACKEND="${1:-}"
MODE="${2:-smoke}"
if [[ -z "$BACKEND" ]]; then
  echo "Usage: $0 <dpdk|rdma|socket-udp|socket-tcp> [smoke|sweep|drop-curve]" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$SCRIPT_DIR/../build}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$SCRIPT_DIR/../bench-results/$TS-$BACKEND-$MODE"
mkdir -p "$OUT_DIR"

CSV="$OUT_DIR/runs.csv"
echo "lang,backend,post_process,payload,batch,target_gbps,seconds,packets,bytes,pps,gbps,drops,drops_kind,cpu_busy_mean,gpu_sm_pct,gpu_mem_bw" > "$CSV"

# Capture slow-moving environment state once per result set.
"$SCRIPT_DIR/bench_capture_environment.sh" "$OUT_DIR"

RUN_SECONDS=30
DRIVER_LOG="$OUT_DIR/last_run.stderr"

# Per-backend sweep matrices (see docs/performance-dgx-spark.md methodology).
# Native-shape sizes are the leftmost entry; "matched 8K" cell is also included.
case "$BACKEND" in
  dpdk)
    PAYLOADS_SWEEP=(8000 4096 1024 256 64)
    BATCHES_SWEEP=(10240 4096 1024 256)
    PAYLOADS_HEADLINE=(8000)
    BATCHES_HEADLINE=(10240)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_raw_tx_rx_spark.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
    : "${ETH_DST_ADDR:?ETH_DST_ADDR must be set for dpdk backend (cat /sys/class/net/<rx-iface>/address)}"
    ;;
  rdma)
    PAYLOADS_SWEEP=(8000000 1048576 65536 8192 4096)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(8000000)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_rdma_tx_rx_spark.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_rdma"
    ;;
  socket-udp)
    PAYLOADS_SWEEP=(1472 1024 256 64)
    BATCHES_SWEEP=(256 32 1)
    PAYLOADS_HEADLINE=(1472)
    BATCHES_HEADLINE=(256)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_udp_tx_rx.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    ;;
  socket-tcp)
    PAYLOADS_SWEEP=(1048576 65536 1024)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(65536)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_tcp_tx_rx.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    ;;
  *) echo "Unknown backend: $BACKEND" >&2; exit 1 ;;
esac

DROP_CURVE_TARGETS=(1 5 10 25 50 75 100 0)  # 0 means unpaced (line rate)

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

# Read a scalar field from a `key=value` style stdout line.
# usage: extract_field <pattern-prefix> <field-name> <file>
extract_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" | tail -n1 | grep -oE " $field=[^ ]+" | head -n1 | sed -E "s/.*$field=//"
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

# Count RDMA CQ errors in the manager log.
parse_rdma_drops() {
  local log="$1"
  grep -c 'CQ error' "$log" 2>/dev/null || echo 0
}

# Snapshot socket drops on the kernel side.
snapshot_proc_net_udp() {
  awk 'NR>1 { sum += strtonum("0x" $13) } END { print sum+0 }' /proc/net/udp 2>/dev/null || echo 0
}
snapshot_nstat() {
  nstat -a 2>/dev/null | awk '/TcpExtTCPLostRetransmit|TcpRetransSegs|TcpInErrs/ { s += $2 } END { print s+0 }' || echo 0
}

# Substitute payload / batch into the base YAML and write a temp config.
generate_yaml() {
  local out="$1" payload="$2" batch="$3"
  case "$BACKEND" in
    dpdk)
      sed -E \
        -e "s|^( *payload_size: ).*|\1$payload|" \
        -e "s|^( *batch_size: ).*|\1$batch|" \
        -e "s|<00:00:00:00:00:00>|$ETH_DST_ADDR|g" \
        "$BASE_YAML" > "$out"
      ;;
    rdma)
      sed -E "s|^( *message_size: ).*|\1$payload|g" "$BASE_YAML" > "$out"
      ;;
    socket-udp|socket-tcp)
      sed -E "s|^( *message_size: ).*|\1$payload|g" "$BASE_YAML" > "$out"
      ;;
  esac
}

# Run one cell. Echoes the CSV row to stdout.
run_cell() {
  local lang="$1" payload="$2" batch="$3" target_gbps="$4"
  local cell="$lang-$BACKEND-p$payload-b$batch-g$target_gbps"
  local cell_dir="$OUT_DIR/$cell"
  mkdir -p "$cell_dir"

  local yaml="$cell_dir/config.yaml"
  generate_yaml "$yaml" "$payload" "$batch"

  # Snapshot kernel-side drop counters.
  local udp_before tcp_before
  udp_before="$(snapshot_proc_net_udp)"
  tcp_before="$(snapshot_nstat)"

  # Background captures.
  ( mpstat -P ALL 1 "$RUN_SECONDS" > "$cell_dir/mpstat.txt" 2>&1 ) &
  local mpstat_pid=$!
  ( nvidia-smi dmon -s pucvmet -c "$RUN_SECONDS" > "$cell_dir/nvidia_smi_dmon.txt" 2>&1 ) &
  local dmon_pid=$!

  # Run the bench. Stderr captures DAQIRI_LOG_* output (DPDK/RDMA drop sources).
  local stdout="$cell_dir/stdout.txt"
  local stderr="$cell_dir/stderr.txt"
  local args=("$yaml" --seconds "$RUN_SECONDS")
  [[ "$target_gbps" != "0" ]] && args+=(--target-gbps "$target_gbps")
  [[ "$BACKEND" == "rdma" || "$BACKEND" =~ ^socket- ]] && args+=(--mode both)

  "$BENCH_BIN" "${args[@]}" > "$stdout" 2> "$stderr" || true
  cp "$stderr" "$DRIVER_LOG"

  # Stop background captures (they self-terminate at -c <N>, but reap if needed).
  wait "$mpstat_pid" 2>/dev/null || true
  wait "$dmon_pid"  2>/dev/null || true

  # Parse bench stdout. For RX-bearing benches "RX complete" is authoritative;
  # for TX-only configs fall back to "TX complete".
  local pkts bytes secs
  pkts="$(extract_field 'RX complete' packets "$stdout")"
  bytes="$(extract_field 'RX complete' bytes   "$stdout")"
  secs="$(extract_field 'RX complete' seconds  "$stdout")"
  if [[ -z "$pkts" ]]; then
    pkts="$(extract_field 'TX complete' packets "$stdout")"
    bytes="$(extract_field 'TX complete' bytes   "$stdout")"
    secs="$(extract_field 'TX complete' seconds  "$stdout")"
  fi
  if [[ -z "$pkts" ]]; then
    # RDMA prints "Client/Server complete: ... send_completions=N send_bytes=N seconds=S"
    pkts="$(extract_field 'Client complete' send_completions "$stdout")"
    bytes="$(extract_field 'Client complete' send_bytes "$stdout")"
    secs="$(extract_field 'Client complete' seconds "$stdout")"
  fi
  pkts="${pkts:-0}"; bytes="${bytes:-0}"; secs="${secs:-0}"

  local pps gbps
  pps="$(awk -v p="$pkts" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.0f", p/s; else print 0 }')"
  gbps="$(awk -v b="$bytes" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"

  # Drops per backend.
  local drops drops_kind
  case "$BACKEND" in
    dpdk)
      drops="$(parse_dpdk_drops "$stderr")"
      drops_kind="dpdk-imissed+ierrors+nombuf"
      ;;
    rdma)
      drops="$(parse_rdma_drops "$stderr")"
      drops_kind="rdma-cqe-error"
      ;;
    socket-udp)
      local udp_after; udp_after="$(snapshot_proc_net_udp)"
      drops="$((udp_after - udp_before))"
      drops_kind="udp-proc-net-udp-drops"
      ;;
    socket-tcp)
      local tcp_after; tcp_after="$(snapshot_nstat)"
      drops="$((tcp_after - tcp_before))"
      drops_kind="tcp-nstat-retrans+inerrs"
      ;;
  esac

  # Mean CPU busy% across all cores (mpstat row "all"). Simple aggregate; per-core
  # in mpstat.txt for deeper review.
  local cpu_busy_mean
  cpu_busy_mean="$(awk '/Average:.*all/ { print 100 - $NF; exit }' "$cell_dir/mpstat.txt" 2>/dev/null || echo 0)"
  cpu_busy_mean="${cpu_busy_mean:-0}"

  # GPU SM% and memory BW from nvidia-smi dmon. dmon output columns vary by
  # driver; sm is typically column 5 in `-s pucvmet`, mem is column 6.
  local gpu_sm gpu_mem
  gpu_sm="$(awk '/^ *[0-9]/ { count++; sum += $5 } END { if (count) printf "%.1f", sum/count; else print 0 }' \
               "$cell_dir/nvidia_smi_dmon.txt" 2>/dev/null || echo 0)"
  gpu_mem="$(awk '/^ *[0-9]/ { count++; sum += $6 } END { if (count) printf "%.1f", sum/count; else print 0 }' \
                "$cell_dir/nvidia_smi_dmon.txt" 2>/dev/null || echo 0)"

  echo "$lang,$BACKEND,none,$payload,$batch,$target_gbps,$secs,$pkts,$bytes,$pps,$gbps,$drops,$drops_kind,$cpu_busy_mean,$gpu_sm,$gpu_mem" \
    | tee -a "$CSV"
}

# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

case "$MODE" in
  smoke)
    # One cell, native-shape, unpaced.
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        run_cell cpp "$p" "$b" 0
      done
    done
    ;;
  sweep)
    # Full payload × batch matrix at line rate.
    for p in "${PAYLOADS_SWEEP[@]}"; do
      for b in "${BATCHES_SWEEP[@]}"; do
        run_cell cpp "$p" "$b" 0
      done
    done
    ;;
  drop-curve)
    # Hold native-shape constant, sweep target_gbps.
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for g in "${DROP_CURVE_TARGETS[@]}"; do
          run_cell cpp "$p" "$b" "$g"
        done
      done
    done
    ;;
  *) echo "Unknown mode: $MODE" >&2; exit 1 ;;
esac

echo
echo "Results in: $OUT_DIR"
echo "CSV:        $CSV"
