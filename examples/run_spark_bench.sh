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
#     mode    ∈ {smoke, sweep, drop-curve, drop-curve-matrix}  (default: smoke)
#
# Required environment in current shell:
#   DAQIRI_BUILD_DIR — path to the cmake build dir (defaults to ../build).
#   ETH_DST_ADDR     — required for dpdk backend (the RX iface MAC).
#   RX_IFACE         — kernel name of the RX interface for /proc/net/udp diff
#                       (e.g. enP2p1s0f1np1); required for socket-udp.
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
  echo "Usage: $0 <dpdk|rdma|socket-udp|socket-tcp> [smoke|sweep|drop-curve|drop-curve-matrix]" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$SCRIPT_DIR/../build}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$SCRIPT_DIR/../bench-results/$TS-$BACKEND-$MODE"
mkdir -p "$OUT_DIR"

CSV="$OUT_DIR/runs.csv"
echo "lang,backend,post_process,payload,batch,target_gbps,seconds,packets,bytes,pps,gbps,drops,drops_kind,cpu_master_pct,cpu_tx_pct,cpu_rx_pct,gpu_sm_pct,gpu_mem_pct" > "$CSV"

# Capture slow-moving environment state once per result set.
"$SCRIPT_DIR/bench_capture_environment.sh" "$OUT_DIR"

RUN_SECONDS=30
DRIVER_LOG="$OUT_DIR/last_run.stderr"
FAILURES=0

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
    CPU_MASTER=8; CPU_TX=17; CPU_RX=18
    : "${ETH_DST_ADDR:?ETH_DST_ADDR must be set for dpdk backend (cat /sys/class/net/<rx-iface>/address)}"
    ;;
  rdma)
    PAYLOADS_SWEEP=(8000000 1048576 65536 8192 4096)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(8000000)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_rdma_tx_rx_spark.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_rdma"
    CPU_MASTER=8; CPU_TX=17; CPU_RX=18
    ;;
  socket-udp)
    PAYLOADS_SWEEP=(1472 1024 256 64)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(1472)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_udp_tx_rx.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    CPU_MASTER=8; CPU_TX=17; CPU_RX=18
    ;;
  socket-tcp)
    PAYLOADS_SWEEP=(1048576 65536 1024)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(65536)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_tcp_tx_rx.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    CPU_MASTER=8; CPU_TX=17; CPU_RX=18
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
# /proc/net/udp column 13 ("drops") is printed in decimal (%lu in
# net/ipv4/udp.c). The local_address / rem_address columns are hex.
snapshot_proc_net_udp() {
  awk 'NR>1 { sum += $13 } END { print sum+0 }' /proc/net/udp 2>/dev/null || echo 0
}
snapshot_nstat() {
  nstat -a 2>/dev/null | awk '/TcpExtTCPLostRetransmit|TcpRetransSegs|TcpInErrs/ { s += $2 } END { print s+0 }' || echo 0
}

# Snapshot /proc/stat per-cpu counters to a file. Mpstat is often not installed
# in the bench container; /proc/stat is always available.
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

  # Snapshot per-cpu stats just before the bench starts.
  snapshot_cpu_stat "$cell_dir/cpu_stat.before"

  # Background GPU dmon (1-sec sample, RUN_SECONDS samples).
  ( nvidia-smi dmon -s pucvmet -c "$RUN_SECONDS" > "$cell_dir/nvidia_smi_dmon.txt" 2>&1 ) &
  local dmon_pid=$!

  # Run the bench. Stderr captures DAQIRI_LOG_* output (DPDK/RDMA drop sources).
  local stdout="$cell_dir/stdout.txt"
  local stderr="$cell_dir/stderr.txt"
  local args=("$yaml" --seconds "$RUN_SECONDS")
  [[ "$target_gbps" != "0" ]] && args+=(--target-gbps "$target_gbps")
  [[ "$BACKEND" == "rdma" || "$BACKEND" =~ ^socket- ]] && args+=(--mode both)

  local bench_rc=0
  "$BENCH_BIN" "${args[@]}" > "$stdout" 2> "$stderr" || bench_rc=$?
  cp "$stderr" "$DRIVER_LOG"

  # Snapshot per-cpu stats right after the bench exits (before background
  # captures finish reaping, to bound the window).
  snapshot_cpu_stat "$cell_dir/cpu_stat.after"

  # Stop background captures (they self-terminate at -c <N>, but reap if needed).
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
    case "$BACKEND" in
      rdma)
        # RDMA prints "Client/Server complete: ... send_completions=N send_bytes=N seconds=S"
        pkts="$(extract_field 'Client complete' send_completions "$stdout")"
        bytes="$(extract_field 'Client complete' send_bytes "$stdout")"
        secs="$(extract_field 'Client complete' seconds "$stdout")"
        ;;
      socket-udp|socket-tcp)
        # socket_bench prints "Client/Server complete: ... sent_packets=N sent_bytes=N
        # recv_packets=N recv_bytes=N seconds=S". Use the TX-side counters for parity
        # with how the RDMA fallback reports throughput.
        pkts="$(extract_field 'Client complete' sent_packets "$stdout")"
        bytes="$(extract_field 'Client complete' sent_bytes "$stdout")"
        secs="$(extract_field 'Client complete' seconds "$stdout")"
        ;;
    esac
  fi
  local stats_missing=0
  if [[ -z "${pkts:-}" || -z "${bytes:-}" || -z "${secs:-}" ]]; then
    stats_missing=1
  fi
  if [[ "$bench_rc" -ne 0 || "$stats_missing" -ne 0 ]]; then
    if [[ "$bench_rc" -ne 0 ]]; then
      echo "ERROR: $cell bench exited with status $bench_rc" >&2
    fi
    if [[ "$stats_missing" -ne 0 ]]; then
      echo "ERROR: $cell produced no parseable completion stats" >&2
    fi
    echo "       stdout: $stdout" >&2
    echo "       stderr: $stderr" >&2
    return 1
  fi

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

  # Per-core CPU busy% over the bench window. Cores defined per-backend
  # (master/TX/RX) match the YAML so we measure the threads we actually pin.
  local cpu_master_pct cpu_tx_pct cpu_rx_pct
  cpu_master_pct="$(cpu_busy_pct "$cell_dir/cpu_stat.before" "$cell_dir/cpu_stat.after" "$CPU_MASTER")"
  cpu_tx_pct="$(cpu_busy_pct     "$cell_dir/cpu_stat.before" "$cell_dir/cpu_stat.after" "$CPU_TX")"
  cpu_rx_pct="$(cpu_busy_pct     "$cell_dir/cpu_stat.before" "$cell_dir/cpu_stat.after" "$CPU_RX")"

  # GPU SM% (column 5) and memory-controller % (column 6) from nvidia-smi
  # dmon -s pucvmet. These are near zero for GPUDirect workloads (GPU is a
  # DMA target, not a compute engine).
  local gpu_sm gpu_mem
  gpu_sm="$(awk '/^ *[0-9]/ { count++; sum += $5 } END { if (count) printf "%.1f", sum/count; else print 0 }' \
               "$cell_dir/nvidia_smi_dmon.txt" 2>/dev/null || echo 0)"
  gpu_mem="$(awk '/^ *[0-9]/ { count++; sum += $6 } END { if (count) printf "%.1f", sum/count; else print 0 }' \
                "$cell_dir/nvidia_smi_dmon.txt" 2>/dev/null || echo 0)"

  echo "$lang,$BACKEND,none,$payload,$batch,$target_gbps,$secs,$pkts,$bytes,$pps,$gbps,$drops,$drops_kind,$cpu_master_pct,$cpu_tx_pct,$cpu_rx_pct,$gpu_sm,$gpu_mem" \
    | tee -a "$CSV"
}

run_cell_or_record_failure() {
  run_cell "$@" || FAILURES=$((FAILURES + 1))
}

# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

case "$MODE" in
  smoke)
    # One cell, native-shape, unpaced.
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        run_cell_or_record_failure cpp "$p" "$b" 0
      done
    done
    ;;
  sweep)
    # Full payload × batch matrix at line rate.
    for p in "${PAYLOADS_SWEEP[@]}"; do
      for b in "${BATCHES_SWEEP[@]}"; do
        run_cell_or_record_failure cpp "$p" "$b" 0
      done
    done
    ;;
  drop-curve)
    # Hold native-shape constant, sweep target_gbps.
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for g in "${DROP_CURVE_TARGETS[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" "$g"
        done
      done
    done
    ;;
  drop-curve-matrix)
    # 2D drop curve: sweep payload × target_gbps at the headline batch.
    for p in "${PAYLOADS_SWEEP[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for g in "${DROP_CURVE_TARGETS[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" "$g"
        done
      done
    done
    ;;
  *) echo "Unknown mode: $MODE" >&2; exit 1 ;;
esac

echo
echo "Results in: $OUT_DIR"
echo "CSV:        $CSV"

if [[ "$FAILURES" -ne 0 ]]; then
  echo "Failed cells: $FAILURES" >&2
  exit 1
fi
