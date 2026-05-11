#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Capture host/NIC/GPU/build state for a benchmark run, so numbers are
# reproducible across machines and over time. Writes one structured text file
# with named sections.
#
# Usage: ./bench_capture_environment.sh <output_dir>
#        Default output dir: bench-results/<UTC timestamp>/

set -u

OUT_DIR="${1:-bench-results/$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/environment.txt"

# Run a command, capturing exit status. Always write a header so the section is
# present even when the command is missing or fails — silent absence is harder
# to debug than an explicit "command not found".
run_section() {
  local label="$1"; shift
  {
    echo "=========================================================="
    echo "[$label]"
    echo "  cmd: $*"
    echo "=========================================================="
    if command -v "$1" >/dev/null 2>&1 || [[ "$1" == /* || "$1" == ./* ]]; then
      "$@" 2>&1
      echo "  (exit: $?)"
    else
      echo "  (command not found in PATH: $1)"
    fi
    echo
  } >> "$OUT"
}

# Cat a file/glob; write a header either way.
cat_section() {
  local label="$1"; shift
  {
    echo "=========================================================="
    echo "[$label]"
    echo "  paths: $*"
    echo "=========================================================="
    for p in "$@"; do
      if compgen -G "$p" >/dev/null; then
        for f in $p; do
          echo "----- $f -----"
          cat "$f" 2>&1
        done
      else
        echo "  (no match: $p)"
      fi
    done
    echo
  } >> "$OUT"
}

: > "$OUT"

echo "DAQIRI benchmark environment capture" >> "$OUT"
echo "Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$OUT"
echo "Host:      $(hostname)" >> "$OUT"
echo "Output:    $OUT" >> "$OUT"
echo >> "$OUT"

# --- Kernel / OS ---
run_section "uname"           uname -a
cat_section "kernel-cmdline"  /proc/cmdline
cat_section "os-release"      /etc/os-release
run_section "lsb-release"     lsb_release -a
run_section "clocksource"     cat /sys/devices/system/clocksource/clocksource0/current_clocksource

# --- CPU / NUMA / IRQ ---
run_section "numactl"         numactl --show
run_section "lscpu"           lscpu
cat_section "cpu-isolated"    /sys/devices/system/cpu/isolated
run_section "cpufreq-info"    cpupower frequency-info
cat_section "cpu-governor"    /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
run_section "irq-mlx5"        bash -c "grep mlx5 /proc/interrupts || true"

# --- Hugepages ---
cat_section "hugepages"       /sys/kernel/mm/hugepages/*/nr_hugepages
run_section "free-h"          free -h

# --- PCIe topology ---
run_section "lspci-mellanox"  bash -c "lspci -vvv -d 15b3: 2>/dev/null"
run_section "lspci-nvidia"    bash -c "lspci -vvv -d 10de: 2>/dev/null"

# --- NIC: OFED / firmware / DPDK binding ---
run_section "ofed-info"       ofed_info -s
run_section "mlxfwmanager"    mlxfwmanager --query
run_section "dpdk-devbind"    dpdk-devbind.py --status
# Per-iface ethtool — iterate over the daqiri-tx/rx names if present, else all mlx5.
for iface in daqiri-tx daqiri-rx $(ls /sys/class/net 2>/dev/null | grep -E '^(enP|enp|eth)' || true); do
  [[ -d "/sys/class/net/$iface" ]] || continue
  run_section "ethtool-i:$iface"  ethtool -i "$iface"
  run_section "ethtool-g:$iface"  ethtool -g "$iface"
  run_section "ethtool-l:$iface"  ethtool -l "$iface"
  cat_section "iface-mtu:$iface"  "/sys/class/net/$iface/mtu"
  cat_section "iface-mac:$iface"  "/sys/class/net/$iface/address"
done

# --- GPU ---
run_section "nvidia-smi-q"        nvidia-smi -q
run_section "nvidia-smi-tempclk"  nvidia-smi --query-gpu=name,driver_version,temperature.gpu,clocks.current.sm,clocks.current.memory --format=csv

# --- Build state ---
DAQIRI_DIR="$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel 2>/dev/null || pwd)"
run_section "git-rev-parse"   git -C "$DAQIRI_DIR" rev-parse HEAD
run_section "git-status"      git -C "$DAQIRI_DIR" status --short
run_section "git-describe"    git -C "$DAQIRI_DIR" describe --always --dirty

echo "Capture complete: $OUT"
