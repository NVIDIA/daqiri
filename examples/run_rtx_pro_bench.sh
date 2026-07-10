#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# RTX PRO 6000 benchmark runner (discrete dGPU, arch 120).
# Primary: dual-port L2 wire closed-loop (real NIC traffic, any host with a link).
# Optional: sw-smoke (no NIC, build sanity only).
# Adapts examples/run_spark_bench.sh; override topology via discovery or CLI.
#
# Usage:
#   ./run_rtx_pro_bench.sh <backend> <mode> [options]
#
#   backend: dpdk | dpdk-hds | rdma | roce | ibverbs | socket-udp | socket-tcp
#   mode:    nic-smoke | issue17 | sweep | drop-curve | drop-curve-matrix |
#            sw-smoke | build-only
#
#   nic-smoke  : wire closed-loop sanity (TX port → RX port over L2)
#   issue17    : none/fft/gemm on wire
#   sw-smoke   : optional build sanity (no NIC, non-wire)
#
# Options:
#   --seconds N
#   --workload none|fft|gemm|gemm_fp16
#   --build-targets t1,t2,...   (build-only)
#   --tx-bdf, --rx-bdf, --rx-mac, --tx-gpu, --rx-gpu
#
# Environment:
#   DAQIRI_BUILD_DIR, ETH_DST_ADDR (required for dpdk wire modes)
#   WORKLOAD, WORKLOAD_BATCH, GEMM_N, SYNC_INTERVAL (GPU post-process)
#   RTX_TX_BDF, RTX_RX_BDF (defaults 61:00.0 / 61:00.1)
#
# Run inside the project container as root (per AGENTS.md).

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$REPO_ROOT/build}"
DISCOVER="$REPO_ROOT/scripts/discover_rtx_pro_topology.sh"

BACKEND="${1:-}"
MODE="${2:-}"
shift 2 2>/dev/null || true

RUN_SECONDS=30
BUILD_TARGETS="raw_gpudirect,raw_hds,raw_reorder_seq,raw_reorder_quantize,rdma,socket"
TX_BDF=""
RX_BDF=""
RX_MAC=""
TX_GPU=""
RX_GPU=""
CLI_WORKLOAD=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds) RUN_SECONDS="$2"; shift 2 ;;
    --workload) CLI_WORKLOAD="$2"; shift 2 ;;
    --build-targets) BUILD_TARGETS="$2"; shift 2 ;;
    --tx-bdf) TX_BDF="$2"; shift 2 ;;
    --rx-bdf) RX_BDF="$2"; shift 2 ;;
    --rx-mac) RX_MAC="$2"; shift 2 ;;
    --tx-gpu) TX_GPU="$2"; shift 2 ;;
    --rx-gpu) RX_GPU="$2"; shift 2 ;;
    -h|--help)
      sed -n '1,40p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "$BACKEND" || -z "$MODE" ]]; then
  echo "Usage: $0 <backend> <mode> [--seconds N] [--workload none|fft|gemm] ..." >&2
  exit 1
fi

# roce is an alias for rdma; smoke is an alias for nic-smoke.
[[ "$BACKEND" == "roce" ]] && BACKEND="rdma"
if [[ "$MODE" == "smoke" ]]; then
  MODE="nic-smoke"
fi
[[ "$MODE" == "workload-smoke" ]] && MODE="issue17"
# sw-workload-smoke is the no-cable analogue of workload-smoke/issue17.
[[ "$MODE" == "sw-workload-smoke" ]] && MODE="sw-issue17"

# Software-loopback modes need no NIC/cable; all "sw-*" modes share this path.
# Detecting it from the mode (not a single literal) keeps the wire-only guards
# below correct for every current and future SW mode.
IS_SW=0
[[ "$MODE" == sw-* ]] && IS_SW=1

WORKLOAD="${CLI_WORKLOAD:-${WORKLOAD:-none}}"
case "$WORKLOAD" in
  none|fft|gemm|gemm_fp16) ;;
  *)
    echo "Invalid WORKLOAD '$WORKLOAD' (expected none|fft|gemm|gemm_fp16)" >&2
    exit 1
    ;;
esac
WORKLOAD_BATCH="${WORKLOAD_BATCH:-}"
GEMM_N="${GEMM_N:-}"
SYNC_INTERVAL="${SYNC_INTERVAL:-}"

# --------------------------------------------------------------------------
# build-only
# --------------------------------------------------------------------------
if [[ "$MODE" == "build-only" ]]; then
  IFS=',' read -r -a targets <<< "$BUILD_TARGETS"
  cmake_args=(
    -S "$REPO_ROOT" -B "$BUILD_DIR"
    -DBUILD_SHARED_LIBS=ON
    -DDAQIRI_BUILD_PYTHON=OFF
    -DDAQIRI_BUILD_EXAMPLES=ON
    -DDAQIRI_MGR="dpdk socket rdma ibverbs"
    -DCMAKE_CUDA_ARCHITECTURES=120
  )
  echo "Configuring: cmake ${cmake_args[*]}"
  cmake "${cmake_args[@]}"

  declare -A target_map=(
    [raw_gpudirect]=daqiri_bench_raw_gpudirect
    [raw_hds]=daqiri_bench_raw_hds
    [raw_reorder_seq]=daqiri_bench_raw_reorder_seq
    [raw_reorder_quantize]=daqiri_bench_raw_reorder_quantize
    [rdma]=daqiri_bench_rdma
    [socket]=daqiri_bench_socket
  )
  build_list=()
  for t in "${targets[@]}"; do
    t="$(echo "$t" | xargs)"
    [[ -z "$t" ]] && continue
    if [[ -n "${target_map[$t]:-}" ]]; then
      build_list+=("${target_map[$t]}")
    else
      echo "Unknown build target: $t" >&2
      exit 1
    fi
  done
  echo "Building: ${build_list[*]}"
  cmake --build "$BUILD_DIR" --target "${build_list[@]}" -j"$(nproc)"
  exit $?
fi

# --------------------------------------------------------------------------
# Discovery + wire preflight
# --------------------------------------------------------------------------
if [[ -x "$DISCOVER" ]]; then
  # shellcheck disable=SC1090
  source "$DISCOVER"
fi
[[ -n "$TX_BDF" ]] && export RTX_TX_BDF="$TX_BDF"
[[ -n "$RX_BDF" ]] && export RTX_RX_BDF="$RX_BDF"
[[ -n "$RX_MAC" ]] && export ETH_DST_ADDR="$RX_MAC"

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$REPO_ROOT/bench-results/${TS}-rtx-pro-${BACKEND}-${MODE}"
mkdir -p "$OUT_DIR"

CSV="$OUT_DIR/runs.csv"
SUMMARY_CSV="$OUT_DIR/summary.csv"
echo "lang,backend,post_process,payload,batch,target_gbps,seconds,tx_packets,rx_packets,bytes,pps,gbps,rx_gbps,drops,drops_kind,cpu_master_pct,cpu_tx_pct,cpu_rx_pct,gpu_sm_pct,gpu_mem_pct,post_process_batch,post_process_gemm_n,post_process_sync" > "$CSV"
echo "backend,workload,config,seconds,total_data_rate_gbps,total_packets_sent,total_packets_received,total_dropped,drop_source,notes" > "$SUMMARY_CSV"

"$SCRIPT_DIR/bench_capture_environment.sh" "$OUT_DIR"

DRIVER_LOG="$OUT_DIR/last_run.stderr"
FAILURES=0

TX_BDF="${RTX_TX_BDF:-0000:61:00.0}"
RX_BDF="${RTX_RX_BDF:-0000:61:00.1}"
TX_GPU="${TX_GPU:-0}"
RX_GPU="${RX_GPU:-1}"

wire_preflight() {
  if [[ "$IS_SW" == "1" ]]; then
    return 0
  fi
  case "$BACKEND" in
    dpdk|dpdk-hds|ibverbs)
      : "${ETH_DST_ADDR:?ETH_DST_ADDR must be set for wire modes (source scripts/discover_rtx_pro_topology.sh)}"
      local tx_if="${RTX_TX_IFACE:-}"
      local rx_if="${RTX_RX_IFACE:-}"
      [[ -z "$tx_if" ]] && tx_if="$(resolve_iface_for_bdf "$TX_BDF" 2>/dev/null || true)"
      [[ -z "$rx_if" ]] && rx_if="$(resolve_iface_for_bdf "$RX_BDF" 2>/dev/null || true)"
      if [[ -n "$tx_if" && -n "$rx_if" ]]; then
        local c0 c1
        c0="$(cat "/sys/class/net/${tx_if}/carrier" 2>/dev/null || echo 0)"
        c1="$(cat "/sys/class/net/${rx_if}/carrier" 2>/dev/null || echo 0)"
        if [[ "$c0" != "1" || "$c1" != "1" ]]; then
          echo "WARNING: carrier not 1 on both ports ($tx_if=$c0 $rx_if=$c1); wire test may fail" >&2
        fi
      fi
      ;;
  esac
}

wire_preflight

# --------------------------------------------------------------------------
# Backend configuration
# --------------------------------------------------------------------------
TX_SENDER_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
case "$BACKEND" in
  dpdk)
    PAYLOADS_SWEEP=(8000 4096 1024 256 64)
    BATCHES_SWEEP=(10240 4096 1024 256)
    PAYLOADS_HEADLINE=(8000)
    BATCHES_HEADLINE=(10240)
    case "$MODE" in
      sw-smoke|sw-issue17)
        BASE_YAML="$SCRIPT_DIR/daqiri_bench_raw_sw_loopback_rtx_pro_6000.yaml"
        ;;
      nic-smoke|sweep|drop-curve|drop-curve-matrix|issue17)
        BASE_YAML="$SCRIPT_DIR/daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml"
        ;;
      *)
        echo "Unknown mode for dpdk: $MODE" >&2
        exit 1
        ;;
    esac
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
    CPU_MASTER=3
    CPU_TX=11
    CPU_RX=9
    ;;
  dpdk-hds)
    PAYLOADS_SWEEP=(1000)
    BATCHES_SWEEP=(10240)
    PAYLOADS_HEADLINE=(1000)
    BATCHES_HEADLINE=(10240)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_raw_tx_rx_hds_rtx_pro_6000.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_hds"
    CPU_MASTER=3
    CPU_TX=10
    CPU_RX=8
    ;;
  rdma)
    PAYLOADS_SWEEP=(8000000 1048576 65536 8192 4096)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(8000000)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_rdma_tx_rx_rtx_pro_6000.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_rdma"
    CPU_MASTER=3
    CPU_TX=7
    CPU_RX=8
    ;;
  ibverbs)
    PAYLOADS_SWEEP=(8000)
    BATCHES_SWEEP=(1024)
    PAYLOADS_HEADLINE=(8000)
    BATCHES_HEADLINE=(1024)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_raw_rx_ibverbs_rtx_pro_6000.yaml"
    TX_YAML="$SCRIPT_DIR/daqiri_bench_raw_tx_only_rtx_pro_6000_nic.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
    CPU_MASTER=3
    CPU_TX=10
    CPU_RX=8
    ;;
  socket-udp)
    PAYLOADS_SWEEP=(1472 1024 256 64)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(1472)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_udp_tx_rx.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    CPU_MASTER=3
    CPU_TX=7
    CPU_RX=8
    ;;
  socket-tcp)
    PAYLOADS_SWEEP=(1048576 65536 1024)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(65536)
    BATCHES_HEADLINE=(1)
    BASE_YAML="$SCRIPT_DIR/daqiri_bench_socket_tcp_tx_rx.yaml"
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    CPU_MASTER=3
    CPU_TX=7
    CPU_RX=8
    ;;
  *)
    echo "Unknown backend: $BACKEND" >&2
    exit 1
    ;;
esac

DROP_CURVE_TARGETS=(1 5 10 25 50 75 100 0)

# SW loopback rides the single-segment GPUDirect ring in the dpdk manager. HDS
# (segment split), RoCE, ibverbs, and sockets have no software-loopback path,
# so fail early with a clear pointer rather than emitting a misleading run.
if [[ "$IS_SW" == "1" && "$BACKEND" != "dpdk" ]]; then
  echo "SW-loopback modes (sw-smoke / sw-issue17) are only supported on the 'dpdk' backend." >&2
  echo "Use: $0 dpdk sw-issue17 --workload fft   (no cable needed)" >&2
  echo "For $BACKEND, use a wire mode (nic-smoke / issue17) with a cabled link." >&2
  exit 1
fi

if [[ ! -x "$BENCH_BIN" ]]; then
  echo "Benchmark binary not found: $BENCH_BIN (run build-only mode first)" >&2
  exit 1
fi

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
extract_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" | tail -n1 | grep -oE " $field=[^ ]+" | head -n1 | sed -E "s/.*$field=//"
}

parse_dpdk_drops() {
  local log="$1"
  local sum=0 v
  for key in imissed ierrors rx_nombuf; do
    v="$(grep -oE "$key=[0-9]+" "$log" 2>/dev/null | grep -oE '[0-9]+$' | sort -n | tail -n1 || true)"
    [[ -n "${v:-}" ]] && sum=$((sum + v))
  done
  # DAQIRI_LOG_ERROR lines: "Dropped N packets ... (total: T)"
  v="$(grep -oE 'Dropped [0-9]+ packets.*\(total: [0-9]+\)' "$log" 2>/dev/null \
        | grep -oE 'total: [0-9]+' | grep -oE '[0-9]+$' | sort -n | tail -n1 || true)"
  [[ -n "${v:-}" && "$v" -gt "$sum" ]] && sum="$v"
  echo "$sum"
}

parse_rdma_drops() {
  local log="$1"
  grep -c 'CQ error' "$log" 2>/dev/null || echo 0
}

snapshot_proc_net_udp() {
  awk 'NR>1 { sum += $13 } END { print sum+0 }' /proc/net/udp 2>/dev/null || echo 0
}

snapshot_nstat() {
  nstat -a 2>/dev/null | awk '/TcpExtTCPLostRetransmit|TcpRetransSegs|TcpInErrs/ { s += $2 } END { print s+0 }' || echo 0
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

max_phy_counter() {
  local key="$1" file="$2"
  # DPDK logs use "tx_phy_packets:\t123" (colon), not key=value.
  grep -oE "${key}:[[:space:]]*[0-9]+" "$file" 2>/dev/null \
    | grep -oE '[0-9]+$' \
    | sort -n \
    | tail -n1
}

log_phy_counters() {
  local stderr_file="$1"
  local tx_phy rx_phy
  tx_phy="$(max_phy_counter 'tx_phy_packets' "$stderr_file")"
  rx_phy="$(max_phy_counter 'rx_phy_packets' "$stderr_file")"
  tx_phy="${tx_phy:-0}"
  rx_phy="${rx_phy:-0}"
  echo "post-run phy: tx_phy_packets=${tx_phy} rx_phy_packets=${rx_phy}" >> "$OUT_DIR/wire_validation.txt"
  echo "${tx_phy} ${rx_phy}"
}

wire_phy_ok() {
  local stderr_file="$1" rx_pkts="$2"
  local counts tx_phy rx_phy
  counts="$(log_phy_counters "$stderr_file")"
  read -r tx_phy rx_phy <<< "$counts"
  # Dual-port closed-loop: TX port drives tx_phy, RX port drives rx_phy. Take
  # the per-port maxima from extended stats (each counter appears twice).
  if [[ "${rx_pkts:-0}" -gt 1000 && "${rx_phy:-0}" -lt 100 ]]; then
    echo "ERROR: rx_phy_packets=${rx_phy:-0} flat while rx_pkts=$rx_pkts — traffic may not have crossed the link" >&2
    return 1
  fi
  if [[ "${rx_pkts:-0}" -gt 1000 && "${tx_phy:-0}" -lt 100 ]]; then
    echo "ERROR: tx_phy_packets=${tx_phy:-0} flat while rx_pkts=$rx_pkts — TX port may not be driving the wire" >&2
    return 1
  fi
  return 0
}

generate_yaml() {
  local out="$1" payload="$2" batch="$3"
  case "$BACKEND" in
    dpdk)
      # SW loopback has no NIC MAC/BDF to substitute (address is "loopback" and
      # eth_dst is a don't-care), so only rewrite the destination MAC on the
      # wire path where discovery populated ETH_DST_ADDR.
      local sed_args=(
        -e "s|^( *payload_size: ).*|\1$payload|"
        -e "s|^( *batch_size: ).*|\1$batch|g"
        -e "s|address: 0000:61:00.0|address: $TX_BDF|g"
        -e "s|address: 0000:61:00.1|address: $RX_BDF|g"
      )
      if [[ "$IS_SW" != "1" && -n "${ETH_DST_ADDR:-}" ]]; then
        sed_args+=(
          -e "s|<00:00:00:00:00:00>|$ETH_DST_ADDR|g"
          -e "s|^( *eth_dst_addr: ).*|\1$ETH_DST_ADDR|"
        )
      fi
      sed -E "${sed_args[@]}" "$BASE_YAML" > "$out"
      awk -v txg="$TX_GPU" -v rxg="$RX_GPU" '
        /^    - name: "Data_TX_GPU"/ { in_tx=1; in_rx=0 }
        /^    - name: "Data_RX_GPU"/ { in_rx=1; in_tx=0 }
        in_tx && /^      affinity:/ { sub(/[0-9]+$/, txg); in_tx=0 }
        in_rx && /^      affinity:/ { sub(/[0-9]+$/, rxg); in_rx=0 }
        { print }
      ' "$out" > "${out}.tmp" && mv "${out}.tmp" "$out"
      ;;
    dpdk-hds)
      local ipv4_len=$((payload + 50))
      sed -E \
        -e "s|^( *payload_size: ).*|\1$payload|" \
        -e "s|^( *batch_size: ).*|\1$batch|g" \
        -e "s|<00:00:00:00:00:00>|$ETH_DST_ADDR|g" \
        -e "s|^( *eth_dst_addr: ).*|\1$ETH_DST_ADDR|" \
        -e "s|^( *ipv4_len: ).*|\1$ipv4_len|" \
        -e "s|address: 0000:61:00.0|address: $TX_BDF|g" \
        -e "s|address: 0000:61:00.1|address: $RX_BDF|g" \
        "$BASE_YAML" > "$out"
      ;;
    ibverbs)
      sed -E \
        -e "s|address: 0000:61:00.1|address: $RX_BDF|g" \
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

generate_tx_yaml() {
  local out="$1" payload="${2:-8000}" batch="${3:-10240}"
  sed -E \
    -e "s|<00:00:00:00:00:00>|$ETH_DST_ADDR|g" \
    -e "s|^( *eth_dst_addr: ).*|\1$ETH_DST_ADDR|" \
    -e "s|address: 0000:61:00.0|address: $TX_BDF|g" \
    -e "s|^( *payload_size: ).*|\1$payload|" \
    -e "s|^( *batch_size: ).*|\1$batch|g" \
    "$TX_YAML" > "$out"
  awk -v txg="$TX_GPU" '
    /^    - name: "Data_TX_GPU"/ { in_tx=1 }
    in_tx && /^      affinity:/ { sub(/[0-9]+$/, txg); in_tx=0 }
    { print }
  ' "$out" > "${out}.tmp" && mv "${out}.tmp" "$out"
}

append_summary_row() {
  local workload="$1" config="$2" secs="$3" data_rate="$4" tx_pkts="$5" rx_pkts="$6" drops="$7" drop_kind="$8" notes="$9"
  echo "$BACKEND,$workload,$config,$secs,$data_rate,$tx_pkts,$rx_pkts,$drops,$drop_kind,$notes" >> "$SUMMARY_CSV"
}

run_cell() {
  local lang="$1" payload="$2" batch="$3" target_gbps="$4" workload="${5:-$WORKLOAD}"
  local cell="$lang-$BACKEND-w${workload}-p$payload-b$batch-g$target_gbps"
  local cell_dir="$OUT_DIR/$cell"
  mkdir -p "$cell_dir"

  local yaml="$cell_dir/config.yaml"
  generate_yaml "$yaml" "$payload" "$batch"

  local udp_before tcp_before
  udp_before="$(snapshot_proc_net_udp)"
  tcp_before="$(snapshot_nstat)"

  snapshot_cpu_stat "$cell_dir/cpu_stat.before"

  ( nvidia-smi dmon -s pucvmet -c "$RUN_SECONDS" > "$cell_dir/nvidia_smi_dmon.txt" 2>&1 ) &
  local dmon_pid=$!

  local stdout="$cell_dir/stdout.txt"
  local stderr="$cell_dir/stderr.txt"
  local args=("$yaml" --seconds "$RUN_SECONDS")
  [[ "$target_gbps" != "0" ]] && args+=(--target-gbps "$target_gbps")
  [[ "$BACKEND" == "rdma" || "$BACKEND" =~ ^socket- ]] && args+=(--mode both)
  if [[ "$workload" != "none" ]]; then
    args+=(--workload "$workload")
    [[ -n "$WORKLOAD_BATCH" ]] && args+=(--workload-batch-bytes "$WORKLOAD_BATCH")
    [[ -n "$GEMM_N" ]] && args+=(--workload-gemm-n "$GEMM_N")
    [[ -n "$SYNC_INTERVAL" ]] && args+=(--workload-sync-interval "$SYNC_INTERVAL")
  fi

  local tx_pid=""
  local rx_pid=""
  local bench_rc=0
  if [[ "$BACKEND" == "ibverbs" ]]; then
    # ibverbs RX init needs hugepages; start it before the DPDK TX sender so both
    # processes are not competing for the same 1G pages during EAL bring-up.
    "$BENCH_BIN" "${args[@]}" > "$stdout" 2> "$stderr" &
    rx_pid=$!
    sleep 5
    local tx_yaml="$cell_dir/tx_sender.yaml"
    generate_tx_yaml "$tx_yaml" "$payload" "$batch"
    "$TX_SENDER_BIN" "$tx_yaml" --seconds "$((RUN_SECONDS + 5))" > "$cell_dir/tx_stdout.txt" 2> "$cell_dir/tx_stderr.txt" &
    tx_pid=$!
    wait "$rx_pid" || bench_rc=$?
    wait "$tx_pid" 2>/dev/null || true
    cp "$stderr" "$DRIVER_LOG"
  else
    "$BENCH_BIN" "${args[@]}" > "$stdout" 2> "$stderr" || bench_rc=$?
    cp "$stderr" "$DRIVER_LOG"
  fi

  snapshot_cpu_stat "$cell_dir/cpu_stat.after"
  wait "$dmon_pid" 2>/dev/null || true

  local tx_pkts rx_pkts bytes secs tx_bytes rx_bytes
  tx_pkts="$(extract_field 'TX complete' packets "$stdout")"
  tx_bytes="$(extract_field 'TX complete' bytes "$stdout")"
  rx_pkts="$(extract_field 'RX complete' packets "$stdout")"
  rx_bytes="$(extract_field 'RX complete' bytes "$stdout")"
  secs="$(extract_field 'RX complete' seconds "$stdout")"
  [[ -z "$secs" ]] && secs="$(extract_field 'TX complete' seconds "$stdout")"

  if [[ -z "$tx_pkts" && -z "$rx_pkts" ]]; then
    case "$BACKEND" in
      rdma)
        tx_pkts="$(extract_field 'Client complete' send_completions "$stdout")"
        tx_bytes="$(extract_field 'Client complete' send_bytes "$stdout")"
        rx_pkts="$(extract_field 'Client complete' recv_completions "$stdout")"
        rx_bytes="$(extract_field 'Client complete' recv_bytes "$stdout")"
        secs="$(extract_field 'Client complete' seconds "$stdout")"
        ;;
      socket-udp|socket-tcp)
        tx_pkts="$(extract_field 'Client complete' sent_packets "$stdout")"
        tx_bytes="$(extract_field 'Client complete' sent_bytes "$stdout")"
        rx_pkts="$(extract_field 'Client complete' recv_packets "$stdout")"
        rx_bytes="$(extract_field 'Client complete' recv_bytes "$stdout")"
        secs="$(extract_field 'Client complete' seconds "$stdout")"
        ;;
    esac
  fi

  bytes="${rx_bytes:-${tx_bytes:-0}}"
  local stats_missing=0
  if [[ -z "${secs:-}" ]]; then
    stats_missing=1
  fi
  if [[ "$bench_rc" -ne 0 || "$stats_missing" -ne 0 ]]; then
    echo "ERROR: $cell failed (rc=$bench_rc stats_missing=$stats_missing)" >&2
    echo "       stdout: $stdout" >&2
    echo "       stderr: $stderr" >&2
    return 1
  fi

  if [[ "$BACKEND" =~ ^(dpdk|dpdk-hds)$ && "$IS_SW" != "1" ]]; then
    if [[ -z "${rx_pkts:-}" || "$rx_pkts" == "0" ]]; then
      echo "ERROR: $cell NIC RX packets zero — check cable/MAC/flow" >&2
      return 1
    fi
    wire_phy_ok "$stderr" "$rx_pkts" || return 1
  fi

  if [[ "$BACKEND" == "ibverbs" && "$IS_SW" != "1" ]]; then
    local tx_sender_pkts tx_sender_rc=0
    tx_sender_pkts="$(extract_field 'TX complete' packets "$cell_dir/tx_stdout.txt")"
    if grep -q 'daqiri_init failed' "$cell_dir/tx_stderr.txt" 2>/dev/null; then
      echo "ERROR: $cell ibverbs TX sender failed (daqiri_init) — see $cell_dir/tx_stderr.txt" >&2
      return 1
    fi
    if [[ -z "${tx_sender_pkts:-}" || "$tx_sender_pkts" == "0" ]]; then
      echo "ERROR: $cell ibverbs TX sender sent zero packets — RX port may still be kernel-bound to ibverbs only" >&2
      return 1
    fi
    tx_pkts="${tx_sender_pkts}"
    local min_wire_rx=$((RUN_SECONDS * 50000))
    [[ "$min_wire_rx" -lt 500000 ]] && min_wire_rx=500000
    if [[ "${rx_pkts:-0}" -lt "$min_wire_rx" ]]; then
      echo "ERROR: $cell ibverbs RX too low ($rx_pkts pkts, need >=$min_wire_rx) — check flow/MAC/cable" >&2
      return 1
    fi
  fi

  if [[ "$workload" != "none" && "$IS_SW" != "1" && "${rx_pkts:-0}" -lt 1000000 ]]; then
    echo "ERROR: $cell workload=$workload RX too low ($rx_pkts pkts) — GPU post-process path likely stalled" >&2
    return 1
  fi

  tx_pkts="${tx_pkts:-0}"
  rx_pkts="${rx_pkts:-0}"
  local pkts="$rx_pkts"
  [[ "$pkts" == "0" && "$tx_pkts" != "0" ]] && pkts="$tx_pkts"

  local pps gbps rx_gbps
  pps="$(awk -v p="$pkts" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.0f", p/s; else print 0 }')"
  gbps="$(awk -v b="${tx_bytes:-0}" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
  rx_gbps="$(awk -v b="${rx_bytes:-0}" -v s="$secs" 'BEGIN { if (s+0>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
  local data_rate_gbps="$rx_gbps"
  [[ "$data_rate_gbps" == "0.000" || -z "$data_rate_gbps" ]] && data_rate_gbps="$gbps"

  local drops drops_kind notes=""
  case "$BACKEND" in
    dpdk|dpdk-hds|ibverbs)
      drops="$(parse_dpdk_drops "$stderr")"
      drops_kind="dpdk-imissed+ierrors+nombuf"
      ;;
    rdma)
      drops="$(parse_rdma_drops "$stderr")"
      drops_kind="rdma-cqe-error"
      ;;
    socket-udp)
      local udp_after
      udp_after="$(snapshot_proc_net_udp)"
      drops="$((udp_after - udp_before))"
      drops_kind="udp-proc-net-udp-drops"
      ;;
    socket-tcp)
      local tcp_after
      tcp_after="$(snapshot_nstat)"
      drops="$((tcp_after - tcp_before))"
      drops_kind="tcp-nstat-retrans+inerrs"
      ;;
  esac

  if [[ "$IS_SW" == "1" ]]; then
    notes="sw-loopback-nonwire"
  fi

  if [[ "$BACKEND" == "ibverbs" ]]; then
    notes="host-staged"
    if [[ -n "${tx_pkts:-}" && "$tx_pkts" -gt 0 && "$rx_pkts" -lt "$tx_pkts" ]]; then
      local external_loss=$((tx_pkts - rx_pkts))
      drops=$((drops + external_loss))
      drops_kind="${drops_kind}+sender-minus-receiver"
    fi
  fi

  if [[ "$IS_SW" != "1" && "${drops:-0}" -gt 1000000 ]]; then
    local nic_drops="$drops"
    local drop_base="${tx_pkts:-0}"
    if [[ "$BACKEND" == "ibverbs" ]]; then
      # Paired DPDK TX + ibverbs RX: sender-minus-receiver is RX backpressure, not a
      # NIC drop. Fail only on DPDK/imissed counters from the RX process log.
      nic_drops="$(parse_dpdk_drops "$stderr")"
      drop_base="${rx_pkts:-0}"
    fi
    if [[ "$drop_base" -gt 0 && "$nic_drops" -gt 1000000 ]]; then
      local drop_pct
      drop_pct="$(awk -v d="$nic_drops" -v t="$drop_base" 'BEGIN { printf "%.0f", (d*100.0)/t }')"
      if [[ "$drop_pct" -gt 5 ]]; then
        echo "ERROR: $cell drop rate ${drop_pct}% (${nic_drops} drops / ${drop_base} pkts) exceeds 5% wire smoke threshold" >&2
        return 1
      fi
    fi
  fi

  local cpu_master_pct cpu_tx_pct cpu_rx_pct
  cpu_master_pct="$(cpu_busy_pct "$cell_dir/cpu_stat.before" "$cell_dir/cpu_stat.after" "$CPU_MASTER")"
  cpu_tx_pct="$(cpu_busy_pct "$cell_dir/cpu_stat.before" "$cell_dir/cpu_stat.after" "$CPU_TX")"
  cpu_rx_pct="$(cpu_busy_pct "$cell_dir/cpu_stat.before" "$cell_dir/cpu_stat.after" "$CPU_RX")"

  local gpu_sm gpu_mem
  gpu_sm="$(awk '/^ *[0-9]/ { count++; sum += $5 } END { if (count) printf "%.1f", sum/count; else print 0 }' \
               "$cell_dir/nvidia_smi_dmon.txt" 2>/dev/null || echo 0)"
  gpu_mem="$(awk '/^ *[0-9]/ { count++; sum += $6 } END { if (count) printf "%.1f", sum/count; else print 0 }' \
                "$cell_dir/nvidia_smi_dmon.txt" 2>/dev/null || echo 0)"

  local pp_batch="${WORKLOAD_BATCH:-default}"
  local pp_gemm_n="${GEMM_N:-derived}"
  local pp_sync="${SYNC_INTERVAL:-default}"

  echo "$lang,$BACKEND,$workload,$payload,$batch,$target_gbps,$secs,$tx_pkts,$rx_pkts,$bytes,$pps,$gbps,$rx_gbps,$drops,$drops_kind,$cpu_master_pct,$cpu_tx_pct,$cpu_rx_pct,$gpu_sm,$gpu_mem,$pp_batch,$pp_gemm_n,$pp_sync" \
    | tee -a "$CSV"

  append_summary_row "$workload" "$(basename "$yaml")" "$secs" "$data_rate_gbps" "$tx_pkts" "$rx_pkts" "$drops" "$drops_kind" "$notes"
}

run_cell_or_record_failure() {
  run_cell "$@" || FAILURES=$((FAILURES + 1))
}

# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
case "$MODE" in
  issue17|sw-issue17)
    # Same none/fft/gemm workload matrix on the wire (issue17) or over the
    # software loopback (sw-issue17). The SW variant validates the FFT/GEMM
    # post-process path end-to-end with no cable, so results are labelled
    # non-wire in the summary via the SW config name.
    for wl in none fft gemm; do
      for p in "${PAYLOADS_HEADLINE[@]}"; do
        for b in "${BATCHES_HEADLINE[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" 0 "$wl"
        done
      done
    done
    ;;
  sw-smoke|nic-smoke)
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        run_cell_or_record_failure cpp "$p" "$b" 0
      done
    done
    ;;
  sweep)
    for p in "${PAYLOADS_SWEEP[@]}"; do
      for b in "${BATCHES_SWEEP[@]}"; do
        run_cell_or_record_failure cpp "$p" "$b" 0
      done
    done
    ;;
  drop-curve)
    for p in "${PAYLOADS_HEADLINE[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for g in "${DROP_CURVE_TARGETS[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" "$g"
        done
      done
    done
    ;;
  drop-curve-matrix)
    for p in "${PAYLOADS_SWEEP[@]}"; do
      for b in "${BATCHES_HEADLINE[@]}"; do
        for g in "${DROP_CURVE_TARGETS[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" "$g"
        done
      done
    done
    ;;
  *)
    echo "Unknown mode: $MODE" >&2
    exit 1
    ;;
esac

echo
echo "Results in: $OUT_DIR"
echo "CSV:        $CSV"
echo "Summary:    $SUMMARY_CSV"

if [[ "$FAILURES" -ne 0 ]]; then
  echo "Failed cells: $FAILURES" >&2
  exit 1
fi
