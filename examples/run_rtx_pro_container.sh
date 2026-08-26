#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Launch an RTX PRO 6000 benchmark inside the project container.
#
# Usage:
#   ./examples/run_rtx_pro_container.sh <name> "<command>"
#
#   IMAGE=...    container image (default daqiri:resnet-rtxpro)
#   DETACH=1     run in the background
#   GPU_BDFS=... space-separated PCIe addresses to expose (default: all healthy)
#
# The normal path is `docker run --gpus all`, and that is what this script uses
# when it works. It falls back to passing the device nodes by hand when the
# NVIDIA container hook cannot enumerate the GPUs.
#
# That fallback matters on a host with a faulted board: nvidia-container-cli
# walks *every* GPU through NVML before honouring --gpus, so one unresponsive
# device makes every container fail to start, whichever GPU was requested.
# Bypassing the hook means bind-mounting the driver userspace this script would
# otherwise have injected, and passing only the healthy /dev/nvidiaN nodes.
#
# Device minors do not follow nvidia-smi order, so they are read from
# /proc/driver/nvidia/gpus/<bdf>/information rather than assumed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

NAME="${1:?usage: run_rtx_pro_container.sh <container-name> <command>}"
CMD="${2:?usage: run_rtx_pro_container.sh <container-name> <command>}"

IMAGE="${IMAGE:-daqiri:resnet-rtxpro}"
DETACH="${DETACH:-0}"

COMMON_ARGS=(
  --name "${NAME}"
  --rm
  --privileged
  --network host
  --ipc host
  -u 0
  # DPDK's EAL looks for hugepages under both of these.
  -v /dev/hugepages:/dev/hugepages
  -v /mnt/huge:/mnt/huge
  -v /dev/infiniband:/dev/infiniband
  -v /sys/bus/pci:/sys/bus/pci
  -v "${REPO_ROOT}:/work"
  -w /work
)

# Named network namespaces, for the socket/RoCE wire loopback. The namespaces are
# created on the host by scripts/setup_spark_wire_loopback_netns.sh (the image has
# no iproute2), and the bench enters them with `nsenter --net=/var/run/netns/<ns>`,
# which needs the handles visible in here. rshared so namespaces created after the
# container starts still show up.
if [[ -d /var/run/netns ]]; then
  COMMON_ARGS+=(-v /var/run/netns:/var/run/netns:rshared)
fi

# Resolve the topology on the host and pass the answer in.
#
# Discovery asks LLDP which two ports are cabled to each other, and the client
# for that lives on the host, not in the image. Left to itself the container
# falls back to guessing from carrier state and picks a switch-facing port, so
# the run transmits into the fabric and receives nothing.
if [[ -z "${RTX_TX_BDF:-}" && -r "${SCRIPT_DIR}/../scripts/discover_rtx_pro_topology.sh" ]]; then
  # shellcheck disable=SC1091
  source "${SCRIPT_DIR}/../scripts/discover_rtx_pro_topology.sh" >/dev/null 2>&1 || true
fi
# mlnx_perf ships in the image but shells out to ethtool, which does not. Mount
# the host binary and its one non-standard dependency so the independent
# wire-rate measurement AGENTS.md asks for is actually available in a run.
if [[ -x /usr/sbin/ethtool ]]; then
  COMMON_ARGS+=(-v /usr/sbin/ethtool:/usr/sbin/ethtool:ro)
  for mnl in /lib/x86_64-linux-gnu/libmnl.so.0*; do
    [[ -e "$mnl" ]] && COMMON_ARGS+=(-v "${mnl}:${mnl}:ro")
  done
fi

for var in RTX_TX_BDF RTX_RX_BDF RTX_TX_IFACE RTX_RX_IFACE RTX_TX_MAC RTX_RX_MAC \
           ETH_DST_ADDR RTX_TX_GPU RTX_RX_GPU RTX_TX_GPU2 RTX_RX_GPU2 \
           RTX_TX_NUMA RTX_RX_NUMA RTX_CPU_CORES RTX_MASTER_CORE \
           RTX_TX_Q0_POLL RTX_TX_Q0_WORK RTX_TX_Q1_POLL RTX_TX_Q1_WORK \
           RTX_RX_Q0_POLL RTX_RX_Q0_WORK RTX_RX_Q1_POLL RTX_RX_Q1_WORK; do
  [[ -n "${!var:-}" ]] && COMMON_ARGS+=(-e "${var}=${!var}")
done
[[ "${DETACH}" == "1" ]] && COMMON_ARGS+=(-d)

# A GPU is healthy if nvidia-smi can query it. An unresponsive board answers
# with an NVML error and must be left out of the container entirely.
healthy_gpu_bdfs() {
  local info bdf
  for info in /proc/driver/nvidia/gpus/*/information; do
    [[ -r "$info" ]] || continue
    bdf="$(basename "$(dirname "$info")")"
    nvidia-smi -i "$bdf" --query-gpu=uuid --format=csv,noheader >/dev/null 2>&1 || continue
    echo "$bdf"
  done
}

device_minor_for_bdf() {
  awk '/Device Minor:/ { print $NF }' "/proc/driver/nvidia/gpus/$1/information" 2>/dev/null
}

# --rm only cleans up a container that exits on its own. An interrupted run, or a
# second launch under the same name while the first is still up, leaves the name
# taken and every subsequent launch fails on a name conflict rather than on
# anything to do with the benchmark. Clear it first so relaunching is always safe.
docker rm -f "${NAME}" >/dev/null 2>&1 || true

if docker run --rm --gpus all "${IMAGE}" true >/dev/null 2>&1; then
  exec docker run "${COMMON_ARGS[@]}" --gpus all \
    --entrypoint bash "${IMAGE}" -lc "${CMD}"
fi

echo "NVIDIA container hook unavailable; passing GPU devices directly." >&2

mapfile -t GPU_LIST < <(if [[ -n "${GPU_BDFS:-}" ]]; then printf '%s\n' ${GPU_BDFS}; else healthy_gpu_bdfs; fi)
if [[ ${#GPU_LIST[@]} -eq 0 ]]; then
  echo "ERROR: no usable GPUs found." >&2
  exit 1
fi

DEV_ARGS=(--device /dev/nvidiactl)
for d in /dev/nvidia-uvm /dev/nvidia-uvm-tools /dev/nvidia-modeset; do
  [[ -e "$d" ]] && DEV_ARGS+=(--device "$d")
done

UUIDS=()
for bdf in "${GPU_LIST[@]}"; do
  minor="$(device_minor_for_bdf "$bdf")"
  [[ -n "$minor" && -e "/dev/nvidia${minor}" ]] || continue
  DEV_ARGS+=(--device "/dev/nvidia${minor}")
  # Select by UUID: a CUDA ordinal shifts when a board drops out, a UUID does not.
  UUIDS+=("$(nvidia-smi -i "$bdf" --query-gpu=uuid --format=csv,noheader)")
done
echo "GPUs: ${GPU_LIST[*]}" >&2

# Driver userspace the hook would have injected. Both the versioned file and its
# SONAME symlink are mounted: a bind mount resolves the symlink host-side, so the
# container sees a regular file under the name the loader actually searches for.
LIB_MOUNTS=()
LIB_DIR=/usr/lib/x86_64-linux-gnu
for soname in libcuda.so.1 libnvidia-ml.so.1 libnvidia-ptxjitcompiler.so.1 \
              libnvidia-nvvm.so.4 libnvidia-allocator.so.1; do
  [[ -e "${LIB_DIR}/${soname}" ]] || continue
  LIB_MOUNTS+=(-v "${LIB_DIR}/${soname}:${LIB_DIR}/${soname}:ro")
  target="$(basename "$(readlink -f "${LIB_DIR}/${soname}")")"
  [[ "$target" != "$soname" ]] && LIB_MOUNTS+=(-v "${LIB_DIR}/${target}:${LIB_DIR}/${target}:ro")
done
# Loaded by libcuda at runtime, with no SONAME symlink to find it by.
for extra in "${LIB_DIR}"/libnvidia-gpucomp.so.*; do
  [[ -e "$extra" ]] && LIB_MOUNTS+=(-v "${extra}:${extra}:ro")
done
for smi in /usr/bin/nvidia-smi /usr/sbin/nvidia-smi; do
  [[ -x "$smi" ]] && { LIB_MOUNTS+=(-v "${smi}:/usr/bin/nvidia-smi:ro"); break; }
done

CUDA_VIS="$(IFS=,; echo "${UUIDS[*]}")"

exec docker run "${COMMON_ARGS[@]}" \
  -e CUDA_VISIBLE_DEVICES="${CUDA_VIS}" \
  -e CUDA_DEVICE_ORDER="${CUDA_DEVICE_ORDER:-PCI_BUS_ID}" \
  -e NVIDIA_VISIBLE_DEVICES=void \
  "${LIB_MOUNTS[@]}" \
  "${DEV_ARGS[@]}" \
  --entrypoint bash "${IMAGE}" -lc "ldconfig 2>/dev/null; ${CMD}"
