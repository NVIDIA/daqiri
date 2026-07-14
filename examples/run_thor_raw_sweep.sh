#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Raw-Ethernet throughput sweep for the IGX Thor box (before/after system tuning).
# Runs daqiri_bench_raw_gpudirect over a physical CX7 two-port loopback across a
# 2 engines x 5 payloads matrix and emits one CSV row per cell.
#
#   engines : dpdk (default raw engine) | ibverbs (pure-DevX MPRQ engine, set via
#             `engine: "ibverbs"` injected into the config)
#   payloads: 8000 4096 1024 256 64  (bytes)
#
# Execution model (host-driven): this script runs on the HOST. It samples the RX
# netdev's *_phy SerDes counters via host `ethtool -S` (works as non-root, same
# counters mlnx_perf reports, kept live by the mlx5 bifurcated driver even while
# the engine owns the port) and launches each bench cell inside the project
# container (`daqiri:local`, privileged, --gpus all, hugepages mounted) as root.
# The full-run byte delta / elapsed gives Gb/s (matches run_spark_bench.sh's
# wire-transit methodology); a non-advancing rx_packets_phy flags an on-chip
# eswitch short-cut instead of a true cable crossing.
#
# Usage (from the host, docker + ethtool on PATH):
#   PHASE=before ./run_thor_raw_sweep.sh
#   PHASE=after  ./run_thor_raw_sweep.sh
#
# Environment (all have Thor defaults):
#   PHASE          before | after   (tags the CSV rows; default "before")
#   DOCKER_IMAGE   container image   (default daqiri:local)
#   CONTAINER_BENCH  bench path in image (default /opt/daqiri/bin/daqiri_bench_raw_gpudirect)
#   RUN_IN_DOCKER  1=launch cells via docker, 0=run CONTAINER_BENCH directly
#                  (default 1; set 0 when already inside the container with ethtool)
#   BASE_YAML      base config       (default examples/daqiri_bench_raw_tx_rx_thor.yaml)
#   RX_NETDEV      rx_port netdev     (default enP4p3s0f1np1)
#   TX_NETDEV      tx_port netdev     (default enP4p3s0f0np0)
#   ETH_DST_ADDR   rx_port MAC        (default ac:3a:e2:86:7a:0f)
#   ENGINES        space list         (default "dpdk ibverbs")
#   PAYLOADS       space list         (default "8000 4096 1024 256 64")
#   RUN_SECONDS    per-cell seconds   (default 30)
#   REPEATS        reps per cell      (default 1; use 3 for the published re-run)
#   LINE_RATE_GBPS link line rate     (default 200; used for the %-of-line column)
#   OUT_ROOT       results parent dir  (default /home/rguru/projects/daqiri-llm-notes)
#   HUGE_MNT       host hugetlbfs path (default /dev/hugepages; mounted into cells)
#   GPU_DEVICE     discrete GPU to pin (default 1 = nvidia-smi index of the RTX
#                  PRO 6000; the Tegra CUDA runtime enumerates one GPU class at a
#                  time so we expose the discrete card alone -> it is ordinal 0)
#   NVIDIA_DISABLE_REQUIRE  bypass the image's NVIDIA_REQUIRE_CUDA gate (default 1;
#                  precautionary -- the image is now CUDA 13.0 to match the host
#                  580/13.0 driver, but the pre-GA 580.00 build sits just below the
#                  13.0 GA driver baseline so the gate can still be picky. The 13.0
#                  runtime itself works against this driver; verified empirically.)
#
# Deliverables land under $OUT_ROOT/<ts>-thor-raw/ (outside the repo tree). In
# docker mode the cell dir (holding the generated YAML the bench reads) and
# $HUGE_MNT are bind-mounted into the container at identical paths, so absolute
# paths resolve on both sides. CUDA_DEVICE_ORDER=PCI_BUS_ID is set inside every
# cell so ordinal 1 = the discrete RTX PRO 6000.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_YAML="${BASE_YAML:-$SCRIPT_DIR/daqiri_bench_raw_tx_rx_thor.yaml}"

PHASE="${PHASE:-before}"
DOCKER_IMAGE="${DOCKER_IMAGE:-daqiri:local}"
CONTAINER_BENCH="${CONTAINER_BENCH:-/opt/daqiri/bin/daqiri_bench_raw_gpudirect}"
RUN_IN_DOCKER="${RUN_IN_DOCKER:-1}"
RX_NETDEV="${RX_NETDEV:-enP4p3s0f1np1}"
TX_NETDEV="${TX_NETDEV:-enP4p3s0f0np0}"
ETH_DST_ADDR="${ETH_DST_ADDR:-ac:3a:e2:86:7a:0f}"
RUN_SECONDS="${RUN_SECONDS:-30}"
REPEATS="${REPEATS:-1}"
LINE_RATE_GBPS="${LINE_RATE_GBPS:-200}"
OUT_ROOT="${OUT_ROOT:-/home/rguru/projects/daqiri-llm-notes}"
HUGE_MNT="${HUGE_MNT:-/dev/hugepages}"
# Select the DISCRETE RTX PRO 6000 for CUDA. CRITICAL: `--privileged` (needed for
# DPDK NIC/hugepage access) exposes ALL host /dev/nvidia* nodes and OVERRIDES the
# `--gpus` selection, so the Tegra mixed-enumeration falls back to the integrated
# Thor iGPU (sm_110, which reports DMA_BUF_SUPPORTED=0 -> forces the peermem path,
# which then fails). The reliable selector under --privileged is CUDA_VISIBLE_DEVICES
# by UUID: it makes the discrete sm_120 card CUDA device 0 (DMA_BUF=1), matching the
# config's affinity: 0. GPU_DEVICE still feeds --gpus (harmless belt-and-suspenders).
GPU_DEVICE="${GPU_DEVICE:-1}"
GPU_UUID="${GPU_UUID:-GPU-5f077945-4eac-c3f4-7939-ec8d5457c4b8}"
# NIC descriptor-ring depth (DPDK engine). Default 8192 needs num_bufs >= ~24576
# per queue-backed MR, which two 8 KB GPUDirect regions can't fit in the 256 MiB
# BAR1. Shrink the ring to 2048 so num_bufs 12288 is a healthy ~6x ring (no RX
# starvation / flow-control backpressure). 2048 x 8 KB = 16 MB in-flight, ample
# for a single queue. Consumed by DpdkEngine::initialize via env.
NUM_RX_DESC="${NUM_RX_DESC:-2048}"
NUM_TX_DESC="${NUM_TX_DESC:-2048}"
# Optional buffer-config overrides (sed into the temp config). Empty = keep the
# YAML's values. Use these for the Resizable-BAR "optimized" run, e.g.
# NUM_BUFS=51200 BATCH_SIZE=10240 NUM_RX_DESC=8192 NUM_TX_DESC=8192 (restores the
# published-config buffering once BAR1 is large enough to map the GPU regions).
NUM_BUFS="${NUM_BUFS:-}"
BATCH_SIZE="${BATCH_SIZE:-}"
# Optional per-burst GPU workload on the real received payload (raw_gpudirect bench):
# WORKLOAD = none|fft|gemm|gemm_fp16 (empty = don't pass the flag -> bench default none)
# WORKLOAD_BATCH = --workload-batch-bytes value (empty = bench default). Exercises the
# GPU compute path (cuFFT/cuBLAS) where the locked dGPU clocks actually matter.
WORKLOAD="${WORKLOAD:-}"
WORKLOAD_BATCH="${WORKLOAD_BATCH:-}"
read -r -a ENGINES <<< "${ENGINES:-dpdk ibverbs}"
read -r -a PAYLOADS <<< "${PAYLOADS:-8000 4096 1024 256 64}"

if [[ ! -f "$BASE_YAML" ]]; then
  echo "ERROR: base config not found: $BASE_YAML" >&2
  exit 1
fi
if ! command -v ethtool >/dev/null 2>&1; then
  echo "ERROR: ethtool not on PATH (host measurement needs it)." >&2
  exit 1
fi
if [[ ! -e "/sys/class/net/$RX_NETDEV" ]]; then
  echo "ERROR: rx netdev $RX_NETDEV not present; set RX_NETDEV." >&2
  exit 1
fi
if [[ "$RUN_IN_DOCKER" == "1" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker not on PATH; set RUN_IN_DOCKER=0 to run the bench directly." >&2
    exit 1
  fi
  if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
    echo "ERROR: docker image '$DOCKER_IMAGE' not found; build it first." >&2
    exit 1
  fi
elif [[ ! -x "$CONTAINER_BENCH" ]]; then
  echo "ERROR: bench binary not found/executable: $CONTAINER_BENCH" >&2
  exit 1
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$OUT_ROOT/$TS-thor-raw"
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/runs.csv"
if [[ ! -f "$CSV" ]]; then
  echo "phase,engine,payload,rep,seconds,rx_bytes_phy,rx_packets_phy,gbps,pps,pct_line_rate,drops,app_packets" > "$CSV"
fi
echo "Output dir: $OUT_DIR"
FAILURES=0

# Sum a NIC *_phy SerDes counter. Empty/missing netdev -> 0.
phy_counter() {
  local netdev="$1" key="$2"
  ethtool -S "$netdev" 2>/dev/null \
    | awk -F'[: ]+' -v k="$key" '$2 == k { s += $3 } END { printf "%d", s+0 }'
}

# Sum engine drop counters (DPDK: imissed/ierrors/rx_nombuf; ibverbs: CQ errors)
# from the bench stderr/DAQIRI_LOG output.
parse_drops() {
  local log="$1" sum=0 v key
  for key in imissed ierrors rx_nombuf; do
    v="$(grep -oE "$key=[0-9]+" "$log" 2>/dev/null | tail -n1 | sed -E 's/.*=//' || true)"
    [[ -n "${v:-}" ]] && sum=$((sum + v))
  done
  local cqe; cqe="$(grep -c 'CQ error' "$log" 2>/dev/null)" || true
  sum=$((sum + ${cqe:-0}))
  echo "$sum"
}

# Read the app-reported RX (fallback TX) packet count from the bench stdout.
app_packets() {
  local out="$1" p
  p="$(grep -E '^RX complete' "$out" | tail -n1 | grep -oE ' packets=[0-9]+' | sed -E 's/.*=//')"
  [[ -z "$p" ]] && p="$(grep -E '^TX complete' "$out" | tail -n1 | grep -oE ' packets=[0-9]+' | sed -E 's/.*=//')"
  echo "${p:-0}"
}

# Build the per-cell config: substitute payload_size + eth_dst_addr, and for the
# ibverbs engine inject `engine: "ibverbs"` right after the stream_type line.
generate_yaml() {
  local out="$1" engine="$2" payload="$3"
  sed -E \
    -e "s|^( *payload_size: ).*|\1$payload|" \
    -e "s|<00:00:00:00:00:00>|$ETH_DST_ADDR|g" \
    "$BASE_YAML" > "$out"
  [[ -n "$NUM_BUFS" ]] && sed -i -E "s|^( *num_bufs: ).*|\1$NUM_BUFS|" "$out"
  [[ -n "$BATCH_SIZE" ]] && sed -i -E "s|^( *batch_size: ).*|\1$BATCH_SIZE|" "$out"
  if [[ "$engine" == "ibverbs" ]]; then
    sed -i -E '/^( *)stream_type: "raw"/a\    engine: "ibverbs"' "$out"
  fi
}

# Launch one bench cell. In docker mode the cell dir (YAML + logs) and $HUGE_MNT
# are bind-mounted at identical paths so the absolute YAML path resolves inside.
launch_bench() {
  local yaml="$1" stdout="$2" stderr="$3" cell_dir="$4"
  # Optional GPU workload run per received burst on the real payload (issue #15):
  # --workload none|fft|gemm|gemm_fp16, --workload-batch-bytes N (compute working set).
  local wl=()
  [[ -n "$WORKLOAD" ]] && wl+=(--workload "$WORKLOAD")
  [[ -n "$WORKLOAD_BATCH" ]] && wl+=(--workload-batch-bytes "$WORKLOAD_BATCH")
  if [[ "$RUN_IN_DOCKER" == "1" ]]; then
    docker run --rm --privileged --runtime=nvidia --network=host \
      --gpus "\"device=$GPU_DEVICE\"" \
      -e CUDA_VISIBLE_DEVICES="$GPU_UUID" \
      -e DAQIRI_NUM_RX_DESC="$NUM_RX_DESC" -e DAQIRI_NUM_TX_DESC="$NUM_TX_DESC" \
      -e CUDA_DEVICE_ORDER=PCI_BUS_ID \
      -e NVIDIA_DISABLE_REQUIRE="${NVIDIA_DISABLE_REQUIRE:-1}" \
      -v "$HUGE_MNT:$HUGE_MNT" \
      -v "$cell_dir:$cell_dir" \
      "$DOCKER_IMAGE" \
      "$CONTAINER_BENCH" "$yaml" --seconds "$RUN_SECONDS" "${wl[@]}" \
      > "$stdout" 2> "$stderr"
  else
    CUDA_DEVICE_ORDER=PCI_BUS_ID "$CONTAINER_BENCH" "$yaml" --seconds "$RUN_SECONDS" "${wl[@]}" \
      > "$stdout" 2> "$stderr"
  fi
}

run_cell() {
  local engine="$1" payload="$2" rep="$3"
  local cell="$PHASE-$engine-p$payload-r$rep"
  local cell_dir="$OUT_DIR/$cell"
  mkdir -p "$cell_dir"
  local yaml="$cell_dir/config.yaml"
  local stdout="$cell_dir/stdout.txt" stderr="$cell_dir/stderr.txt"
  generate_yaml "$yaml" "$engine" "$payload"

  # Settle: let any residual in-flight traffic from the previous cell fully land
  # before snapshotting the baseline, so the rx_*_phy delta measures ONLY this
  # cell (prevents cross-cell contamination that inflates back-to-back cells,
  # esp. workload cells whose GPU-busy teardown delays traffic stop).
  sleep "${SETTLE_SECONDS:-3}"

  local b_before p_before b_after p_after
  b_before="$(phy_counter "$RX_NETDEV" rx_bytes_phy)"
  p_before="$(phy_counter "$RX_NETDEV" rx_packets_phy)"
  local t_before; t_before="$(date +%s.%N)"

  launch_bench "$yaml" "$stdout" "$stderr" "$cell_dir"
  local rc=$?

  local t_after; t_after="$(date +%s.%N)"
  b_after="$(phy_counter "$RX_NETDEV" rx_bytes_phy)"
  p_after="$(phy_counter "$RX_NETDEV" rx_packets_phy)"

  if [[ "$rc" -ne 0 ]]; then
    echo "ERROR: $cell bench exited $rc (see $stderr)" >&2
    FAILURES=$((FAILURES + 1)); return 1
  fi

  local d_bytes d_pkts elapsed gbps pps pct
  d_bytes=$((b_after - b_before))
  d_pkts=$((p_after - p_before))
  elapsed="$(awk -v a="$t_before" -v b="$t_after" 'BEGIN{printf "%.3f", b-a}')"
  gbps="$(awk -v b="$d_bytes" -v s="$elapsed" 'BEGIN{ if(s>0) printf "%.3f", (b*8.0)/s/1e9; else print 0 }')"
  pps="$(awk  -v p="$d_pkts"  -v s="$elapsed" 'BEGIN{ if(s>0) printf "%.0f", p/s; else print 0 }')"
  pct="$(awk  -v g="$gbps" -v l="$LINE_RATE_GBPS" 'BEGIN{ if(l>0) printf "%.1f", 100.0*g/l; else print 0 }')"

  if [[ "$d_pkts" -le 0 ]]; then
    echo "WARN: $cell rx_packets_phy did not advance -- traffic may not have crossed the wire" >&2
  fi

  local drops apkts
  drops="$(parse_drops "$stderr")"
  apkts="$(app_packets "$stdout")"

  echo "$PHASE,$engine,$payload,$rep,$elapsed,$d_bytes,$d_pkts,$gbps,$pps,$pct,$drops,$apkts" | tee -a "$CSV"
}

echo "phase=$PHASE  engines=[${ENGINES[*]}]  payloads=[${PAYLOADS[*]}]  ${RUN_SECONDS}s x ${REPEATS} rep(s)  docker=$RUN_IN_DOCKER"
for engine in "${ENGINES[@]}"; do
  for payload in "${PAYLOADS[@]}"; do
    for rep in $(seq 1 "$REPEATS"); do
      run_cell "$engine" "$payload" "$rep" || true
    done
  done
done

echo
echo "Results in: $OUT_DIR"
echo "CSV:        $CSV"
[[ "$FAILURES" -ne 0 ]] && { echo "Failed cells: $FAILURES" >&2; exit 1; }
exit 0
