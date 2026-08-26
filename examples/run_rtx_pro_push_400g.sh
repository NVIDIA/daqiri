#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# RTX PRO 6000 -- push toward line rate using the documented levers:
#   1) MQ 2t2r + larger TX/RX batches
#   2) Single-queue batch sweep and small-packet stress
#   3) Dual parallel wire loops, when the host has two cabled pairs
#
# Topology, GPU ordinals, and poll cores all come from
# scripts/discover_rtx_pro_topology.sh. Disable 802.3x pause and raise the MTU
# on both ports first; otherwise the levers are measured against a throttled
# link (see docs/tutorials/system_configuration.md steps 9 and 10).
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

# $BUILD_DIR/src first: that is where CMake writes libdaqiri.so, and with the
# image's installed copy ahead of it a rebuild has no effect on what runs.
export LD_LIBRARY_PATH="$BUILD_DIR/src:/opt/daqiri/lib:${LD_LIBRARY_PATH:-}"

if [[ -x "$DISCOVER" ]]; then
  # shellcheck disable=SC1090
  source "$DISCOVER"
fi

# Everything topology-shaped comes from discovery. Hardcoded fallbacks are the
# reason an earlier version of this script benchmarked a port pair that had no
# cable between them.
TX_BDF="${RTX_TX_BDF:-}"
RX_BDF="${RTX_RX_BDF:-}"
TX_IF="${RTX_TX_IFACE:-}"
RX_IF="${RTX_RX_IFACE:-}"
TX_GPU="${TX_GPU:-${RTX_TX_GPU:-0}}"
RX_GPU="${RX_GPU:-${RTX_RX_GPU:-1}}"
TX_NUMA="${RTX_TX_NUMA:-0}"
RX_NUMA="${RTX_RX_NUMA:-0}"
CPU_MASTER="${RTX_MASTER_CORE:-3}"
CPU_TX="${RTX_TX_Q0_POLL:-11}"
CPU_RX="${RTX_RX_Q0_POLL:-9}"
CPU_TX_WORK="${RTX_TX_Q0_WORK:-$((CPU_TX + 1))}"
CPU_RX_WORK="${RTX_RX_Q0_WORK:-$((CPU_RX + 1))}"
CPU_TX2="${RTX_TX_Q1_POLL:-$((CPU_TX + 2))}"
CPU_RX2="${RTX_RX_Q1_POLL:-$((CPU_RX + 2))}"
IS_SW=0

# shellcheck disable=SC1090
source "$SCRIPT_DIR/rtx_pro_yaml_rewrite.sh"

if [[ -z "$TX_BDF" || -z "$RX_BDF" ]]; then
  echo "ERROR: no cabled port pair found. Set RTX_TX_BDF and RTX_RX_BDF." >&2
  exit 1
fi
[[ -z "${ETH_DST_ADDR:-}" ]] && { echo "ERROR: ETH_DST_ADDR required" >&2; exit 1; }

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${DAQIRI_BENCH_RESULTS_DIR:-$REPO_ROOT/bench-results}/$TS-rtx-pro-push400g"
mkdir -p "$OUT_DIR"

CSV="$OUT_DIR/runs.csv"
echo "experiment,cell,tx_cores,rx_cores,payload,batch,rep,tx_gbps,rx_gbps,wire_tx_gbps,wire_rx_gbps,pps,drops,notes" > "$CSV"

# Same NIC-counter measurement and gate as the other two runners. This script
# drives several port pairs, so each cell sets WIRE_TX_IFACE/WIRE_RX_IFACE for
# the pair it is about to use.
WIRE_TX_IFACE="$TX_IF"
WIRE_RX_IFACE="$RX_IF"
NIC_VALIDATION_LOG="$OUT_DIR/wire_validation.txt"
# shellcheck disable=SC1090
source "$SCRIPT_DIR/rtx_pro_nic_counters.sh"

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

gen_nic_yaml() {
  local out="$1" txbdf="$2" rxbdf="$3" rxmac="$4" payload="${5:-8000}" batch="${6:-10240}"
  sed -E \
    -e "s|^( *payload_size: ).*|\1$payload|" \
    -e "s|^( *batch_size: ).*|\1$batch|g" \
    "$NIC_BASE" > "$out"
  # rewrite_raw_topology and rewrite_eth_dst read the topology from the caller's
  # scope, so a per-loop override is passed by shadowing those variables.
  local TX_BDF="$txbdf" RX_BDF="$rxbdf" ETH_DST_ADDR="$rxmac"
  rewrite_raw_topology "$out"
  rewrite_eth_dst "$out"
}

record_run() {
  local exp="$1" cell="$2" tx_cores="$3" rx_cores="$4" payload="$5" batch="$6"
  local stdout="$7" stderr="$8" notes="$9"
  local nic_before="${10:-}" nic_after="${11:-}"
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
  # An experiment that reports a throughput the NIC did not see is worse than a
  # missing row, because the point of these cells is to compare them.
  if ! nic_report "$nic_before" "$nic_after" "$rx_pkts" "$secs"; then
    echo "       ($exp $cell p$payload b$batch)" >&2
    FAILURES=$((FAILURES + 1))
    return 1
  fi
  tx_gbps="$(awk -v b="$tx_bytes" -v s="$secs" 'BEGIN { printf "%.3f", (b*8.0)/s/1e9 }')"
  rx_gbps="$(awk -v b="$rx_bytes" -v s="$secs" 'BEGIN { printf "%.3f", (b*8.0)/s/1e9 }')"
  pps="$(awk -v p="$rx_pkts" -v s="$secs" 'BEGIN { printf "%.0f", p/s }')"
  drops="$(parse_dpdk_drops "$stderr")"
  echo "$exp,$cell,$tx_cores,$rx_cores,$payload,$batch,1,$tx_gbps,$rx_gbps,$WIRE_TX_GBPS,$WIRE_RX_GBPS,$pps,$drops,$notes" | tee -a "$CSV"
}

run_mq_cell() {
  local exp="$1" tx_count="$2" rx_count="$3" payload="$4" batch="$5"
  local cell notes tx_cores rx_cores
  if [[ "$tx_count" == 2 && "$rx_count" == 2 ]]; then cell="2t2r"
  elif [[ "$tx_count" == 1 && "$rx_count" == 2 ]]; then cell="1t2r"
  elif [[ "$tx_count" == 2 && "$rx_count" == 1 ]]; then cell="2t1r"
  else cell="1t1r"; fi
  if [[ "$tx_count" == 2 ]]; then tx_cores="$CPU_TX|$CPU_TX2"; else tx_cores="$CPU_TX"; fi
  if [[ "$rx_count" == 2 ]]; then rx_cores="$CPU_RX|$CPU_RX2"; else rx_cores="$CPU_RX"; fi
  notes="batch=$batch mq"
  local run_dir="$OUT_DIR/mq-${cell}-p${payload}-b${batch}"
  mkdir -p "$run_dir"
  local cfg="$run_dir/config.yaml"
  local gen_args=(
    "$MQ_BASE" --tx "$tx_count" --rx "$rx_count"
    --payload "$payload" --batch "$batch" --eth-dst "$ETH_DST_ADDR"
    --tx-bdf "$TX_BDF" --rx-bdf "$RX_BDF"
    --tx-gpu "$TX_GPU" --rx-gpu "$RX_GPU"
  )
  [[ -n "${RTX_TX_GPU2:-}" ]] && gen_args+=(--tx-gpu2 "$RTX_TX_GPU2")
  [[ -n "${RTX_RX_GPU2:-}" ]] && gen_args+=(--rx-gpu2 "$RTX_RX_GPU2")
  # Without this the cells inherit the template's cores, so the core-scaling
  # experiment measures the template rather than this machine.
  [[ -n "${RTX_CPU_CORES:-}" ]] && gen_args+=(--cpu-cores "$RTX_CPU_CORES")
  python3 "$MQ_GEN" "${gen_args[@]}" > "$cfg" 2>"$run_dir/gen.err" || return 1
  # Patch queue batch_size in daqiri cfg (generator sets bench_tx only).
  awk -v b="$batch" '
    /^          batch_size:/ { sub(/[0-9]+$/, b); print; next }
    { print }
  ' "$cfg" > "${cfg}.tmp" && mv "${cfg}.tmp" "$cfg"
  local before after
  before="$(nic_snapshot)"
  "$BENCH_BIN" "$cfg" --seconds "$RUN_SECONDS" > "$run_dir/stdout.txt" 2>"$run_dir/stderr.txt" || true
  after="$(nic_snapshot)"
  record_run "$exp" "$cell" "$tx_cores" "$rx_cores" "$payload" "$batch" \
    "$run_dir/stdout.txt" "$run_dir/stderr.txt" "$notes" "$before" "$after"
}

# $8/$9 name the ports this loop uses, so the NIC is read on the pair actually
# under test rather than always on the primary one.
run_nic_loop() {
  local exp="$1" txbdf="$2" rxbdf="$3" rxmac="$4" label="$5"
  local payload="${6:-8000}" batch="${7:-10240}"
  local WIRE_TX_IFACE="${8:-$TX_IF}" WIRE_RX_IFACE="${9:-$RX_IF}"
  local run_dir="$OUT_DIR/${exp}-${label}"
  mkdir -p "$run_dir"
  gen_nic_yaml "$run_dir/config.yaml" "$txbdf" "$rxbdf" "$rxmac" "$payload" "$batch"
  local before after
  before="$(nic_snapshot)"
  "$BENCH_BIN" "$run_dir/config.yaml" --seconds "$RUN_SECONDS" \
    > "$run_dir/stdout.txt" 2>"$run_dir/stderr.txt" || true
  after="$(nic_snapshot)"
  record_run "$exp" "1t1r" "11" "9" "$payload" "$batch" \
    "$run_dir/stdout.txt" "$run_dir/stderr.txt" "$label" "$before" "$after"
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

# Lever 3: two cables driven at once, for an aggregate above one link's rate.
#
# This needs a *second* pair of ports cabled to each other. Ports that go to a
# switch cannot serve: frames sent to a peer that is not on the other end of the
# cable never come back. An earlier version named a fixed second pair that was
# in fact switch-facing, so it recorded two zero-throughput rows every run.
PAIR2_TX_IF=""
PAIR2_RX_IF=""
while read -r _a _b; do
  [[ -n "$_a" && -n "$_b" ]] || continue
  [[ "$_a" == "$TX_IF" || "$_a" == "$RX_IF" ]] && continue
  [[ "$_b" == "$TX_IF" || "$_b" == "$RX_IF" ]] && continue
  PAIR2_TX_IF="$_a"
  PAIR2_RX_IF="$_b"
  break
done <<< "${RTX_LOOPBACK_PAIRS:-}"

if [[ -n "$PAIR2_TX_IF" ]]; then
  PAIR2_TX_BDF="$(basename "$(readlink -f "/sys/class/net/${PAIR2_TX_IF}/device")")"
  PAIR2_RX_BDF="$(basename "$(readlink -f "/sys/class/net/${PAIR2_RX_IF}/device")")"
  PAIR2_RX_MAC="$(cat "/sys/class/net/${PAIR2_RX_IF}/address")"
  echo "Second loopback pair: ${PAIR2_TX_IF} -> ${PAIR2_RX_IF}" >&2

  run_nic_loop "dual-loop-probe" "$PAIR2_TX_BDF" "$PAIR2_RX_BDF" "$PAIR2_RX_MAC" \
    "${PAIR2_TX_IF}-to-${PAIR2_RX_IF}" 8000 10240 \
    "$PAIR2_TX_IF" "$PAIR2_RX_IF" || true

  PAIR2_DIR="$OUT_DIR/dual-parallel"
  mkdir -p "$PAIR2_DIR"
  gen_nic_yaml "$PAIR2_DIR/loop1.yaml" "$TX_BDF" "$RX_BDF" "$ETH_DST_ADDR" 8000 16384
  gen_nic_yaml "$PAIR2_DIR/loop2.yaml" "$PAIR2_TX_BDF" "$PAIR2_RX_BDF" "$PAIR2_RX_MAC" 8000 16384

  echo "Starting dual parallel run (loop1 + loop2)..." >&2
  # Both loops run inside one window, so each pair is read separately before and
  # after the pair of processes.
  l1_before="$(WIRE_TX_IFACE="$TX_IF" WIRE_RX_IFACE="$RX_IF" nic_snapshot)"
  l2_before="$(WIRE_TX_IFACE="$PAIR2_TX_IF" WIRE_RX_IFACE="$PAIR2_RX_IF" nic_snapshot)"
  "$BENCH_BIN" "$PAIR2_DIR/loop1.yaml" --seconds "$RUN_SECONDS" > "$PAIR2_DIR/l1.stdout" 2>"$PAIR2_DIR/l1.stderr" &
  pid1=$!
  "$BENCH_BIN" "$PAIR2_DIR/loop2.yaml" --seconds "$RUN_SECONDS" > "$PAIR2_DIR/l2.stdout" 2>"$PAIR2_DIR/l2.stderr" &
  pid2=$!
  wait "$pid1" || true
  wait "$pid2" || true
  l1_after="$(WIRE_TX_IFACE="$TX_IF" WIRE_RX_IFACE="$RX_IF" nic_snapshot)"
  l2_after="$(WIRE_TX_IFACE="$PAIR2_TX_IF" WIRE_RX_IFACE="$PAIR2_RX_IF" nic_snapshot)"

  l1_rx="$(sum_field 'RX complete' bytes "$PAIR2_DIR/l1.stdout")"
  l2_rx="$(sum_field 'RX complete' bytes "$PAIR2_DIR/l2.stdout")"
  l1_pkts="$(sum_field 'RX complete' packets "$PAIR2_DIR/l1.stdout")"
  l2_pkts="$(sum_field 'RX complete' packets "$PAIR2_DIR/l2.stdout")"
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

  WIRE_TX_IFACE="$TX_IF" WIRE_RX_IFACE="$RX_IF" \
    nic_report "$l1_before" "$l1_after" "${l1_pkts:-0}" "${l1_secs:-0}" || true
  l1_wire_tx="$WIRE_TX_GBPS"; l1_wire_rx="$WIRE_RX_GBPS"
  WIRE_TX_IFACE="$PAIR2_TX_IF" WIRE_RX_IFACE="$PAIR2_RX_IF" \
    nic_report "$l2_before" "$l2_after" "${l2_pkts:-0}" "${l2_secs:-0}" || true
  l2_wire_tx="$WIRE_TX_GBPS"; l2_wire_rx="$WIRE_RX_GBPS"
  agg_wire="$(awk -v a="$l1_wire_rx" -v b="$l2_wire_rx" 'BEGIN { printf "%.3f", a+b }')"

  echo "dual-parallel,loop1,|,|,8000,16384,1,$l1_gbps,$l1_gbps,$l1_wire_tx,$l1_wire_rx,0,0,pair=${TX_IF}->${RX_IF}" | tee -a "$CSV"
  echo "dual-parallel,loop2,|,|,8000,16384,1,$l2_gbps,$l2_gbps,$l2_wire_tx,$l2_wire_rx,0,0,pair=${PAIR2_TX_IF}->${PAIR2_RX_IF}" | tee -a "$CSV"
  echo "dual-parallel,sum,,,8000,16384,1,0,${agg_gbps},0,${agg_wire},0,0,aggregate of both cables" | tee -a "$CSV"
else
  echo "Skipping the dual-loop experiment: only one cabled port pair on this host." >&2
fi

echo
echo "==================== Results ===================="
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"
echo
best="$(awk -F, 'NR>1 && $9+0>max {max=$9+0; line=$0} END {print line}' "$CSV")"
echo "Best single RX Gbps: $best"
best_agg="$(awk -F, '$1=="dual-parallel" && $2=="sum" {print $9; exit}' "$CSV")"
echo "Dual-loop aggregate RX Gbps: ${best_agg:-not run (one cabled pair)}"

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
if [[ "$FAILURES" -gt 0 ]]; then
  echo "Failures: $FAILURES" >&2
  exit 1
fi
exit 0
