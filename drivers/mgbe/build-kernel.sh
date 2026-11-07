#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 VENDOR_SOURCE BUILD_DIRECTORY [baseline|patched]" >&2
  exit 2
fi
[[ -e /.dockerenv ]] || { echo 'Run this build inside the project container.' >&2; exit 1; }
source_root=$(realpath "$1")
mkdir -p "$2"
output=$(realpath "$2")
mode=${3:-patched}
[[ $mode == baseline || $mode == patched ]] || exit 2
prototype=$(cd -- "$(dirname -- "$0")" && pwd)
kernel_release=$(uname -r)
kernel_headers=/lib/modules/$kernel_release/build
oot_headers=/usr/src/linux-headers-tegra-oot-igx-2.0-$kernel_release/nvidia-oot
expected=85b5dd4a7b14c7311c651b4fd9e60515a813da0f
[[ $(git -C "$source_root" rev-parse HEAD) == "$expected" ]] || {
  echo "Expected Canonical tegra-oot v2.4.0-1_igx-2.0 ($expected)." >&2; exit 1;
}
git -C "$source_root" diff --quiet HEAD -- nvidia-oot/drivers/net/ethernet/nvidia/nvethernet nvethernetrm || {
  echo 'Vendor source has local changes; refusing to build an unverified base.' >&2; exit 1;
}
[[ $(modinfo -F srcversion nvethernet) == ACEB0B6366A461508EE22A8 ]] || {
  echo 'Installed stock driver differs from the validated source fingerprint.' >&2; exit 1;
}
[[ -f $oot_headers/Module.symvers ]] || { echo 'Missing matching OOT symbol versions.' >&2; exit 1; }
module=$output/nvethernet
if [[ -e $module ]]; then
  echo "Build directory already contains nvethernet; use a new directory: $output" >&2
  exit 1
fi
mkdir -p "$module" "$output/conftest/nvidia"
cp -a "$source_root/nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/." "$module/"
rm "$module/nvethernetrm"
cp -a "$source_root/nvethernetrm" "$module/nvethernetrm"
cp -a "$source_root/nvidia-oot/scripts/conftest/." "$output/conftest/nvidia/"
make -j "$(nproc)" -f "$output/conftest/nvidia/Makefile" \
  src="$output/conftest/nvidia" obj="$output/conftest/nvidia" \
  CC=gcc LD=ld ARCH=arm64 NV_KERNEL_SOURCES="$kernel_headers" NV_KERNEL_OUTPUT="$kernel_headers"
# Do not import the module's own exports as external symbols.
awk '$3 !~ /nvethernet\/nvethernet$/ {print}' "$oot_headers/Module.symvers" > "$output/Module.symvers.external"
if [[ $mode == patched ]]; then
  python3 "$prototype/prepare_vendor.py" "$source_root" "$module"
fi
make -C "$kernel_headers" -j "$(nproc)" M="$module" ARCH=arm64 \
  "srctree.nvidia-oot=$source_root/nvidia-oot" \
  "srctree.nvconftest=$output/conftest" \
  "KBUILD_EXTRA_SYMBOLS=$output/Module.symvers.external" \
  "EXTRA_CFLAGS=-I$output/conftest -I$source_root/nvidia-oot/include -I$module/nvethernetrm/include" modules
modinfo "$module/nvethernet.ko" | head -28
