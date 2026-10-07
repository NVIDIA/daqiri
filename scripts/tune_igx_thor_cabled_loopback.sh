#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HARDWARE_RESOLVER="$SCRIPT_DIR/resolve_benchmark_hardware.py"
HARDWARE_DEFAULTS="$REPO_ROOT/examples/cabled_loopback_hardware.yaml"
GPU_SERVICE=/etc/systemd/system/daqiri-gpu-max-clocks.service

if [[ "$(id -u)" -ne 0 ]]; then
  echo "ERROR: run this script as root: sudo $0" >&2
  exit 1
fi

for command in nmcli ethtool nvidia-smi python3 systemctl; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "ERROR: required command not found: $command" >&2
    exit 1
  }
done

resolved_hardware="$(python3 "$HARDWARE_RESOLVER" \
  --defaults "$HARDWARE_DEFAULTS" --platform igx-thor --format shell)"
eval "$resolved_hardware"

GPU_UUID="$DEFAULT_GPU_UUID"
EXPECTED_ISOLATED_CPUS="$DEFAULT_ISOLATED_CPUS"
NETWORK_INTERFACES=("$DEFAULT_DPDK_TX_NETDEV" "$DEFAULT_DPDK_RX_NETDEV")
NVIDIA_SMI="$(command -v nvidia-smi)"
GPU_SM_CLOCK="$("$NVIDIA_SMI" -i "$GPU_UUID" \
  --query-gpu=clocks.max.sm --format=csv,noheader,nounits | xargs)"
GPU_MEMORY_CLOCK="$("$NVIDIA_SMI" -i "$GPU_UUID" \
  --query-gpu=clocks.max.memory --format=csv,noheader,nounits | xargs)"

if [[ "$(cat /sys/devices/system/cpu/isolated)" != "$EXPECTED_ISOLATED_CPUS" ]]; then
  echo "ERROR: isolated CPUs are $(cat /sys/devices/system/cpu/isolated), expected $EXPECTED_ISOLATED_CPUS" >&2
  echo "This script does not modify the kernel command line." >&2
  exit 1
fi

for governor in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
  if [[ "$(cat "$governor")" != "performance" ]]; then
    echo "ERROR: $governor is not set to performance" >&2
    exit 1
  fi
done

NETWORK_CONNECTIONS=()
for interface in "${NETWORK_INTERFACES[@]}"; do
  connection=""
  while IFS= read -r candidate; do
    if [[ "$(nmcli -g connection.interface-name connection show "$candidate")" == "$interface" ]]; then
      connection="$candidate"
      break
    fi
  done < <(nmcli -g NAME connection show)
  if [[ -z "$connection" ]]; then
    echo "ERROR: no NetworkManager connection targets $interface" >&2
    exit 1
  fi
  NETWORK_CONNECTIONS+=("$connection")
done

for connection in "${NETWORK_CONNECTIONS[@]}"; do
  nmcli connection modify "$connection" \
    ipv4.method disabled \
    ipv6.method disabled \
    ethernet.mtu 9000 \
    ethtool.pause-autoneg off \
    ethtool.pause-rx off \
    ethtool.pause-tx off
  nmcli connection up "$connection"
done

"$NVIDIA_SMI" -i "$GPU_UUID" --persistence-mode=1
"$NVIDIA_SMI" -i "$GPU_UUID" --lock-gpu-clocks="$GPU_SM_CLOCK,$GPU_SM_CLOCK" --mode=1
"$NVIDIA_SMI" -i "$GPU_UUID" --lock-memory-clocks="$GPU_MEMORY_CLOCK,$GPU_MEMORY_CLOCK"

cat > "$GPU_SERVICE" <<EOF
[Unit]
Description=Lock DAQIRI discrete GPU clocks
After=multi-user.target

[Service]
Type=oneshot
ExecStart=$NVIDIA_SMI -i $GPU_UUID --persistence-mode=1
ExecStart=$NVIDIA_SMI -i $GPU_UUID --lock-gpu-clocks=$GPU_SM_CLOCK,$GPU_SM_CLOCK --mode=1
ExecStart=$NVIDIA_SMI -i $GPU_UUID --lock-memory-clocks=$GPU_MEMORY_CLOCK,$GPU_MEMORY_CLOCK
RemainAfterExit=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable daqiri-gpu-max-clocks.service
systemctl restart daqiri-gpu-max-clocks.service
systemctl is-active --quiet daqiri-gpu-max-clocks.service

for interface in "${NETWORK_INTERFACES[@]}"; do
  mtu="$(cat "/sys/class/net/$interface/mtu")"
  speed="$(ethtool "$interface" | awk '/Speed:/ {gsub(/Mb\/s/, "", $2); print $2}')"
  pause="$(ethtool --show-pause "$interface")"
  [[ "$mtu" == "9000" ]] || { echo "ERROR: $interface MTU is $mtu" >&2; exit 1; }
  [[ "$speed" == "$EXPECTED_LINK_MBPS" ]] || {
    echo "ERROR: $interface speed is ${speed:-unknown} Mb/s" >&2
    exit 1
  }
  if grep -Eq '^(RX|TX):[[:space:]]+on$' <<< "$pause"; then
    echo "ERROR: $interface still has pause enabled" >&2
    exit 1
  fi
done

page_count="$(awk '/HugePages_Total:/ {print $2}' /proc/meminfo)"
page_size_kib="$(awk '/Hugepagesize:/ {print $2}' /proc/meminfo)"
if (( page_count * page_size_kib < 4 * 1024 * 1024 )); then
  echo "ERROR: less than 4 GiB of hugepages is configured" >&2
  exit 1
fi

if [[ "$("$NVIDIA_SMI" -i "$GPU_UUID" --query-gpu=persistence_mode --format=csv,noheader,nounits)" != "Enabled" ]]; then
  echo "ERROR: GPU persistence mode is not enabled" >&2
  exit 1
fi

echo "IGX Thor cabled-loopback tuning is applied and verified."
echo "No restart is required."
