#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Measure a DPDK raw-Ethernet receiver using an ibverbs RAW_PACKET transmitter
# paced by the NIC QP rate table.  The engines deliberately run in separate
# processes: the transmitter's implementation must not affect the RX result.
#
# The DPDK software --target-gbps pacer gates whole application batches.  At a
# 10,240-packet batch that produces a microburst even at a low average rate, so
# it cannot establish a loss-free small-packet receive knee.  ibverbs instead
# programs the HCA RAW_PACKET QP rate limit (pacing_mbps), which meters egress
# independently of the application burst cadence.
#
# Run in the privileged benchmark container, with the cabled ports in the
# default namespace (tear down dq_wire_* first):
#
#   ETH_DST_ADDR=<rx-mac> TX_PCI=<tx-pci> RX_PCI=<rx-pci> \
#     PAYLOADS=256 PACINGS=40000 \
#     ./examples/run_spark_raw_paced_tx.sh
#
# Required environment:
#   ETH_DST_ADDR — destination MAC of the DPDK receive port.
#   PAYLOADS     — one or more payload bytes.
#   PACINGS      — one or more ibverbs L2 rate caps in Mbps.
#   TX_PCI, RX_PCI — PCI addresses of the raw TX and DPDK RX ports.
# Optional: DAQIRI_BUILD_DIR, RUN_SECONDS (30), REPEATS (3), TX_BATCH (512),
#           TX_NUM_BUFS (4096), RX_BATCH (10240), TX_NETDEV, RX_NETDEV.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$REPO_DIR/build}"
BENCH_BIN="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect"
TX_BASE="$SCRIPT_DIR/daqiri_bench_raw_tx_spark_xhost.yaml"
RX_BASE="$SCRIPT_DIR/daqiri_bench_raw_rx_spark_xhost.yaml"
RUN_SECONDS="${RUN_SECONDS:-30}"
REPEATS="${REPEATS:-3}"
TX_PCI="${TX_PCI:?TX_PCI must identify the ibverbs transmit port}"
RX_PCI="${RX_PCI:?RX_PCI must identify the DPDK receive port}"
TX_BATCH="${TX_BATCH:-512}"
RX_BATCH="${RX_BATCH:-10240}"
TX_NUM_BUFS="${TX_NUM_BUFS:-4096}"

: "${ETH_DST_ADDR:?ETH_DST_ADDR must be the DPDK receive-port MAC}"
: "${PAYLOADS:?PAYLOADS must contain one or more payload sizes}"
: "${PACINGS:?PACINGS must contain one or more ibverbs pacing rates in Mbps}"

for value in "$RUN_SECONDS" "$REPEATS" "$TX_BATCH" "$RX_BATCH" "$TX_NUM_BUFS"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "Expected positive integer, got '$value'" >&2; exit 1; }
done
for value in $PAYLOADS $PACINGS; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid PAYLOADS/PACINGS entry '$value'" >&2; exit 1; }
done
[[ -x "$BENCH_BIN" ]] || { echo "Benchmark binary not found: $BENCH_BIN" >&2; exit 1; }
[[ -f "$TX_BASE" && -f "$RX_BASE" ]] || { echo "Raw TX/RX base YAML missing" >&2; exit 1; }
if ip netns list 2>/dev/null | grep -q '^dq_wire_'; then
  echo "dq_wire_* namespaces are up; tear them down before direct DPDK/ibverbs raw testing." >&2
  exit 1
fi

export LD_LIBRARY_PATH="$BUILD_DIR/src:$BUILD_DIR:/opt/daqiri/lib:${LD_LIBRARY_PATH:-}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$REPO_DIR/bench-results/$TS-raw-ibverbs-paced-tx"
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/runs.csv"
# `counter_gbps` is derived from the NIC byte counter (which includes FCS but
# not preamble/SFD or inter-frame gap).  `line_gbps` adds those 20 bytes per
# frame, making it directly comparable to the ibverbs QP rate cap.
echo "payload,pacing_mbps,rep,seconds,tx_packets,tx_bytes,rx_packets,rx_bytes,app_gbps,counter_gbps,line_gbps,tx_phy_packets,rx_phy_packets,drops" > "$CSV"

tx_netdev="${TX_NETDEV:-$(ls "/sys/bus/pci/devices/$TX_PCI/net" 2>/dev/null | head -n1 || true)}"
rx_netdev="${RX_NETDEV:-$(ls "/sys/bus/pci/devices/$RX_PCI/net" 2>/dev/null | head -n1 || true)}"

phy_counter() {
  local netdev="$1" key="$2"
  [[ -n "$netdev" ]] || { echo 0; return; }
  ethtool -S "$netdev" 2>/dev/null | awk -F'[: ]+' -v k="$key" '$2 == k { s += $3 } END { printf "%d", s+0 }'
}

field() {
  local prefix="$1" name="$2" file="$3"
  grep -E "^$prefix" "$file" | tail -n1 | grep -oE " $name=[^ ]+" | head -n1 | sed -E "s/.*$name=//"
}

dpdk_drops() {
  local log="$1" sum=0 value discards
  for key in imissed ierrors rx_nombuf; do
    value="$(grep -oE "$key=[0-9]+" "$log" 2>/dev/null | tail -n1 | sed -E 's/.*=//' || true)"
    sum=$((sum + ${value:-0}))
  done
  discards="$(grep -E 'rx_prio[0-9]+_buf_discard_packets:' "$log" 2>/dev/null \
    | sed -E 's/.*:[[:space:]]*([0-9]+)[[:space:]]*$/\1/' \
    | awk '{ s += $1 } END { printf "%d", s+0 }')"
  echo $((sum + ${discards:-0}))
}

make_tx_yaml() {
  local out="$1" payload="$2" pacing="$3" frame=$((payload + 64))
  sed -E \
    -e '/^[[:space:]]*stream_type: "raw"$/a\    engine: "ibverbs"' \
    -e "s|^([[:space:]]*)address: .*|\\1address: $TX_PCI|" \
    -e "s|^([[:space:]]*)num_bufs: .*|\\1num_bufs: $TX_NUM_BUFS|" \
    -e "s|^([[:space:]]*)buf_size: .*|\\1buf_size: $frame|" \
    -e "s|^([[:space:]]*)batch_size: .*|\\1batch_size: $TX_BATCH|" \
    -e "s|^([[:space:]]*)payload_size: .*|\\1payload_size: $payload|" \
    -e "s|<00:00:00:00:00:00>|$ETH_DST_ADDR|g" \
    -e '/^[[:space:]]*tx:/,/^[[:space:]]*bench_tx:/ {
          /^[[:space:]]*cpu_core: /a\          pacing_mbps: '"$pacing"'
        }' \
    "$TX_BASE" > "$out"
}

make_rx_yaml() {
  local out="$1"
  sed -E \
    -e '/^[[:space:]]*stream_type: "raw"$/a\    engine: "dpdk"' \
    -e "s|^([[:space:]]*)address: .*|\\1address: $RX_PCI|" \
    -e "s|^([[:space:]]*)batch_size: .*|\\1batch_size: $RX_BATCH|" \
    "$RX_BASE" > "$out"
}

for payload in $PAYLOADS; do
  for pacing in $PACINGS; do
    for rep in $(seq 1 "$REPEATS"); do
      cell="$OUT_DIR/p${payload}-m${pacing}-r${rep}"
      mkdir -p "$cell"
      make_tx_yaml "$cell/tx.yaml" "$payload" "$pacing"
      make_rx_yaml "$cell/rx.yaml"
      tx_before="$(phy_counter "$tx_netdev" tx_packets_phy)"
      rx_before="$(phy_counter "$rx_netdev" rx_packets_phy)"
      rx_bytes_before="$(phy_counter "$rx_netdev" rx_bytes_phy)"
      "$BENCH_BIN" "$cell/rx.yaml" --seconds "$((RUN_SECONDS + 8))" > "$cell/rx.out" 2> "$cell/rx.err" &
      rx_pid=$!
      sleep 3
      "$BENCH_BIN" "$cell/tx.yaml" --seconds "$RUN_SECONDS" > "$cell/tx.out" 2> "$cell/tx.err" || true
      wait "$rx_pid" || true
      tx_packets="$(field 'TX complete' packets "$cell/tx.out")"
      tx_bytes="$(field 'TX complete' bytes "$cell/tx.out")"
      tx_seconds="$(field 'TX complete' seconds "$cell/tx.out")"
      rx_packets="$(field 'RX complete' packets "$cell/rx.out")"
      rx_bytes="$(field 'RX complete' bytes "$cell/rx.out")"
      rx_seconds="$(field 'RX complete' seconds "$cell/rx.out")"
      tx_phy=$(( $(phy_counter "$tx_netdev" tx_packets_phy) - tx_before ))
      rx_phy=$(( $(phy_counter "$rx_netdev" rx_packets_phy) - rx_before ))
      rx_phy_bytes=$(( $(phy_counter "$rx_netdev" rx_bytes_phy) - rx_bytes_before ))
      drops="$(dpdk_drops "$cell/rx.err")"
      # RX deliberately outlives TX by eight seconds to drain the cable.  Use
      # the transmitter's active interval for app and wire rates; dividing by
      # the receiver lifetime would understate every result by ~21%.
      if [[ -z "$tx_seconds" || "$tx_seconds" == 0 || -z "$rx_bytes" ]]; then
        echo "ERROR: no parseable TX/RX completion stats for $cell" >&2
        continue
      fi
      app_gbps="$(awk -v b="$rx_bytes" -v s="$tx_seconds" 'BEGIN { printf "%.3f", b*8/s/1e9 }')"
      counter_gbps="$(awk -v b="$rx_phy_bytes" -v s="$tx_seconds" 'BEGIN { printf "%.3f", b*8/s/1e9 }')"
      # The DAQIRI buffer contains a 64-byte UDP/IP/Ethernet header plus the
      # configured payload. mlx5 packet counters account for the FCS but not
      # the 8-byte preamble/SFD and 12-byte inter-frame gap. The QP rate table
      # meters the latter as well, so include all of them here.
      line_bytes=$((payload + 64 + 4 + 8 + 12))
      line_gbps="$(awk -v p="$tx_phy" -v b="$line_bytes" -v s="$tx_seconds" 'BEGIN { printf "%.3f", p*b*8/s/1e9 }')"
      echo "$payload,$pacing,$rep,$tx_seconds,${tx_packets:-},${tx_bytes:-},${rx_packets:-},$rx_bytes,$app_gbps,$counter_gbps,$line_gbps,$tx_phy,$rx_phy,$drops" | tee -a "$CSV"
    done
  done
done

echo "Results: $CSV"
