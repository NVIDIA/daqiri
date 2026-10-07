#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Sweep wrapper for DAQIRI loopback benchmarks. Runs the bench across a
# matrix of (payload/message size, batch size, target-gbps), captures per-run
# CPU/GPU/NIC counters, and emits one CSV row per cell into bench-results/.
#
# Drop sources per backend (per the report methodology):
#   DPDK    : grep imissed/ierrors/rx_nombuf from bench log (DAQIRI_LOG_INFO).
#   ibverbs : sum CQ errors and application/TX-ring packet drops from engine stats.
#   RDMA    : grep "CQ error" lines from bench log (DAQIRI_LOG_ERROR).
#   socket  : diff /proc/net/udp drops column (UDP, read inside the server netns);
#             nstat retrans/inerrs (TCP, read inside the client netns).
#
# Usage:
#   ./run_cabled_loopback_bench.sh --platform dgx-spark|igx-thor <backend> [mode]
#   ./run_cabled_loopback_bench.sh --platform dgx-spark|igx-thor --show-hardware
#     backend ∈ {dpdk, ibverbs, rdma, socket-udp, socket-tcp}
#     mode    ∈ {smoke, sweep, drop-curve, drop-curve-matrix}  (default: smoke)
#
# Required environment in current shell:
#   DAQIRI_BUILD_DIR — path to the cmake build dir (defaults to ../build).
#   REPEATS          — repeats per cell for error bars (default 1; use 3 for the
#                      published re-run). Each rep is an independent run + CSV row.
#   WORKLOAD         — representative GPU workload run on the REAL received data
#                      in the receive path (preceded by a reorder/gather step):
#                      none (default) | fft | gemm (FP32) | gemm_fp16 (FP16
#                      tensor-core matmul). Honoured by all backends (dpdk,
#                      ibverbs, rdma, socket-udp, socket-tcp); recorded in the CSV post_process
#                      column.
#   GEMM_DIM         — the square GEMM side length n (--workload-gemm-dim; default
#                      1024), held fixed so FLOPs/call (2·n³) is constant. The
#                      compute working set is n·n·elem_size, read from the front of
#                      each received I/O unit. Recorded in post_process_gemm_dim.
#   FFT_LEN          — the 1-D C2C transform length (--workload-fft-len; default
#                      1024) for WORKLOAD=fft, held fixed while the I/O unit is
#                      swept. Independent of GEMM_DIM.
#   SYNC_INTERVAL    — drain the GPU stream every N compute calls
#                      (--workload-sync-interval; default 2). Sweep it (1 2 4 8 16 32)
#                      to see how much of the receive+compute ceiling is single-thread
#                      GPU sync-stall. Recorded in post_process_sync.
#   SOCKET_RX_IO_CORES — optional space-separated socket receive I/O cores, one
#                      per concurrent pair. These override the server RX queue
#                      core independently of the socket_bench worker core.
#   BATCHES_OVERRIDE   — optional space-separated batch sizes for one-off runs.
#   PAYLOADS_OVERRIDE  — optional space-separated payload sizes for one-off runs.
#   DROP_CURVE_TARGETS_OVERRIDE — optional space-separated pacing targets in Gbps.
#   DAQIRI_IBVERBS_NUM_BUFS — optional raw ibverbs TX/RX slot-count override.
#   PAIRS_OVERRIDE     — optional space-separated socket pair counts. For example,
#                      PAIRS_OVERRIDE=1 selects the pair-0 CPU placement only.
#
# Hardware defaults live in cabled_loopback_hardware.yaml. DAQIRI_GPU_UUID,
# DPDK_{TX,RX}_PCI, DPDK_{TX,RX}_NETDEV, ETH_{SRC,DST}_ADDR, and the DAQIRI_*_CORE
# variables override resolved values for one-off experiments.
#
# rdma and socket-{udp,tcp} run split server/client processes inside the
# dq_wire_server / dq_wire_client namespaces, so bring up the netns wire loopback
# first (`scripts/setup_cabled_loopback_netns.sh --platform <platform> up`). Raw
# dpdk/ibverbs runs use the default namespace and need the netns torn down.
#
# Run inside the project container as root (per AGENTS.md).

set -u
set -o pipefail

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

PLATFORM=""
SHOW_HARDWARE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform)
      [[ $# -ge 2 ]] || { echo "--platform requires a value" >&2; exit 1; }
      PLATFORM="$2"
      shift 2
      ;;
    --show-hardware)
      SHOW_HARDWARE=1
      shift
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
    *)
      break
      ;;
  esac
done

BACKEND="${1:-}"
MODE="${2:-smoke}"
if [[ -z "$PLATFORM" ]]; then
  echo "--platform is required (dgx-spark or igx-thor)" >&2
  exit 1
fi
if [[ "$SHOW_HARDWARE" -eq 0 && -z "$BACKEND" ]]; then
  echo "Usage: $0 --platform dgx-spark|igx-thor <dpdk|ibverbs|rdma|socket-udp|socket-tcp> [smoke|sweep|drop-curve|drop-curve-matrix]" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
HARDWARE_RESOLVER="$REPO_DIR/scripts/resolve_benchmark_hardware.py"
HARDWARE_DEFAULTS="$SCRIPT_DIR/cabled_loopback_hardware.yaml"
CLIENT_NS="${CLIENT_NS:-dq_wire_client}"
SERVER_NS="${SERVER_NS:-dq_wire_server}"
resolver_args=(--platform "$PLATFORM" --defaults "$HARDWARE_DEFAULTS")
if [[ "$SHOW_HARDWARE" -eq 1 ]]; then
  python3 "$HARDWARE_RESOLVER" "${resolver_args[@]}"
  exit $?
fi
if [[ "$BACKEND" == "rdma" || "$BACKEND" =~ ^socket- ]]; then
  resolver_args+=(--client-namespace "$CLIENT_NS" --server-namespace "$SERVER_NS")
fi
resolved_hardware="$(python3 "$HARDWARE_RESOLVER" "${resolver_args[@]}" --format shell)" || exit 1
eval "$resolved_hardware"

GPU_MONITOR_ID="${DAQIRI_GPU_UUID:-$DEFAULT_GPU_UUID}"
export CUDA_VISIBLE_DEVICES="$GPU_MONITOR_ID"

MASTER_CORE="${DAQIRI_MASTER_CORE:-$DEFAULT_MASTER_CORE}"
DPDK_TX_QUEUE_CORE="${DAQIRI_DPDK_TX_QUEUE_CORE:-$DEFAULT_DPDK_TX_QUEUE_CORE}"
DPDK_RX_QUEUE_CORE="${DAQIRI_DPDK_RX_QUEUE_CORE:-$DEFAULT_DPDK_RX_QUEUE_CORE}"
DPDK_TX_WORKER_CORE="${DAQIRI_DPDK_TX_WORKER_CORE:-$DEFAULT_DPDK_TX_WORKER_CORE}"
DPDK_RX_WORKER_CORE="${DAQIRI_DPDK_RX_WORKER_CORE:-$DEFAULT_DPDK_RX_WORKER_CORE}"
RDMA_CLIENT_RX_CORE="${DAQIRI_RDMA_CLIENT_RX_CORE:-$DEFAULT_RDMA_CLIENT_RX_CORE}"
RDMA_CLIENT_TX_CORE="${DAQIRI_RDMA_CLIENT_TX_CORE:-$DEFAULT_RDMA_CLIENT_TX_CORE}"
RDMA_SERVER_RX_CORE="${DAQIRI_RDMA_SERVER_RX_CORE:-$DEFAULT_RDMA_SERVER_RX_CORE}"
RDMA_SERVER_TX_CORE="${DAQIRI_RDMA_SERVER_TX_CORE:-$DEFAULT_RDMA_SERVER_TX_CORE}"
DPDK_MEMORY_KIND="${DAQIRI_DPDK_MEMORY_KIND:-$DEFAULT_DPDK_MEMORY_KIND}"
DPDK_PAYLOAD_BATCHES="${DAQIRI_DPDK_PAYLOAD_BATCHES:-$DEFAULT_DPDK_PAYLOAD_BATCHES}"
DPDK_PAYLOAD_PACING_GBPS="${DAQIRI_DPDK_PAYLOAD_PACING_GBPS:-$DEFAULT_DPDK_PAYLOAD_PACING_GBPS}"
IBVERBS_MEMORY_KIND="${DAQIRI_IBVERBS_MEMORY_KIND:-$DEFAULT_IBVERBS_MEMORY_KIND}"
IBVERBS_BATCH_SIZE="${DAQIRI_IBVERBS_BATCH_SIZE:-$DEFAULT_IBVERBS_BATCH_SIZE}"
IBVERBS_NUM_BUFS="${DAQIRI_IBVERBS_NUM_BUFS:-$DEFAULT_IBVERBS_NUM_BUFS}"
IBVERBS_PAYLOAD_BATCHES="${DAQIRI_IBVERBS_PAYLOAD_BATCHES:-$DEFAULT_IBVERBS_PAYLOAD_BATCHES}"
IBVERBS_PAYLOAD_PACING_GBPS="${DAQIRI_IBVERBS_PAYLOAD_PACING_GBPS:-$DEFAULT_IBVERBS_PAYLOAD_PACING_GBPS}"
RDMA_MEMORY_KIND="${DAQIRI_RDMA_MEMORY_KIND:-$DEFAULT_RDMA_MEMORY_KIND}"
read -r -a SRV_PIN_CORES <<< "${DAQIRI_SOCKET_SERVER_CORES:-$DEFAULT_SOCKET_SERVER_CORES}"
read -r -a CLI_PIN_CORES <<< "${DAQIRI_SOCKET_CLIENT_CORES:-$DEFAULT_SOCKET_CLIENT_CORES}"
read -r -a DEFAULT_SOCKET_PAIR_COUNTS_ARRAY <<< "$DEFAULT_SOCKET_PAIR_COUNTS"
read -r -a DEFAULT_SOCKET_HEADLINE_PAIRS_ARRAY <<< "$DEFAULT_SOCKET_HEADLINE_PAIRS"
if (( ${#SRV_PIN_CORES[@]} == 0 || ${#SRV_PIN_CORES[@]} != ${#CLI_PIN_CORES[@]} )); then
  echo "Socket server/client core lists must be non-empty and have equal length" >&2
  exit 1
fi

BUILD_DIR="${DAQIRI_BUILD_DIR:-$SCRIPT_DIR/../build}"
# Shared production/benchmark configuration generator. It emits complete,
# independently runnable role configs from the cell's actual parameters.
CONFIG_GEN="$SCRIPT_DIR/../scripts/gen_daqiri_config.py"
if [[ ! -f "$CONFIG_GEN" ]]; then
  echo "Configuration generator not found: $CONFIG_GEN" >&2
  exit 1
fi
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$SCRIPT_DIR/../bench-results/$TS-$BACKEND-$MODE"
mkdir -p "$OUT_DIR"

python3 "$HARDWARE_RESOLVER" "${resolver_args[@]}" > "$OUT_DIR/resolved-hardware.json"

CSV="$OUT_DIR/runs.csv"
# `pairs` = number of concurrent client/server process pairs (socket backends sweep
# this; raw/RDMA are always 1). `gbps` is aggregate App TX, `rx_gbps` aggregate App RX
# (summed across pairs); App-level loss is (gbps - rx_gbps) / gbps.
# post_process_gemm_dim = GEMM_DIM pinned dimension (default 1024).
# CPU core/percentage columns identify the actual sampled cores. For a multi-pair
# socket run, TX and RX are pair-0 samples rather than aggregate utilization.
# post_process_sync (last column) = SYNC_INTERVAL, or "default" (2) when unset.
CSV_HEADER="platform,lang,backend,post_process,payload,batch,observed_max_rx_burst,pairs"
CSV_HEADER+=",target_gbps,rep,seconds,packets,bytes,pps,gbps,rx_gbps,drops,drops_kind"
CSV_HEADER+=",cpu_master_core,cpu_tx_core,cpu_rx_core"
CSV_HEADER+=",cpu_master_pct,cpu_tx_pct,cpu_rx_pct,gpu_sm_pct,gpu_mem_pct"
CSV_HEADER+=",post_process_gemm_dim,post_process_sync"
echo "$CSV_HEADER" > "$CSV"

# Capture slow-moving environment state once per result set.
"$SCRIPT_DIR/bench_capture_environment.sh" "$OUT_DIR"

RUN_SECONDS="${RUN_SECONDS:-30}"
if [[ ! "$RUN_SECONDS" =~ ^[0-9]+$ || "$RUN_SECONDS" -lt 1 ]]; then
  echo "Invalid RUN_SECONDS '$RUN_SECONDS' (expected a positive integer)" >&2; exit 1
fi
# Repeats per cell for error bars. Each rep is a full independent run with its own
# capture dir (<cell>-r<rep>) and CSV row; the perf-doc tables report mean +/- std
# across reps. Default 1; set REPEATS=3 for the published re-run.
REPEATS="${REPEATS:-1}"
# Representative GPU workload run on the REAL received data (after a reorder/
# gather step) in the receive path: none | fft | gemm (FP32 SGEMM) | gemm_fp16
# (mixed-precision FP16/tensor-core matmul, the inference-style GEMM). Recorded in
# the CSV post_process column. Honoured by ALL backends (dpdk, ibverbs, rdma,
# socket-udp, socket-tcp). Default none = bare loopback (no GPU compute).
WORKLOAD="${WORKLOAD:-none}"
case "$WORKLOAD" in
  none|fft|gemm|gemm_fp16) ;;
  *) echo "Invalid WORKLOAD '$WORKLOAD' (expected none|fft|gemm|gemm_fp16)" >&2; exit 1 ;;
esac
# GEMM_DIM: the square GEMM side length n (--workload-gemm-dim), held FIXED so the
# FLOP count per call (2·n³) is constant. The compute working set is n·n·elem_size,
# read from the front of each received I/O unit (RoCE message / raw reorder window),
# so that unit must be at least that large. Default 1024. Recorded in the CSV
# post_process_gemm_dim column.
GEMM_DIM="${GEMM_DIM:-1024}"
if [[ ! "$GEMM_DIM" =~ ^[1-9][0-9]*$ ]]; then
  echo "Invalid GEMM_DIM '$GEMM_DIM' (expected a positive integer)" >&2; exit 1
fi
# FFT_LEN: the 1-D C2C transform length (--workload-fft-len) for WORKLOAD=fft, held
# FIXED while the I/O unit is swept. Independent of GEMM_DIM. Default 1024.
FFT_LEN="${FFT_LEN:-1024}"
if [[ ! "$FFT_LEN" =~ ^[1-9][0-9]*$ ]]; then
  echo "Invalid FFT_LEN '$FFT_LEN' (expected a positive integer)" >&2; exit 1
fi
# SYNC_INTERVAL: drain the GPU stream every N compute calls (--workload-sync-interval).
# Larger N lets more GEMMs queue asynchronously before the single receive+compute
# thread blocks on the GPU, so sweeping it (e.g. 1 2 4 8 16 32) shows how much of the
# ceiling is CPU sync-stall vs the receive path. Empty = default (2). Recorded in the
# CSV post_process_sync column.
SYNC_INTERVAL="${SYNC_INTERVAL:-}"
if [[ -n "$SYNC_INTERVAL" && ! "$SYNC_INTERVAL" =~ ^[0-9]+$ ]]; then
  echo "Invalid SYNC_INTERVAL '$SYNC_INTERVAL' (expected a positive integer)" >&2; exit 1
fi
# MAX_INFLIGHT: for the RoCE event-recycling recv path, the max recv buffers with
# in-flight GPU work before the receive thread blocks on the oldest event
# (--workload-max-inflight). Larger absorbs more jitter before stalling reposts;
# clamped in the bench to the event pool (63) and rx_depth. Empty = bench default
# (min(rx_depth/2, 32)). Ignored by the run()+sync() DPDK/socket path.
MAX_INFLIGHT="${MAX_INFLIGHT:-}"
if [[ -n "$MAX_INFLIGHT" && ! "$MAX_INFLIGHT" =~ ^[0-9]+$ ]]; then
  echo "Invalid MAX_INFLIGHT '$MAX_INFLIGHT' (expected a positive integer)" >&2; exit 1
fi
SOCKET_RX_IO_PIN_CORES=()
if [[ -n "${SOCKET_RX_IO_CORES:-}" ]]; then
  read -r -a SOCKET_RX_IO_PIN_CORES <<< "$SOCKET_RX_IO_CORES"
  for core in "${SOCKET_RX_IO_PIN_CORES[@]}"; do
    if [[ ! "$core" =~ ^-1$|^[0-9]+$ ]]; then
      echo "Invalid SOCKET_RX_IO_CORES entry '$core' (expected -1 or a CPU index)" >&2
      exit 1
    fi
  done
fi
# NSYS: when set (NSYS=1), wrap the RoCE *server* (the receive + GPU-workload
# process) in `nsys profile` to capture the CUDA/GPU timeline and thread states,
# so we can see whether the GPU stream idles between GEMMs (receive-thread cadence
# gaps) or the SM is genuinely busy. Only the server is traced -- the client is a
# pure traffic generator. Per-cell report lands as <cell_dir>/roce_server.nsys-rep.
# Use `rdma smoke` + a short RUN_SECONDS so the report stays small.
NSYS="${NSYS:-}"
NSYS_BIN="${NSYS_BIN:-}"
if [[ -n "$NSYS" ]]; then
  if [[ -z "$NSYS_BIN" ]]; then
    # Resolve nsys: PATH first, then the usual CUDA toolkit location.
    if command -v nsys >/dev/null 2>&1; then
      NSYS_BIN="$(command -v nsys)"
    elif [[ -x /usr/local/cuda/bin/nsys ]]; then
      NSYS_BIN="/usr/local/cuda/bin/nsys"
    fi
  fi
  if [[ -z "$NSYS_BIN" || ! -x "$NSYS_BIN" ]]; then
    echo "NSYS set but no nsys binary found (looked on PATH and /usr/local/cuda/bin)." >&2
    echo "Set NSYS_BIN=/path/to/nsys explicitly." >&2
    exit 1
  fi
  echo "nsys profiling enabled: $NSYS_BIN" >&2
fi
DRIVER_LOG="$OUT_DIR/last_run.stderr"
FAILURES=0

# Per-backend sweep matrices (see the platform performance reports).
# Native-shape sizes are the leftmost entry; "matched 8K" cell is also included.
case "$BACKEND" in
  dpdk|ibverbs)
    PAYLOADS_SWEEP=(8000 4096 1024 256 64)
    PAYLOADS_HEADLINE=(8000)
    if [[ "$BACKEND" == "ibverbs" ]]; then
      BATCHES_SWEEP=("$IBVERBS_BATCH_SIZE")
      BATCHES_HEADLINE=("$IBVERBS_BATCH_SIZE")
    else
      BATCHES_SWEEP=(10240 4096 1024 256)
      BATCHES_HEADLINE=(10240)
    fi
    PAIRS_SWEEP=(1)
    PAIRS_HEADLINE=(1)
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
    CPU_MASTER="$MASTER_CORE"; CPU_TX="$DPDK_TX_QUEUE_CORE"; CPU_RX="$DPDK_RX_QUEUE_CORE"
    ETH_DST_ADDR="${ETH_DST_ADDR:-$DEFAULT_ETH_DST_ADDR}"
    ETH_SRC_ADDR="${ETH_SRC_ADDR:-$DEFAULT_ETH_SRC_ADDR}"
    # Resolve the tx_port (p0) / rx_port (p1) netdevs so each cell can assert wire
    # transit via their *_phy SerDes counters -- the MLX5 bifurcated driver keeps
    # these live even while the DPDK PMD owns the port, so a non-advancing
    # rx_packets_phy flags the on-chip eswitch short-cut instead of a true cable
    # loopback. Override DPDK_{TX,RX}_PCI / DPDK_{TX,RX}_NETDEV if auto-detect fails.
    DPDK_TX_PCI="${DPDK_TX_PCI:-$DEFAULT_DPDK_TX_PCI}"
    DPDK_RX_PCI="${DPDK_RX_PCI:-$DEFAULT_DPDK_RX_PCI}"
    DPDK_TX_NETDEV="${DPDK_TX_NETDEV:-$DEFAULT_DPDK_TX_NETDEV}"
    DPDK_RX_NETDEV="${DPDK_RX_NETDEV:-$DEFAULT_DPDK_RX_NETDEV}"
    ;;
  rdma)
    PAYLOADS_SWEEP=(8000000 1048576 65536 8192 4096)
    BATCHES_SWEEP=(1)
    PAYLOADS_HEADLINE=(8000000)
    BATCHES_HEADLINE=(1)
    PAIRS_SWEEP=(1)
    PAIRS_HEADLINE=(1)
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_rdma"
    # One-way roles: sample the client TX queue and the server RX queue/worker so
    # cpu_tx_pct and cpu_rx_pct reflect the active RoCE RC data path.
    CPU_MASTER="$MASTER_CORE"; CPU_TX="$RDMA_CLIENT_TX_CORE"; CPU_RX="$RDMA_SERVER_RX_CORE"
    # Resolve the server (RX) / client (TX) netdevs inside the wire-loopback
    # namespaces so each cell can assert wire transit via *_phy SerDes counters,
    # exactly like the dpdk path. RoCE loops over the SAME cable, so a non-advancing
    # server rx_packets_phy flags the on-chip eswitch short-cut instead of a true
    # over-the-cable loopback -- the tell for a RoCE number above the ~99 Gb/s
    # 100GbE line-rate ceiling. Empty if the netns is not up yet (the per-cell check
    # then just skips with a WARN). Override RDMA_{SERVER,CLIENT}_NETDEV if needed.
    RDMA_SERVER_NS="${RDMA_SERVER_NS:-$SERVER_NS}"
    RDMA_CLIENT_NS="${RDMA_CLIENT_NS:-$CLIENT_NS}"
    RDMA_SERVER_NETDEV="${RDMA_SERVER_NETDEV:-$(ip netns exec "$RDMA_SERVER_NS" ls /sys/class/net 2>/dev/null | grep -vx lo | head -n1 || true)}"
    RDMA_CLIENT_NETDEV="${RDMA_CLIENT_NETDEV:-$(ip netns exec "$RDMA_CLIENT_NS" ls /sys/class/net 2>/dev/null | grep -vx lo | head -n1 || true)}"
    ;;
    # Single-frame UDP sizes (<= the ~8972 B MTU payload, so no IP fragmentation).
    # 65507 is intentionally excluded: it fragments into ~8 packets and, under
    # multi-pair unpaced load, reassembly collapses out of the shared per-namespace
    # pool -- not a meaningful steady-state operating point. The netns YAMLs still
    # carry a large buf_size so message_size never overflows the TX buffer.
  socket-udp)
    PAYLOADS_SWEEP=(8000 1000)
    BATCHES_SWEEP=(32)
    PAYLOADS_HEADLINE=(8000)
    BATCHES_HEADLINE=(32)
    # Concurrent client/server pairs. A single pair is core-bound well below line
    # rate; the published matrix scales aggregate throughput with four pairs.
    PAIRS_SWEEP=("${DEFAULT_SOCKET_PAIR_COUNTS_ARRAY[@]}")
    PAIRS_HEADLINE=("${DEFAULT_SOCKET_HEADLINE_PAIRS_ARRAY[@]}")
    SRV_PORT_BASE=5001; CLI_PORT_BASE=5101
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    # Final pair-0 TX/RX attribution is derived from the structured pinning below.
    CPU_MASTER="$MASTER_CORE"; CPU_TX="${CLI_PIN_CORES[0]}"; CPU_RX="${SRV_PIN_CORES[0]}"
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
    PAIRS_SWEEP=("${DEFAULT_SOCKET_PAIR_COUNTS_ARRAY[@]}")
    PAIRS_HEADLINE=("${DEFAULT_SOCKET_HEADLINE_PAIRS_ARRAY[@]}")
    SRV_PORT_BASE=6001; CLI_PORT_BASE=6101
    BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_socket"
    # Final pair-0 TX/RX attribution is derived from the structured pinning below.
    CPU_MASTER="$MASTER_CORE"; CPU_TX="${CLI_PIN_CORES[0]}"; CPU_RX="${SRV_PIN_CORES[0]}"
    ;;
  *) echo "Unknown backend: $BACKEND" >&2; exit 1 ;;
esac

if [[ "$BACKEND" == "rdma" || "$BACKEND" =~ ^socket- ]]; then
  ip netns list 2>/dev/null | grep -qw "$CLIENT_NS" || {
    echo "Missing namespace $CLIENT_NS; run scripts/setup_cabled_loopback_netns.sh --platform $BENCH_PLATFORM up" >&2
    exit 1
  }
  ip netns list 2>/dev/null | grep -qw "$SERVER_NS" || {
    echo "Missing namespace $SERVER_NS; run scripts/setup_cabled_loopback_netns.sh --platform $BENCH_PLATFORM up" >&2
    exit 1
  }
  NETNS_CLIENT_NETDEV="$DEFAULT_DPDK_TX_NETDEV"
  NETNS_SERVER_NETDEV="$DEFAULT_DPDK_RX_NETDEV"
  [[ -n "$NETNS_CLIENT_NETDEV" && -n "$NETNS_SERVER_NETDEV" ]] || {
    echo "Could not resolve cabled netdevs in $CLIENT_NS and $SERVER_NS" >&2
    exit 1
  }
fi

# When a GPU workload is active, its pinned GEMM operand (n·n·elem_size, from
# GEMM_DIM) must fit inside each received I/O unit -- the small entries in the
# default payload sweep can't hold it. Restrict the sweep to the headline
# (native-shape) size so the fixed-n comparison is a single clean point per
# backend instead of silently disabling the workload on the small cells.
# e.g. WORKLOAD=gemm_fp16 GEMM_DIM=1024 REPEATS=3 ./run_cabled_loopback_bench.sh --platform igx-thor rdma sweep
if [[ "$WORKLOAD" != "none" ]]; then
  PAYLOADS_SWEEP=("${PAYLOADS_HEADLINE[@]}")
fi
# Optional space-separated overrides for one-off experiments. Apply them to both
# sweep and headline modes so smoke and drop-curve runs use the requested values.
if [[ -n "${PAYLOADS_OVERRIDE:-}" ]]; then
  read -r -a PAYLOADS_SWEEP <<< "$PAYLOADS_OVERRIDE"
  if (( ${#PAYLOADS_SWEEP[@]} == 0 )); then
    echo "PAYLOADS_OVERRIDE must contain at least one payload size" >&2
    exit 1
  fi
  PAYLOADS_HEADLINE=("${PAYLOADS_SWEEP[@]}")
  for payload in "${PAYLOADS_SWEEP[@]}"; do
    if [[ ! "$payload" =~ ^[1-9][0-9]*$ ]]; then
      echo "Invalid PAYLOADS_OVERRIDE entry '$payload' (expected a positive integer)" >&2
      exit 1
    fi
  done
fi
if [[ -n "${BATCHES_OVERRIDE:-}" ]]; then
  read -r -a BATCHES_SWEEP <<< "$BATCHES_OVERRIDE"
  if (( ${#BATCHES_SWEEP[@]} == 0 )); then
    echo "BATCHES_OVERRIDE must contain at least one batch size" >&2
    exit 1
  fi
  BATCHES_HEADLINE=("${BATCHES_SWEEP[@]}")
  for batch in "${BATCHES_SWEEP[@]}"; do
    if [[ ! "$batch" =~ ^[1-9][0-9]*$ ]]; then
      echo "Invalid BATCHES_OVERRIDE entry '$batch' (expected a positive integer)" >&2
      exit 1
    fi
    if [[ "$BACKEND" == "socket-udp" && "$batch" -gt 32 ]]; then
      echo "Invalid UDP batch size '$batch' (expected 1-32)" >&2
      exit 1
    fi
  done
fi
if [[ -n "${PAIRS_OVERRIDE:-}" ]]; then
  if [[ ! "$BACKEND" =~ ^socket- ]]; then
    echo "PAIRS_OVERRIDE is supported only for socket backends" >&2
    exit 1
  fi
  read -r -a PAIRS_SWEEP <<< "$PAIRS_OVERRIDE"
  if (( ${#PAIRS_SWEEP[@]} == 0 )); then
    echo "PAIRS_OVERRIDE must contain at least one pair count" >&2
    exit 1
  fi
  PAIRS_HEADLINE=("${PAIRS_SWEEP[@]}")
  for pairs in "${PAIRS_SWEEP[@]}"; do
    if [[ ! "$pairs" =~ ^[1-9][0-9]*$ ]]; then
      echo "Invalid PAIRS_OVERRIDE entry '$pairs' (expected a positive integer)" >&2
      exit 1
    fi
  done
fi

# All backends (dpdk, rdma, socket-udp, socket-tcp) now run the workload on real
# received data, so the CSV post_process column records the requested workload
# for every backend.
WORKLOAD_EFF="$WORKLOAD"

DROP_CURVE_TARGETS=(1 5 10 25 50 75 100 0)  # 0 means unpaced (line rate)
if [[ -n "${DROP_CURVE_TARGETS_OVERRIDE:-}" ]]; then
  read -r -a DROP_CURVE_TARGETS <<< "$DROP_CURVE_TARGETS_OVERRIDE"
fi

preflight_core() {
  local core="$1"
  [[ -d "/sys/devices/system/cpu/cpu$core" ]] || {
    echo "Configured CPU $core does not exist" >&2
    return 1
  }
  if [[ -r "/sys/devices/system/cpu/cpu$core/online" ]] &&
      [[ "$(cat "/sys/devices/system/cpu/cpu$core/online")" != "1" ]]; then
    echo "Configured CPU $core is offline" >&2
    return 1
  fi
}

preflight_netdev() {
  local namespace="$1" netdev="$2"
  local command_prefix=()
  [[ -n "$namespace" ]] && command_prefix=(ip netns exec "$namespace")
  local carrier mtu speed pause
  carrier="$("${command_prefix[@]}" cat "/sys/class/net/$netdev/carrier" 2>/dev/null || echo 0)"
  mtu="$("${command_prefix[@]}" cat "/sys/class/net/$netdev/mtu" 2>/dev/null || echo 0)"
  speed="$("${command_prefix[@]}" ethtool "$netdev" 2>/dev/null | awk '/Speed:/ {gsub(/Mb\/s/, "", $2); print $2}')"
  pause="$("${command_prefix[@]}" ethtool --show-pause "$netdev" 2>/dev/null || true)"
  [[ "$carrier" == "1" ]] || { echo "$netdev has no carrier" >&2; return 1; }
  [[ "$mtu" == "9000" ]] || { echo "$netdev MTU is $mtu, expected 9000" >&2; return 1; }
  [[ "$speed" == "$EXPECTED_LINK_MBPS" ]] || {
    echo "$netdev link speed is ${speed:-unknown} Mb/s, expected $EXPECTED_LINK_MBPS" >&2
    return 1
  }
  if grep -Eq '^(RX|TX):[[:space:]]+on$' <<< "$pause"; then
    echo "$netdev has Ethernet pause enabled" >&2
    return 1
  fi
}

preflight_hardware() {
  [[ "$(id -u)" -eq 0 ]] || { echo "Run the benchmark harness as root" >&2; return 1; }
  nvidia-smi -L | grep -Fq "$GPU_MONITOR_ID" || {
    echo "Selected GPU UUID $GPU_MONITOR_ID is not visible" >&2
    return 1
  }
  local cores=("$MASTER_CORE")
  case "$BACKEND" in
    dpdk|ibverbs)
      cores+=("$DPDK_TX_QUEUE_CORE" "$DPDK_RX_QUEUE_CORE" "$DPDK_TX_WORKER_CORE" "$DPDK_RX_WORKER_CORE")
      preflight_netdev "" "$DPDK_TX_NETDEV" || return 1
      preflight_netdev "" "$DPDK_RX_NETDEV" || return 1
      ;;
    rdma)
      cores+=("$RDMA_CLIENT_RX_CORE" "$RDMA_CLIENT_TX_CORE" "$RDMA_SERVER_RX_CORE" "$RDMA_SERVER_TX_CORE")
      preflight_netdev "$CLIENT_NS" "$NETNS_CLIENT_NETDEV" || return 1
      preflight_netdev "$SERVER_NS" "$NETNS_SERVER_NETDEV" || return 1
      ;;
    socket-udp|socket-tcp)
      cores+=("${SRV_PIN_CORES[@]}" "${CLI_PIN_CORES[@]}")
      preflight_netdev "$CLIENT_NS" "$NETNS_CLIENT_NETDEV" || return 1
      preflight_netdev "$SERVER_NS" "$NETNS_SERVER_NETDEV" || return 1
      ;;
  esac
  local core
  for core in "${cores[@]}"; do
    preflight_core "$core" || return 1
  done
}

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

# Read a scalar field from a `key=value` style stdout line.
# usage: extract_field <pattern-prefix> <field-name> <file>
extract_field() {
  local prefix="$1" field="$2" file="$3"
  grep -E "^$prefix" "$file" | tail -n1 | grep -oE " $field=[^ ]+" | head -n1 | sed -E "s/.*$field=//"
}

# Sum DPDK drop counters from the engine log emitted via DAQIRI_LOG_INFO.
parse_dpdk_drops() {
  local log="$1"
  local sum=0 v
  for key in imissed ierrors rx_nombuf; do
    v="$(grep -oE "$key=[0-9]+" "$log" 2>/dev/null | tail -n1 | sed -E "s/.*=//" || true)"
    [[ -n "${v:-}" ]] && sum=$((sum + v))
  done
  echo "$sum"
}

parse_ibverbs_drops() {
  local log="$1"
  awk '
    /ibverbs RX/ {
      if (match($0, /cqe_errors=[0-9]+/)) {
        value = substr($0, RSTART + 11, RLENGTH - 11); sum += value
      }
      if (match($0, /app_ring_full_drops=[0-9]+ bursts \([0-9]+ pkts\)/)) {
        text = substr($0, RSTART, RLENGTH)
        sub(/^.*\(/, "", text); sub(/ pkts\)$/, "", text); sum += text
      }
    }
    /ibverbs TX/ && match($0, /handoff_full_drops=[0-9]+ bursts \([0-9]+ pkts\)/) {
      text = substr($0, RSTART, RLENGTH)
      sub(/^.*\(/, "", text); sub(/ pkts\)$/, "", text); sum += text
    }
    END { print sum + 0 }
  ' "$log"
}

# Count RDMA CQ errors in the engine log.
parse_rdma_drops() {
  local log="$1"
  # `grep -c` already prints 0 on no match (and exits 1); the old `|| echo 0`
  # appended a SECOND line, embedding a newline in the CSV drops field and
  # wrapping every rdma row. Capture the count and swallow the exit code instead.
  local n; n="$(grep -c 'CQ error' "$log" 2>/dev/null)" || true
  echo "${n:-0}"
}

# Snapshot persistent kernel protocol counters. Reading /proc/net/udp after the
# benchmark is invalid because the server socket has already closed and its row,
# including the per-socket drop count, no longer exists. Namespace nstat counters
# survive socket teardown and can be differenced around the complete cell.
snapshot_udp_nstat() {
  local ns="${1:-}"
  local pre=()
  [[ -n "$ns" ]] && pre=(ip netns exec "$ns")
  "${pre[@]}" nstat -a 2>/dev/null | awk '$1 == "UdpInErrors" { print $2+0; found=1 } END { if (!found) print 0 }' || echo 0
}
snapshot_tcp_nstat() {
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
  if [[ ! "$cpu_idx" =~ ^[0-9]+$ ]]; then
    echo "nan"
    return
  fi
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

# Sum a NIC *_phy SerDes counter (proves traffic crossed the cable, not an on-chip
# eswitch short-cut). Empty netdev -> 0. Mirrors run_spark_mq_bench.sh's phy check.
# Optional third arg is a network namespace (the RoCE/socket wire loopback moves the
# cabled netdevs into dq_wire_{server,client}); empty reads the current namespace.
phy_counter() {
  local netdev="$1" key="$2" ns="${3:-}"
  [[ -z "$netdev" ]] && { echo 0; return; }
  { if [[ -n "$ns" ]]; then ip netns exec "$ns" ethtool -S "$netdev" 2>/dev/null
    else ethtool -S "$netdev" 2>/dev/null; fi; } \
    | awk -F'[: ]+' -v k="$key" '$2 == k { s += $3 } END { printf "%d", s+0 }'
}

priority_buffer_discards() {
  local netdev="$1"
  ethtool -S "$netdev" 2>/dev/null \
    | awk -F'[: ]+' '$2 ~ /^rx_prio[0-9]+_buf_discard(_packets)?$/ { s += $3 } END { printf "%d", s+0 }'
}

# Generate a complete config from the cell parameters (raw + RoCE; sockets use
# generate_socket_yaml so each concurrent pair gets unique ports/cores).
generate_yaml() {
  local out="$1" payload="$2" batch="$3" target_gbps="$4"
  case "$BACKEND" in
    dpdk|ibverbs)
      # DPDK retains the report's 8064-byte capacity; ibverbs right-sizes MPRQ strides.
      local raw_memory_kind="$DPDK_MEMORY_KIND"
      local raw_num_bufs=51200
      local raw_buffer_size=8064
      local raw_source_args=()
      local raw_pacing_args=()
      [[ "$target_gbps" != "0" ]] && raw_pacing_args=(--pacing-mbps "$((target_gbps * 1000))")
      if [[ "$BACKEND" == "ibverbs" ]]; then
        raw_memory_kind="$IBVERBS_MEMORY_KIND"
        raw_num_bufs="$IBVERBS_NUM_BUFS"
        raw_buffer_size=$((payload + 64))
        (( raw_buffer_size < 256 )) && raw_buffer_size=256
        raw_source_args=(--eth-src-addr "$ETH_SRC_ADDR")
      fi
      python3 "$CONFIG_GEN" raw-pair \
        --tx-address "$DPDK_TX_PCI" --rx-address "$DPDK_RX_PCI" \
        --master-core "$MASTER_CORE" --engine "$BACKEND" --memory-kind "$raw_memory_kind" \
        --tx-queue-cores "$DPDK_TX_QUEUE_CORE" --rx-queue-cores "$DPDK_RX_QUEUE_CORE" \
        --tx-worker-cores "$DPDK_TX_WORKER_CORE" --rx-worker-cores "$DPDK_RX_WORKER_CORE" \
        --payload-size "$payload" --buffer-size "$raw_buffer_size" \
        --batch-size "$batch" --num-bufs "$raw_num_bufs" \
        "${raw_pacing_args[@]}" \
        "${raw_source_args[@]}" \
        --eth-dst-addr "$ETH_DST_ADDR" \
        --ip-src-addr 1.1.1.1 --ip-dst-addr 2.2.2.2 \
        --output "$out" || return 1
      ;;
    rdma)
      # Size the flow-control window per message size (PR #144). buf_size tracks the
      # payload, and num_bufs / {rx,tx}_depth scale up to the PR-recommended window
      # (rx 512 / tx 128) but are capped so num_bufs * buf_size stays within a memory
      # budget per region -- small messages get the full deep window (where RNR NACKs
      # bite), large messages stay memory-bounded (where window depth is irrelevant).
      # Overridable for the buffer-depth experiment: RDMA_BUDGET_GIB raises the
      # per-region pinned-memory cap, RDMA_RX_NB / RDMA_TX_NB raise the window
      # ceilings (num_bufs + {rx,tx}_depth). Defaults reproduce the shipped sizing
      # (1 GiB, rx 512 / tx 128).
      local budget=$(( ${RDMA_BUDGET_GIB:-1} * 1073741824 ))   # pinned per memory region
      local cap=$(( budget / payload )); (( cap < 1 )) && cap=1
      local rx_nb=${RDMA_RX_NB:-512}; (( rx_nb > cap )) && rx_nb=$cap
      local tx_nb=${RDMA_TX_NB:-128}; (( tx_nb > cap )) && tx_nb=$cap
      # Direction-specific region counts and queue depths keep the flow-control
      # window within the buffers that back it. Server -> $out; client -> sibling.
      python3 "$CONFIG_GEN" socket-pair --transport roce \
        --client-address 10.250.0.1 --server-address 10.250.0.2 \
        --client-port 4096 --server-port 4096 \
        --client-master-core "$MASTER_CORE" --server-master-core "$MASTER_CORE" \
        --client-rx-core "$RDMA_CLIENT_RX_CORE" --client-tx-core "$RDMA_CLIENT_TX_CORE" \
        --server-rx-core "$RDMA_SERVER_RX_CORE" --server-tx-core "$RDMA_SERVER_TX_CORE" \
        --client-worker-core "$RDMA_CLIENT_RX_CORE" --server-worker-core "$RDMA_SERVER_RX_CORE" \
        --message-size "$payload" --buffer-size "$payload" --num-bufs 1 \
        --rx-num-bufs "$rx_nb" --tx-num-bufs "$tx_nb" \
        --rx-depth "$rx_nb" --tx-depth "$tx_nb" --memory-kind "$RDMA_MEMORY_KIND" \
        --role rx --output "$out" || return 1
      python3 "$CONFIG_GEN" socket-pair --transport roce \
        --client-address 10.250.0.1 --server-address 10.250.0.2 \
        --client-port 4096 --server-port 4096 \
        --client-master-core "$MASTER_CORE" --server-master-core "$MASTER_CORE" \
        --client-rx-core "$RDMA_CLIENT_RX_CORE" --client-tx-core "$RDMA_CLIENT_TX_CORE" \
        --server-rx-core "$RDMA_SERVER_RX_CORE" --server-tx-core "$RDMA_SERVER_TX_CORE" \
        --client-worker-core "$RDMA_CLIENT_RX_CORE" --server-worker-core "$RDMA_SERVER_RX_CORE" \
        --message-size "$payload" --buffer-size "$payload" --num-bufs 1 \
        --rx-num-bufs "$rx_nb" --tx-num-bufs "$tx_nb" \
        --rx-depth "$rx_nb" --tx-depth "$tx_nb" --memory-kind "$RDMA_MEMORY_KIND" \
        --role tx --output "${out%.yaml}_client.yaml" || return 1
      ;;
  esac
}

# Separate isolated cores for the send (client) and receive (server) sides of each
# socket pair, so a pair's two processes never time-slice one CPU. Co-locating them
# (the old "one core per pair" setup) forces a send/recv ping-pong on a single core
# that, at mid-size messages (e.g. 8000 B), makes a single TCP stream metastable --
# it locks into an efficient (~30 Gb/s) or a serialized (~15 Gb/s) mode for a whole
# run, producing bimodal, high-variance results.
#
# Keep each pair's two cores in the same CPU cluster when the platform has more
# than one. The platform profile supplies the default pair ordering; environment
# overrides support additional systems without changing the benchmark matrix.
pair_server_core() { echo "${SRV_PIN_CORES[$(( $1 % ${#SRV_PIN_CORES[@]} ))]}"; }
pair_client_core() { echo "${CLI_PIN_CORES[$(( $1 % ${#CLI_PIN_CORES[@]} ))]}"; }
pair_server_io_core() {
  local idx="$1" fallback="$2"
  if (( ${#SOCKET_RX_IO_PIN_CORES[@]} == 0 )); then
    echo "$fallback"
  else
    echo "${SOCKET_RX_IO_PIN_CORES[$(( idx % ${#SOCKET_RX_IO_PIN_CORES[@]} ))]}"
  fi
}

# Socket CSV utilization samples pair 0. Attribute TX to the client application
# worker and RX to the actual server TCP recv()/UDP recvmmsg() I/O thread. An
# unpinned run has no meaningful core sample.
if [[ "$BACKEND" =~ ^socket- ]]; then
  if [[ -n "${SOCKET_NOPIN:-}" ]]; then
    CPU_TX=-1
    CPU_RX=-1
  else
    CPU_TX="$(pair_client_core 0)"
    CPU_RX="$(pair_server_io_core 0 "$(pair_server_core 0)")"
  fi
fi

# Write a complete server/client YAML pair with unique ports and independent
# server/client placement, so the pair does not time-slice one CPU.
generate_socket_yaml() {
  local idx="$1" payload="$2" batch="$3" server_out="$4" client_out="$5"
  local srv_port=$(( SRV_PORT_BASE + idx ))
  local cli_port=$(( CLI_PORT_BASE + idx ))
  # SOCKET_NOPIN=1 runs the bench workers unpinned (cpu_core -1 -> no affinity, the
  # scheduler places them). For gathering the pinned-vs-non-pinned comparison only;
  # the published report stays pinned like the other backends.
  local server_core client_core server_io_core
  if [[ -n "${SOCKET_NOPIN:-}" ]]; then
    server_core=-1; client_core=-1; server_io_core=-1
  else
    server_core="$(pair_server_core "$idx")"
    client_core="$(pair_client_core "$idx")"
    server_io_core="$(pair_server_io_core "$idx" "$server_core")"
  fi
  local transport="${BACKEND#socket-}"
  local buffer_size=65536 num_bufs=1024
  if [[ "$transport" == "tcp" ]]; then
    buffer_size=1048576
    num_bufs=64
  fi
  local common_args=(
    --transport "$transport"
    --client-address 10.250.0.1 --server-address 10.250.0.2
    --client-port "$cli_port" --server-port "$srv_port"
    --client-master-core "$MASTER_CORE" --server-master-core "$MASTER_CORE"
    --client-rx-core "$client_core" --client-tx-core "$client_core"
    --server-rx-core "$server_io_core" --server-tx-core "$server_core"
    --client-worker-core "$client_core" --server-worker-core "$server_core"
    --message-size "$payload" --buffer-size "$buffer_size"
    --num-bufs "$num_bufs" --rx-batch-size "$batch"
  )
  python3 "$CONFIG_GEN" socket-pair "${common_args[@]}" --role rx \
    --output "$server_out" || return 1
  python3 "$CONFIG_GEN" socket-pair "${common_args[@]}" --role tx \
    --output "$client_out" || return 1
}

# Run one cell. Echoes the CSV row to stdout.
run_cell() {
  local lang="$1" payload="$2" batch="$3" pairs="$4" target_gbps="$5" rep="${6:-1}"
  local cell="$lang-$BACKEND-p$payload-b$batch-n$pairs-g$target_gbps-r$rep"
  local cell_dir="$OUT_DIR/$cell"
  mkdir -p "$cell_dir"

  # Generate every input before starting monitors or benchmark processes. A
  # failed generator must fail this cell rather than launching with missing or
  # partially written YAML.
  local yaml="" i
  if [[ "$BACKEND" =~ ^socket- ]]; then
    for ((i = 0; i < pairs; i++)); do
      if ! generate_socket_yaml "$i" "$payload" "$batch" \
          "$cell_dir/server_p$i.yaml" "$cell_dir/client_p$i.yaml"; then
        echo "ERROR: $cell configuration generation failed for socket pair $i" >&2
        return 1
      fi
    done
  else
    yaml="$cell_dir/config.yaml"
    if ! generate_yaml "$yaml" "$payload" "$batch" "$target_gbps"; then
      echo "ERROR: $cell configuration generation failed" >&2
      return 1
    fi
  fi

  # Snapshot kernel-side drop counters. In the netns wire loopback the UDP
  # receiver lives in the server netns and TCP retransmits are counted on the
  # client (sender) netns, so read each counter inside the relevant namespace.
  local udp_ns="" tcp_ns=""
  [[ "$BACKEND" == "socket-udp" ]] && udp_ns="$SERVER_NS"
  [[ "$BACKEND" == "socket-tcp" ]] && tcp_ns="$CLIENT_NS"
  local udp_before tcp_before
  udp_before="$(snapshot_udp_nstat "$udp_ns")"
  tcp_before="$(snapshot_tcp_nstat "$tcp_ns")"
  local raw_priority_drop_before=0 raw_priority_drops=0
  if [[ "$BACKEND" == "dpdk" || "$BACKEND" == "ibverbs" ]]; then
    raw_priority_drop_before="$(priority_buffer_discards "$DPDK_RX_NETDEV")"
  fi

  # Snapshot per-cpu stats just before the bench starts.
  snapshot_cpu_stat "$cell_dir/cpu_stat.before"

  # Background GPU dmon (1-sec sample, RUN_SECONDS samples).
  local dmon_args=(-s pucvmet -c "$RUN_SECONDS")
  [[ -n "$GPU_MONITOR_ID" ]] && dmon_args+=(-i "$GPU_MONITOR_ID")
  ( nvidia-smi dmon "${dmon_args[@]}" > "$cell_dir/nvidia_smi_dmon.txt" 2>&1 ) &
  local dmon_pid=$!

  local perf_netdev=""
  local perf_prefix=()
  if [[ "$BACKEND" == "rdma" || "$BACKEND" =~ ^socket- ]]; then
    perf_netdev="$NETNS_SERVER_NETDEV"
    perf_prefix=(ip netns exec "$SERVER_NS")
  else
    perf_netdev="$DPDK_RX_NETDEV"
  fi
  ( "${perf_prefix[@]}" mlnx_perf -i "$perf_netdev" -t 1 \
      > "$cell_dir/mlnx_perf_rx.txt" 2>&1 ) &
  local mlnx_perf_pid=$!

  # Run the bench. Stderr captures DAQIRI_LOG_* output (DPDK/RDMA drop sources).
  local stdout="$cell_dir/stdout.txt"
  local stderr="$cell_dir/stderr.txt"
  local bench_rc=0
  local pkts="" bytes="" secs="" rx_pkts="" rx_bytes="" observed_max_rx_burst=0

  local bench_extra=()
  if [[ "$target_gbps" != "0" && "$BACKEND" != "dpdk" && "$BACKEND" != "ibverbs" ]]; then
    bench_extra+=(--target-gbps "$target_gbps")
  fi
  # Every backend honours --workload (runs it on real received data); none = skip.
  if [[ "$WORKLOAD_EFF" != "none" ]]; then
    bench_extra+=(--workload "$WORKLOAD_EFF")
    bench_extra+=(--workload-gemm-dim "$GEMM_DIM")
    bench_extra+=(--workload-fft-len "$FFT_LEN")
    [[ -n "$SYNC_INTERVAL" ]] && bench_extra+=(--workload-sync-interval "$SYNC_INTERVAL")
    [[ -n "$MAX_INFLIGHT" ]] && bench_extra+=(--workload-max-inflight "$MAX_INFLIGHT")
  fi
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
    # rate; the published matrix scales aggregate throughput with four pairs.
    # App TX (client sent) and App RX (server recv) are summed across pairs.
    local wire_tx_before wire_rx_before
    wire_tx_before="$(phy_counter "$NETNS_CLIENT_NETDEV" tx_bytes_phy "$CLIENT_NS")"
    wire_rx_before="$(phy_counter "$NETNS_SERVER_NETDEV" rx_bytes_phy "$SERVER_NS")"
    local server_pids=() client_pids=()
    for ((i = 0; i < pairs; i++)); do
      ip netns exec "$SERVER_NS" "$BENCH_BIN" "$cell_dir/server_p$i.yaml" \
          --seconds "$server_seconds" "${bench_extra[@]}" --mode server \
          > "$cell_dir/server_p$i.stdout" 2> "$cell_dir/server_p$i.stderr" &
      server_pids+=("$!")
    done
    sleep "$startup_sleep"
    for ((i = 0; i < pairs; i++)); do
      ip netns exec "$CLIENT_NS" "$BENCH_BIN" "$cell_dir/client_p$i.yaml" \
          --seconds "$RUN_SECONDS" "${bench_extra[@]}" --mode client \
          > "$cell_dir/client_p$i.stdout" 2> "$cell_dir/client_p$i.stderr" &
      client_pids+=("$!")
    done
    for i in "${client_pids[@]}"; do wait "$i" || bench_rc=$?; done
    for i in "${server_pids[@]}"; do wait "$i" 2>/dev/null || true; done

    local tx_pkts=0 tx_bytes=0 agg_rx_pkts=0 agg_rx_bytes=0 max_secs=0
    for ((i = 0; i < pairs; i++)); do
      local sp sb se rp rb max_burst
      sp="$(extract_field 'Client complete' sent_packets "$cell_dir/client_p$i.stdout")"
      sb="$(extract_field 'Client complete' sent_bytes   "$cell_dir/client_p$i.stdout")"
      se="$(extract_field 'Client complete' seconds      "$cell_dir/client_p$i.stdout")"
      rp="$(extract_field 'Server complete' recv_packets "$cell_dir/server_p$i.stdout")"
      rb="$(extract_field 'Server complete' recv_bytes   "$cell_dir/server_p$i.stdout")"
      max_burst="$(extract_field 'Server complete' max_rx_burst \
        "$cell_dir/server_p$i.stdout")"
      tx_pkts=$(( tx_pkts + ${sp:-0} ))
      tx_bytes=$(( tx_bytes + ${sb:-0} ))
      agg_rx_pkts=$(( agg_rx_pkts + ${rp:-0} ))
      agg_rx_bytes=$(( agg_rx_bytes + ${rb:-0} ))
      if (( ${max_burst:-0} > observed_max_rx_burst )); then
        observed_max_rx_burst="${max_burst:-0}"
      fi
      max_secs="$(awk -v a="$max_secs" -v b="${se:-0}" 'BEGIN { print (b+0>a+0)?b:a }')"
    done
    pkts="$tx_pkts"; bytes="$tx_bytes"; rx_pkts="$agg_rx_pkts"; rx_bytes="$agg_rx_bytes"; secs="$max_secs"
    local wire_tx_delta wire_rx_delta wire_min
    wire_tx_delta=$(( $(phy_counter "$NETNS_CLIENT_NETDEV" tx_bytes_phy "$CLIENT_NS") - wire_tx_before ))
    wire_rx_delta=$(( $(phy_counter "$NETNS_SERVER_NETDEV" rx_bytes_phy "$SERVER_NS") - wire_rx_before ))
    wire_min=$(( agg_rx_bytes / 2 ))
    if [[ "$wire_rx_delta" -lt "$wire_min" || "$wire_rx_delta" -le 0 ]]; then
      echo "ERROR: $cell rx_bytes_phy advanced only +$wire_rx_delta on $NETNS_SERVER_NETDEV (expected >= ~$wire_min)" >&2
      bench_rc=90
    else
      echo "INFO: $cell wire OK -- client tx_bytes_phy +$wire_tx_delta, server rx_bytes_phy +$wire_rx_delta" >&2
    fi
    cat "$cell_dir"/server_p*.stderr "$cell_dir"/client_p*.stderr > "$stderr" 2>/dev/null || true
    cat "$cell_dir"/client_p*.stdout "$cell_dir"/server_p*.stdout > "$stdout" 2>/dev/null || true
  elif [[ "$BACKEND" == "rdma" ]]; then
    # Split server/client processes in separate namespaces so RDMA-CM resolves
    # addresses over the wire rather than short-cutting the kernel's local table.
    # Optionally wrap the server (receive + GPU-workload) in nsys. Placed AFTER the
    # netns exec so nsys traces the bench, not `ip netns exec`.
    local -a nsys_pre=()
    if [[ -n "$NSYS" ]]; then
      nsys_pre=("$NSYS_BIN" profile --trace=cuda,osrt --sample=cpu --cpuctxsw=process-tree
                --force-overwrite=true --output "$cell_dir/roce_server")
    fi
    # Snapshot the netns *_phy counters around the run to assert the RoCE traffic
    # actually crossed the cable (client tx -> server rx over the wire), not the
    # on-chip eswitch short-cut that lets a loopback exceed the 100GbE line rate.
    local phy_tx_before phy_rx_before
    phy_tx_before="$(phy_counter "$RDMA_CLIENT_NETDEV" tx_packets_phy "$RDMA_CLIENT_NS")"
    phy_rx_before="$(phy_counter "$RDMA_SERVER_NETDEV" rx_packets_phy "$RDMA_SERVER_NS")"
    ip netns exec "$SERVER_NS" "${nsys_pre[@]}" "$BENCH_BIN" "$yaml" \
        --seconds "$server_seconds" "${bench_extra[@]}" --mode server \
        > "$cell_dir/server_stdout.txt" 2> "$cell_dir/server_stderr.txt" &
    local server_pid=$!
    sleep "$startup_sleep"
    ip netns exec "$CLIENT_NS" "$BENCH_BIN" "${yaml%.yaml}_client.yaml" \
        --seconds "$RUN_SECONDS" "${bench_extra[@]}" --mode client \
        > "$stdout" 2> "$stderr" || bench_rc=$?
    wait "$server_pid" 2>/dev/null || true
    cat "$cell_dir/server_stdout.txt" >> "$stdout"
    cat "$cell_dir/server_stderr.txt" >> "$stderr"
    if [[ -n "$NSYS" && -f "$cell_dir/roce_server.nsys-rep" ]]; then
      echo "=== nsys GPU kernel summary ($cell) ===" >&2
      "$NSYS_BIN" stats --report cuda_gpu_kern_sum --format table \
          "$cell_dir/roce_server.nsys-rep" >&2 || true
      echo "=== nsys CUDA API summary ($cell) ===" >&2
      "$NSYS_BIN" stats --report cuda_api_sum --format table \
          "$cell_dir/roce_server.nsys-rep" >&2 || true
      echo "nsys report: $cell_dir/roce_server.nsys-rep" >&2
    fi
    local phy_tx_delta phy_rx_delta
    phy_tx_delta=$(( $(phy_counter "$RDMA_CLIENT_NETDEV" tx_packets_phy "$RDMA_CLIENT_NS") - phy_tx_before ))
    phy_rx_delta=$(( $(phy_counter "$RDMA_SERVER_NETDEV" rx_packets_phy "$RDMA_SERVER_NS") - phy_rx_before ))
    # RDMA prints "Client complete: ... send_completions=N send_bytes=N seconds=S".
    pkts="$(extract_field 'Client complete' send_completions "$stdout")"
    bytes="$(extract_field 'Client complete' send_bytes "$stdout")"
    secs="$(extract_field 'Client complete' seconds "$stdout")"
    rx_bytes="$bytes"
    # Wire-transit check. On the cable the server's rx_*_phy advances by at least one
    # SerDes packet per RDMA message -- many more once a message exceeds the MTU and
    # segments -- so a genuine over-the-wire run shows phy_rx_delta >= the message
    # count. An on-chip eswitch short-cut leaves rx_*_phy essentially FLAT (only a
    # handful of stray background/control packets), so a bare ">0" is not enough: a
    # +22 on a 23M-message run passed the old check while never touching the cable.
    # Require the delta to reach half the message count (generous margin for counter
    # slack) before certifying wire transit.
    local phy_min=$(( ${pkts:-0} / 2 ))
    if [[ -z "$RDMA_SERVER_NETDEV" ]]; then
      echo "ERROR: $cell could not resolve server netdev in $RDMA_SERVER_NS" >&2
      bench_rc=90
    elif [[ "$phy_rx_delta" -lt "$phy_min" || "$phy_rx_delta" -le 0 ]]; then
      echo "ERROR: $cell rx_packets_phy advanced only +$phy_rx_delta on $RDMA_SERVER_NETDEV (expected >= ~$phy_min, one per message)" >&2
      bench_rc=90
    else
      echo "INFO: $cell wire OK -- client tx_packets_phy +$phy_tx_delta, server rx_packets_phy +$phy_rx_delta (>= $phy_min msgs)" >&2
    fi
  else
    # Snapshot the p0/p1 *_phy counters around the run to assert the packets crossed
    # the cable (tx_port -> rx_port over the wire), not the on-chip eswitch short-cut.
    local phy_tx_before phy_rx_before
    phy_tx_before="$(phy_counter "$DPDK_TX_NETDEV" tx_packets_phy)"
    phy_rx_before="$(phy_counter "$DPDK_RX_NETDEV" rx_packets_phy)"
    "$BENCH_BIN" "$yaml" --seconds "$RUN_SECONDS" "${bench_extra[@]}" \
        > "$stdout" 2> "$stderr" || bench_rc=$?
    local phy_tx_delta phy_rx_delta
    phy_tx_delta=$(( $(phy_counter "$DPDK_TX_NETDEV" tx_packets_phy) - phy_tx_before ))
    phy_rx_delta=$(( $(phy_counter "$DPDK_RX_NETDEV" rx_packets_phy) - phy_rx_before ))
    local raw_priority_drop_after
    raw_priority_drop_after="$(priority_buffer_discards "$DPDK_RX_NETDEV")"
    if (( raw_priority_drop_after >= raw_priority_drop_before )); then
      raw_priority_drops=$((raw_priority_drop_after - raw_priority_drop_before))
    else
      raw_priority_drops="$raw_priority_drop_after"
    fi
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
    # Wire-transit check. Raw Ethernet is one SerDes packet per app packet, so a
    # genuine over-the-wire run advances rx_*_phy by ~the received packet count; an
    # on-chip eswitch short-cut leaves it near-flat. Require the delta to reach half
    # the packet count -- a bare ">0" would let a stray background packet false-pass
    # (the same hole the RoCE check had).
    local phy_min=$(( ${pkts:-0} / 2 ))
    if [[ -z "$DPDK_RX_NETDEV" ]]; then
      echo "ERROR: $cell could not resolve rx_port netdev ($DPDK_RX_PCI)" >&2
      bench_rc=90
    elif [[ "$phy_rx_delta" -lt "$phy_min" || "$phy_rx_delta" -le 0 ]]; then
      echo "ERROR: $cell rx_packets_phy advanced only +$phy_rx_delta on $DPDK_RX_NETDEV (expected >= ~$phy_min)" >&2
      bench_rc=90
    else
      echo "INFO: $cell wire OK -- p0 tx_packets_phy +$phy_tx_delta, p1 rx_packets_phy +$phy_rx_delta (>= $phy_min)" >&2
    fi
  fi
  cp "$stderr" "$DRIVER_LOG"

  # Snapshot per-cpu stats right after the bench exits (before background
  # captures finish reaping, to bound the window).
  snapshot_cpu_stat "$cell_dir/cpu_stat.after"

  # Stop the open-ended NIC capture and reap both monitors.
  kill "$mlnx_perf_pid" 2>/dev/null || true
  wait "$mlnx_perf_pid" 2>/dev/null || true
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
      drops=$(( $(parse_dpdk_drops "$stderr") + raw_priority_drops ))
      drops_kind="dpdk-imissed+ierrors+nombuf+priority-buffer"
      ;;
    ibverbs)
      drops=$(( $(parse_ibverbs_drops "$stderr") + raw_priority_drops ))
      drops_kind="ibverbs-cqe+ring+priority-buffer"
      ;;
    rdma)
      drops="$(parse_rdma_drops "$stderr")"
      drops_kind="rdma-cqe-error"
      ;;
    socket-udp)
      local udp_after; udp_after="$(snapshot_udp_nstat "$udp_ns")"
      local udp_nstat_drops=$((udp_after - udp_before))
      local udp_app_drops=$(( ${pkts:-0} - ${rx_pkts:-0} ))
      (( udp_app_drops < 0 )) && udp_app_drops=0
      drops="$udp_app_drops"
      drops_kind="udp-app-tx-minus-rx"
      if (( udp_nstat_drops > udp_app_drops )); then
        echo "WARN: $cell UdpInErrors delta $udp_nstat_drops exceeds app TX-RX gap $udp_app_drops" >&2
      fi
      ;;
    socket-tcp)
      local tcp_after; tcp_after="$(snapshot_tcp_nstat "$tcp_ns")"
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

  local pp_gemm_dim="$GEMM_DIM"
  local pp_sync="${SYNC_INTERVAL:-default}"
  [[ -z "$pp_sync" ]] && pp_sync="default"
  local row="$BENCH_PLATFORM,$lang,$BACKEND,$WORKLOAD_EFF,$payload,$batch,$observed_max_rx_burst,$pairs"
  row+=",$target_gbps,$rep,$secs,$pkts,$bytes,$pps,$gbps,$rx_gbps,$drops,$drops_kind"
  row+=",$CPU_MASTER,$CPU_TX,$CPU_RX,$cpu_master_pct,$cpu_tx_pct,$cpu_rx_pct"
  row+=",$gpu_sm,$gpu_mem,$pp_gemm_dim,$pp_sync"
  echo "$row" \
    | tee -a "$CSV"
}

# Run a cell REPEATS times (each an independent run + CSV row) for error bars.
run_cell_or_record_failure() {
  local rep
  for rep in $(seq 1 "$REPEATS"); do
    run_cell "$@" "$rep" || FAILURES=$((FAILURES + 1))
  done
}

payload_setting() {
  local settings="$1" payload="$2" fallback="$3" entry
  for entry in $settings; do
    if [[ "${entry%%:*}" == "$payload" ]]; then
      echo "${entry#*:}"
      return
    fi
  done
  echo "$fallback"
}

# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

preflight_hardware || exit 1

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
      cell_batches=("${BATCHES_SWEEP[@]}")
      cell_target=0
      if [[ "$BACKEND" == "dpdk" && -z "${BATCHES_OVERRIDE:-}" ]]; then
        cell_batches=("$(payload_setting "$DPDK_PAYLOAD_BATCHES" "$p" "${BATCHES_SWEEP[0]}")")
        cell_target="$(payload_setting "$DPDK_PAYLOAD_PACING_GBPS" "$p" 0)"
      elif [[ "$BACKEND" == "ibverbs" && -z "${BATCHES_OVERRIDE:-}" ]]; then
        cell_batches=("$(payload_setting "$IBVERBS_PAYLOAD_BATCHES" "$p" "$IBVERBS_BATCH_SIZE")")
        cell_target="$(payload_setting "$IBVERBS_PAYLOAD_PACING_GBPS" "$p" 0)"
      fi
      for b in "${cell_batches[@]}"; do
        for n in "${PAIRS_SWEEP[@]}"; do
          run_cell_or_record_failure cpp "$p" "$b" "$n" "$cell_target"
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
