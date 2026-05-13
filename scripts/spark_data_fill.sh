#!/usr/bin/env bash
# Drives the PR 1 data-fill bench runs for the DGX Spark performance report.
#
# Runs DPDK GPUDirect, socket-UDP, and socket-TCP through their sweep and
# drop-curve modes via examples/run_spark_bench.sh, with pre-flight checks
# and orphan-hugepage cleanup. RDMA is deferred from PR 1 (single-host
# loopback over the cable needs a netns+two-process refactor; tracked
# separately).
#
# Run inside the project container (privileged, --gpus all, /dev/hugepages
# and /mnt/huge mounted, repo at /workspace).
#
# Usage:
#   ./scripts/spark_data_fill.sh                 # all three backends
#   ./scripts/spark_data_fill.sh dpdk            # just DPDK
#   ./scripts/spark_data_fill.sh socket-udp socket-tcp
#
# Env overrides:
#   ETH_DST_ADDR  — RX-side MAC. Auto-detected from
#                   /sys/class/net/enP2p1s0f0np0/address if unset.
#   RX_IFACE      — RX netdev name (default enP2p1s0f0np0).
#   DAQIRI_BUILD_DIR — defaults to ./build.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WRAPPER="$REPO_ROOT/examples/run_spark_bench.sh"
BUILD_DIR="${DAQIRI_BUILD_DIR:-$REPO_ROOT/build}"
RX_IFACE="${RX_IFACE:-enP2p1s0f0np0}"

BACKENDS=("$@")
[[ ${#BACKENDS[@]} -eq 0 ]] && BACKENDS=(dpdk socket-udp socket-tcp)

# --- pre-flight ------------------------------------------------------------

preflight_fail() { echo "PREFLIGHT FAIL: $*" >&2; exit 1; }
note() { echo "[$(date -u +%H:%M:%SZ)] $*"; }

[[ -x "$WRAPPER" ]] || preflight_fail "wrapper missing or not executable: $WRAPPER"

for be in "${BACKENDS[@]}"; do
  case "$be" in
    dpdk)       bin="$BUILD_DIR/examples/daqiri_bench_raw_gpudirect" ;;
    socket-udp|socket-tcp) bin="$BUILD_DIR/examples/daqiri_bench_socket" ;;
    rdma)       preflight_fail "RDMA is deferred from PR 1; see follow-up issue" ;;
    *)          preflight_fail "unknown backend: $be" ;;
  esac
  [[ -x "$bin" ]] || preflight_fail "missing bench binary: $bin (run cmake --build first)"
done

# DPDK-only checks.
if [[ " ${BACKENDS[*]} " == *" dpdk "* ]]; then
  free_hp="$(awk '/^HugePages_Free:/ { print $2 }' /proc/meminfo)"
  [[ "${free_hp:-0}" -ge 4 ]] || preflight_fail "HugePages_Free=$free_hp (need >=4); clean /mnt/huge and /dev/hugepages from prior runs"

  if [[ -z "${ETH_DST_ADDR:-}" ]]; then
    mac_path="/sys/class/net/$RX_IFACE/address"
    [[ -r "$mac_path" ]] || preflight_fail "cannot read $mac_path; set ETH_DST_ADDR explicitly"
    ETH_DST_ADDR="$(cat "$mac_path")"
    export ETH_DST_ADDR
    note "ETH_DST_ADDR auto-detected from $RX_IFACE: $ETH_DST_ADDR"
  fi

  carrier="$(cat "/sys/class/net/$RX_IFACE/carrier" 2>/dev/null || echo 0)"
  [[ "$carrier" == "1" ]] || preflight_fail "RX iface $RX_IFACE has no carrier (cable unplugged or link down)"
fi

note "Pre-flight OK. Backends: ${BACKENDS[*]}"
note "Build dir: $BUILD_DIR"
note "Repo root: $REPO_ROOT"

# --- hugepage cleanup helper ----------------------------------------------

# DPDK leaves orphan rtemap_* files when a bench aborts. Clean between runs so
# we don't run out of hugepages mid-sweep.
clean_orphan_hugepages() {
  local pre post freed
  pre="$(awk '/^HugePages_Free:/ { print $2 }' /proc/meminfo)"
  : "${pre:=0}"
  shopt -s nullglob
  # DPDK uses a random per-process file prefix (override with --file-prefix);
  # match anything ending in `map_<digit>` to catch the common shape without
  # nuking unrelated files. Skip any that are still held by a live process.
  for f in /dev/hugepages/*map_[0-9]* /mnt/huge/*map_[0-9]*; do
    if ! fuser -- "$f" >/dev/null 2>&1; then
      rm -f -- "$f" 2>/dev/null || true
    fi
  done
  shopt -u nullglob
  post="$(awk '/^HugePages_Free:/ { print $2 }' /proc/meminfo)"
  : "${post:=0}"
  freed=$((post - pre))
  if [[ "$freed" -gt 0 ]]; then
    note "Freed $freed orphan hugepages (now ${post} free)"
  fi
  return 0
}

# --- driver loop -----------------------------------------------------------

declare -a RESULT_DIRS

run_backend_mode() {
  local backend="$1" mode="$2"
  note "=== Running: $backend $mode ==="
  clean_orphan_hugepages

  # Stream wrapper output live (per-cell CSV rows appear as they finish) while
  # also keeping a log for post-run parsing of the "Results in:" line.
  local log="/tmp/spark_data_fill.$backend.$mode.log"
  local rc=0
  "$WRAPPER" "$backend" "$mode" 2>&1 | tee "$log" || rc=$?
  rc="${PIPESTATUS[0]:-$rc}"

  if [[ "$rc" -eq 0 ]]; then
    local result_dir
    result_dir="$(awk '/^Results in:/ { print $3 }' "$log" | tail -n1)"
    [[ -n "$result_dir" ]] && RESULT_DIRS+=("$backend/$mode -> $result_dir")
    note "$backend $mode complete"
  else
    note "$backend $mode FAILED (exit $rc); continuing"
    tail -n 40 "$log" >&2
  fi
  clean_orphan_hugepages
}

for be in "${BACKENDS[@]}"; do
  run_backend_mode "$be" sweep
  run_backend_mode "$be" drop-curve
done

# --- summary ---------------------------------------------------------------

echo
echo "=========================================="
echo "Data-fill complete. Result directories:"
echo "=========================================="
for r in "${RESULT_DIRS[@]}"; do
  echo "  $r"
done
echo
echo "Next: aggregate CSVs and fill docs/performance-dgx-spark.md."
