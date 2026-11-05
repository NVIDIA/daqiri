#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Hardware-free SDK checks. GPU/NIC benchmarks are separate privileged runs.
set -euo pipefail
image="${1:?Usage: scripts/check-release-container.sh IMAGE}"
docker run --rm -i --network none \
  --user "$(id -u):$(id -g)" --entrypoint bash "${image}" <<'CHECK'
set -euo pipefail
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Use a private CUDA driver stub for hardware-free linking/import checks.
# Never configure it as a runtime library.
mkdir "$work/stubs"
ln -s /usr/local/cuda/lib64/stubs/libcuda.so "$work/stubs/libcuda.so.1"
export LD_LIBRARY_PATH="$work/stubs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
python3 - <<'PY'
import daqiri
import torch
import yaml
assert torch.ones(2).sum().item() == 2
print("PyTorch:", torch.__version__)
assert daqiri.__abi_version__ == 2
assert daqiri.__version__
assert hasattr(daqiri, 'send_tx_burst')
print('DAQIRI Python:', daqiri.__version__)
PY
for tool in ibdev2netdev mlnx_perf ibv_devinfo ibstat mlxconfig ethtool lspci ip; do
  command -v "$tool" || { echo "Missing tool: $tool" >&2; exit 1; }
done
python3 /opt/daqiri/bin/tune_system.py --help >/dev/null
gen_daqiri_config.py --help >/dev/null
pkg-config --exists daqiri
pkg-config --libs --static daqiri > "$work/pkg-libs"
if grep -Eq 'rte_|dpdk' "$work/pkg-libs"; then
  echo 'DAQIRI unexpectedly requires DPDK' >&2; exit 1
fi
test ! -e /opt/daqiri/lib/libdaqiri_dpdk.so
if grep -Ei '^(libdpdk|librte|python3-scapy|python3-pytest|clang-format|rivermax|aws-sdk)' \
     /opt/daqiri-release/added-packages.tsv; then
  echo 'Unexpected optional dependency added to the release' >&2; exit 1
fi
cat > "$work/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(release_consumer LANGUAGES CXX CUDA)
find_package(daqiri REQUIRED)
add_executable(consumer main.cu)
target_link_libraries(consumer PRIVATE daqiri::daqiri)
target_compile_features(consumer PRIVATE cxx_std_17)
CMAKE
cat > "$work/main.cu" <<'CPP'
#include <daqiri/daqiri.h>
#include <iostream>
int main() {
  char mac[6] = {};
  daqiri::format_eth_addr(mac, "02:00:00:00:00:01");
  std::cout << daqiri::version_string() << '\n';
  return static_cast<unsigned char>(mac[0]) == 2 && mac[5] == 1 ? 0 : 1;
}
CPP
cmake -S "$work" -B "$work/build" -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build "$work/build" -j 2
"$work/build/consumer"
# Intentional word splitting of pkg-config's compiler/linker arguments.
g++ -std=c++17 -x c++ "$work/main.cu" -x none -o "$work/consumer-pc" \
  $(pkg-config --cflags --libs daqiri)
"$work/consumer-pc"
for exe in /opt/daqiri/bin/daqiri_*; do
  if readelf -h "$exe" >/dev/null 2>&1; then
    if ldd "$exe" | grep -q 'not found'; then ldd "$exe"; exit 1; fi
  fi
done
cd /opt/daqiri-release
sha256sum --check SHA256SUMS >/dev/null
test -s daqiri-source.tar.gz
test -s added-packages.tsv
test -s components.tsv
test -s /opt/daqiri/THIRD-PARTY-NOTICES
test -s /opt/daqiri-release/THIRD-PARTY-NOTICES
cmp /opt/daqiri/THIRD-PARTY-NOTICES /opt/daqiri-release/THIRD-PARTY-NOTICES
grep -Fq 'Component: yaml-cpp' /opt/daqiri/THIRD-PARTY-NOTICES
grep -Fq 'Component: spdlog' /opt/daqiri/THIRD-PARTY-NOTICES
grep -Fq 'Component: fmt' /opt/daqiri/THIRD-PARTY-NOTICES
grep -Fq 'ADDED RELEASE-CONTAINER PACKAGES' /opt/daqiri/THIRD-PARTY-NOTICES
grep -Eq '  THIRD-PARTY-NOTICES$' /opt/daqiri-release/SHA256SUMS
echo 'Release SDK checks passed'
if [[ -f source-review.json ]]; then
  echo 'Publication source review remains pending; see /opt/daqiri-release/source-review.json'
fi
CHECK
