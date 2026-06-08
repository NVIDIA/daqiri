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
#   socket  : diff /proc/net/udp drops column (UDP, read inside the server netns);
#             nstat retrans/inerrs (TCP, read inside the client netns).
#
# Usage:
#   ./run_spark_bench.sh <backend> [mode]
#     backend ∈ {dpdk, rdma, socket-udp, socket-tcp}
#     mode    ∈ {smoke, sweep, drop-curve, drop-curve-matrix}  (default: smoke)
#
# Required environment in current shell:
#   DAQIRI_BUILD_DIR — path to the cmake build dir (defaults to ../build).
#   ETH_DST_ADDR     — required for dpdk backend (the RX iface MAC).
#
# rdma and socket-{udp,tcp} run split server/client processes inside the
# dq_wire_server / dq_wire_client namespaces, so bring up the netns wire loopback
# first (scripts/setup_spark_wire_loopback_netns.sh up). dpdk runs in the default
# namespace and needs the netns torn down so the PMD can bind the physical ports.
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
# `pairs` = number of concurrent client/server process pairs (socket backends sweep
# this; dpdk/rdma are always 1). `gbps` is aggregate App TX, `rx_gbps` aggregate App RX
# (summed across pairs); App-level loss is (gbps - rx_gbps) / gbps.
echo "lang,backend,post_process,payload,batch,pairs,target_gbps,seconds,packets,bytes,pps,gbps,rx_gbps,drops,drops_kind,cpu_master_pct,cpu_tx_pct,cpu_rx_pct,gpu_sm_pct,gpu_mem_pct" > "$CSV"

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
    PAIRS_SWEEP=(1)
    PAIRS_HEADLINE=(1)
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
    PAIRS_SWEEP=(1)
    PAIRS_HEADLINE=(1)
    # Split server/client YAMLs with netns IPs (10.250.0.1/2); each process runs
    # inside its own network namespace so RDMA-CM resolves addresses over the wire
    # rather than short-cutting through the kernel's local routing table.
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_rdma_tx_rx_spark_netns_server.yaml"
    BASE_YAML_CLIENT="$SCRIPT_DIR/daqiri_bench_rdma_tx_rx_spark_netns_client.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_rdma"
    CPU_MASTER=8; CPU_TX=17; CPU_RX=18
    ;;
    # Single-frame UDP sizes (<= the ~8972 B MTU payload, so no IP fragmentation).
    # 65507 is intentionally excluded: it fragments into ~8 packets and, under
    # multi-pair unpaced load, reassembly collapses out of the shared per-namespace
    # pool -- not a meaningful steady-state operating point. The netns YAMLs still
    # carry a large buf_size so message_size never overflows the TX buffer.
  socket-udp)
    PAYLOADS_SWEEP=(8000 1000)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(8000)
    BATCHES_HEADLINE=(1)
    # Concurrent client/server pairs. A single pair is core-bound well below line rate;
    # the published matrix reaches ~12 Gb/s aggregate by running four pairs.
    PAIRS_SWEEP=(1 2 4)
    PAIRS_HEADLINE=(4)
    SRV_PORT_BASE=5001; CLI_PORT_BASE=5101
    # Split server/client YAMLs with netns IPs (10.250.0.1/2); each process runs
    # inside its own network namespace so the kernel sends client->server over the
    # wire instead of short-cutting same-host IPs through the loopback (lo) device.
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_udp_tx_rx_netns_server.yaml"
    BASE_YAML_CLIENT="$SCRIPT_DIR/daqiri_bench_socket_udp_tx_rx_netns_client.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    # Pair 0 pins to core 16 (see pair_core); report that core's busy% as the per-pair
    # bottleneck. cpu_tx_pct and cpu_rx_pct therefore both refer to the pair-0 core.
    CPU_MASTER=8; CPU_TX=16; CPU_RX=16
    ;;
  socket-tcp)
    # 1 MiB / 8000 / 1000 to mirror the published TCP matrix. The bench memsets a full
    # message into one TX buffer, so the netns YAMLs carry buf_size/max_payload_size
    # >= 1 MiB (a smaller buffer overflows the heap -- see the daqiri_bench_socket
    # message_size-vs-buf_size validation issue).
    PAYLOADS_SWEEP=(1048576 8000 1000)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(8000)
    BATCHES_HEADLINE=(1)
    PAIRS_SWEEP=(1 2 4)
    PAIRS_HEADLINE=(4)
    SRV_PORT_BASE=6001; CLI_PORT_BASE=6101
    # Split server/client YAMLs with netns IPs (10.250.0.1/2); see socket-udp note.
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_tcp_tx_rx_netns_server.yaml"
    BASE_YAML_CLIENT="$SCRIPT_DIR/daqiri_bench_socket_tcp_tx_rx_netns_client.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    # Pair 0 pins to core 16 (see pair_core); report that core's busy% as the per-pair
    # bottleneck. cpu_tx_pct and cpu_rx_pct therefore both refer to the pair-0 core.
    CPU_MASTER=8; CPU_TX=16; CPU_RX=16
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
# Both helpers take an optional namespace: in the netns wire-loopback setup the
# UDP receiver lives in the server netns and TCP retransmits are counted in the
# client (sender) netns, so the counters must be read inside the right namespace
# rather than the default one.
snapshot_proc_net_udp() {
  local ns="${1:-}"
  if [[ -n "$ns" ]]; then
    ip netns exec "$ns" cat /proc/net/udp 2>/dev/null | awk 'NR>1 { sum += $13 } END { print sum+0 }' || echo 0
  else
    awk 'NR>1 { sum += $13 } END { print sum+0 }' /proc/net/udp 2>/dev/null || echo 0
  fi
}
snapshot_nstat() {
  local ns="${1:-}"
  local pre=()
  [[ -n "$ns" ]] && pre=(ip netns exec "$ns")
  "${pre[@]}" nstat -a 2>/dev/null | awk '/TcpExtTCPLostRetransmit|TcpRetransSegs|TcpInErrs/ { s += $2 } END { print s+0 }' || echo 0
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

# Substitute payload / batch into the base YAML and write a temp config (dpdk + rdma;
# sockets use generate_socket_yaml so each pair gets unique ports/cores).
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
      sed -E "s|^( *message_size: ).*|\1$payload|g" "$BASE_YAML_CLIENT" > "${out%.yaml}_client.yaml"
      ;;
  esac
}

# One isolated core per socket pair: 16 + (idx % 4) -> cores 16-19 for pairs 0-3 (Spark
# isolcpus), wrapping (oversubscribing) beyond four. Both the server and the client of a
# pair pin to this same core. Sharing one CPU across the send and receive sides couples
# the send rate to the receive rate (the sender cannot outrun the receiver it time-slices
# with), so each pair sits near the per-core ceiling and N pairs scale to ~N x that. This
# matches the reference four-pair methodology and keeps App TX ~= App RX with low loss.
pair_core() { echo $(( 16 + ($1 % 4) )); }

# Write the server/client YAML pair for socket pair `idx`: substitute message_size,
# unique ports (SRV/CLI_PORT_BASE + idx), and pin every queue to the pair's core.
generate_socket_yaml() {
  local idx="$1" payload="$2" server_out="$3" client_out="$4"
  local srv_port=$(( SRV_PORT_BASE + idx ))
  local cli_port=$(( CLI_PORT_BASE + idx ))
  local core; core="$(pair_core "$idx")"
  sed -E \
    -e "s|^( *message_size: ).*|\1$payload|g" \
    -e "s|^( *local_port: ).*|\1$srv_port|" \
    -e "s|^( *server_port: ).*|\1$srv_port|" \
    -e "s|^( *cpu_core: ).*|\1$core|" \
    "$BASE_YAML" > "$server_out"
  sed -E \
    -e "s|^( *message_size: ).*|\1$payload|g" \
    -e "s|^( *local_port: ).*|\1$cli_port|" \
    -e "s|^( *remote_port: ).*|\1$srv_port|" \
    -e "s|^( *server_port: ).*|\1$srv_port|" \
    -e "s|^( *cpu_core: ).*|\1$core|" \
    "$BASE_YAML_CLIENT" > "$client_out"
}

# Run one cell. Echoes the CSV row to stdout.
run_cell() {
  local lang="$1" payload="$2" batch="$3" pairs="$4" target_gbps="$5"
  local cell="$lang-$BACKEND-p$payload-b$batch-n$pairs-g$target_gbps"
  local cell_dir="$OUT_DIR/$cell"
  mkdir -p "$cell_dir"

  # Snapshot kernel-side drop counters. In the netns wire loopback the UDP
  # receiver lives in the server netns and TCP retransmits are counted on the
  # client (sender) netns, so read each counter inside the relevant namespace.
  local udp_ns="" tcp_ns=""
  [[ "$BACKEND" == "socket-udp" ]] && udp_ns="dq_wire_server"
  [[ "$BACKEND" == "socket-tcp" ]] && tcp_ns="dq_wire_client"
  local udp_before tcp_before
  udp_before="$(snapshot_proc_net_udp "$udp_ns")"
  tcp_before="$(snapshot_nstat "$tcp_ns")"

  # Snapshot per-cpu stats just before the bench starts.
  snapshot_cpu_stat "$cell_dir/cpu_stat.before"

  # Background GPU dmon (1-sec sample, RUN_SECONDS samples).
  ( nvidia-smi dmon -s pucvmet -c "$RUN_SECONDS" > "$cell_dir/nvidia_smi_dmon.txt" 2>&1 ) &
  local dmon_pid=$!

  # Run the bench. Stderr captures DAQIRI_LOG_* output (DPDK/RDMA drop sources).
  local stdout="$cell_dir/stdout.txt"
  local stderr="$cell_dir/stderr.txt"
  local bench_rc=0
  local pkts="" bytes="" secs="" rx_bytes=""

  local bench_extra=()
  [[ "$target_gbps" != "0" ]] && bench_extra+=(--target-gbps "$target_gbps")
  # Shutdown ordering: the server must keep receiving until the client has fully
  # stopped sending, otherwise the client's last in-flight messages have no peer
  # to land on -- for RDMA that flushes the QP (status 5, "Work Request Flushed
  # Error"), a burst that pollutes the byte/drop counters and can cut the run
  # short. The client starts STARTUP_SLEEP after the server, so give the server
  # STARTUP_SLEEP + SERVER_GRACE extra seconds to outlive it.
  local startup_sleep=3 server_grace=5
  local server_seconds=$(( RUN_SECONDS + startup_sleep + server_grace ))

  if [[ "$BACKEND" =~ ^socket- ]]; then
    # `pairs` independent client/server processes, each in the wire-loopback
    # namespaces with unique ports and cores. A single pair is core-bound below line
    # rate; the published Spark matrix reaches ~12 Gb/s by aggregating four pairs.
    # App TX (client sent) and App RX (server recv) are summed across pairs.
    local i server_pids=() client_pids=()
    for ((i = 0; i < pairs; i++)); do
      generate_socket_yaml "$i" "$payload" \
        "$cell_dir/server_p$i.yaml" "$cell_dir/client_p$i.yaml"
    done
    for ((i = 0; i < pairs; i++)); do
      ip netns exec dq_wire_server "$BENCH_BIN" "$cell_dir/server_p$i.yaml" \
          --seconds "$server_seconds" "${bench_extra[@]}" --mode server \
          > "$cell_dir/server_p$i.stdout" 2> "$cell_dir/server_p$i.stderr" &
      server_pids+=("$!")
    done
    sleep "$startup_sleep"
    for ((i = 0; i < pairs; i++)); do
      ip netns exec dq_wire_client "$BENCH_BIN" "$cell_dir/client_p$i.yaml" \
          --seconds "$RUN_SECONDS" "${bench_extra[@]}" --mode client \
          > "$cell_dir/client_p$i.stdout" 2> "$cell_dir/client_p$i.stderr" &
      client_pids+=("$!")
    done
    for i in "${client_pids[@]}"; do wait "$i" || bench_rc=$?; done
    for i in "${server_pids[@]}"; do wait "$i" 2>/dev/null || true; done

    local tx_pkts=0 tx_bytes=0 agg_rx_bytes=0 max_secs=0
    for ((i = 0; i < pairs; i++)); do
      local sp sb se rb
      sp="$(extract_field 'Client complete' sent_packets "$cell_dir/client_p$i.stdout")"
      sb="$(extract_field 'Client complete' sent_bytes   "$cell_dir/client_p$i.stdout")"
      se="$(extract_field 'Client complete' seconds      "$cell_dir/client_p$i.stdout")"
      rb="$(extract_field 'Server complete' recv_bytes   "$cell_dir/server_p$i.stdout")"
      tx_pkts=$(( tx_pkts + ${sp:-0} ))
      tx_bytes=$(( tx_bytes + ${sb:-0} ))
      agg_rx_bytes=$(( agg_rx_bytes + ${rb:-0} ))
      max_secs="$(awk -v a="$max_secs" -v b="${se:-0}" 'BEGIN { print (b+0>a+0)?b:a }')"
    done
    pkts="$tx_pkts"; bytes="$tx_bytes"; rx_bytes="$agg_rx_bytes"; secs="$max_secs"
    cat "$cell_dir"/server_p*.stderr "$cell_dir"/client_p*.stderr > "$stderr" 2>/dev/null || true
    cat "$cell_dir"/client_p*.stdout "$cell_dir"/server_p*.stdout > "$stdout" 2>/dev/null || true
  elif [[ "$BACKEND" == "rdma" ]]; then
    # Split server/client processes in separate namespaces so RDMA-CM resolves
    # addresses over the wire rather than short-cutting the kernel's local table.
    local yaml="$cell_dir/config.yaml"
    generate_yaml "$yaml" "$payload" "$batch"
    ip netns exec dq_wire_server "$BENCH_BIN" "$yaml" \
        --seconds "$server_seconds" "${bench_extra[@]}" --mode server \
        > "$cell_dir/server_stdout.txt" 2> "$cell_dir/server_stderr.txt" &
    local server_pid=$!
    sleep "$startup_sleep"
    ip netns exec dq_wire_client "$BENCH_BIN" "${yaml%.yaml}_client.yaml" \
        --seconds "$RUN_SECONDS" "${bench_extra[@]}" --mode client \
        > "$stdout" 2> "$stderr" || bench_rc=$?
    wait "$server_pid" 2>/dev/null || true
    cat "$cell_dir/server_stdout.txt" >> "$stdout"
    cat "$cell_dir/server_stderr.txt" >> "$stderr"
    # RDMA prints "Client complete: ... send_completions=N send_bytes=N seconds=S".
    pkts="$(extract_field 'Client complete' send_completions "$stdout")"
    bytes="$(extract_field 'Client complete' send_bytes "$stdout")"
    secs="$(extract_field 'Client complete' seconds "$stdout")"
    rx_bytes="$bytes"
  else
    local yaml="$cell_dir/config.yaml"
    generate_yaml "$yaml" "$payload" "$batch"
    "$BENCH_BIN" "$yaml" --seconds "$RUN_SECONDS" "${bench_extra[@]}" \
        > "$stdout" 2> "$stderr" || bench_rc=$?
    # For RX-bearing benches "RX complete" is authoritative; fall back to "TX complete".
    pkts="$(extract_field 'RX complete' packets "$stdout")"
    bytes="$(extract_field 'RX complete' bytes   "$stdout")"
    secs="$(extract_field 'RX complete' seconds  "$stdout")"
    if [[ -z "$pkts" ]]; then
      pkts="$(extract_field 'TX complete' packets "$stdout")"
      bytes="$(extract_field 'TX complete' bytes   "$stdout")"
      secs="$(extract_field 'TX complete' seconds  "$stdout")"
    fi
    rx_bytes="$bytes"
  fi
  cp "$stderr" "$DRIVER_LOG"

  # Snapshot per-cpu stats right after the bench exits (before background
  # captures finish reaping, to bound the window).
  snapshot_cpu_stat "$cell_dir/cpu_stat.after"

  # Stop background captures (they self-terminate at -c <N>, but reap if needed).
  wait "$dmon_pid"  2>/dev/null || true

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

  local pps gbps rx_gbps
  pps="$(awk -v p="$pkts" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.0f", p/s; else print 0 }')"
  gbps="$(awk -v b="$bytes" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
  rx_gbps="$(awk -v b="${rx_bytes:-0}" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"

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
      local udp_after; udp_after="$(snapshot_proc_net_udp "$udp_ns")"
      drops="$((udp_after - udp_before))"
      drops_kind="udp-proc-net-udp-drops"
      ;;
    socket-tcp)
      local tcp_after; tcp_after="$(snapshot_nstat "$tcp_ns")"
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

  echo "$lang,$BACKEND,none,$payload,$batch,$pairs,$target_gbps,$secs,$pkts,$bytes,$pps,$gbps,$rx_gbps,$drops,$drops_kind,$cpu_master_pct,$cpu_tx_pct,$cpu_rx_pct,$gpu_sm,$gpu_mem" \
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
    # One cell, native-shape, unpaced (headline pair count).
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for n in "${PAIRS_HEADLINE[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" "$n" 0
        done
      done
    done
    ;;
  sweep)
    # Full payload × batch × pairs matrix at line rate.
    for p in "${PAYLOADS_SWEEP[@]}"; do
      for b in "${BATCHES_SWEEP[@]}"; do
        for n in "${PAIRS_SWEEP[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" "$n" 0
        done
      done
    done
    ;;
  drop-curve)
    # Hold native-shape + headline pairs constant, sweep target_gbps.
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for n in "${PAIRS_HEADLINE[@]}"; do
          for g in "${DROP_CURVE_TARGETS[@]}"; do
            run_cell_or_record_failure cpp "$p" "$b" "$n" "$g"
          done
        done
      done
    done
    ;;
  drop-curve-matrix)
    # 2D drop curve: sweep payload × target_gbps at the headline batch + pairs.
    for p in "${PAYLOADS_SWEEP[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for n in "${PAIRS_HEADLINE[@]}"; do
          for g in "${DROP_CURVE_TARGETS[@]}"; do
            run_cell_or_record_failure cpp "$p" "$b" "$n" "$g"
          done
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
