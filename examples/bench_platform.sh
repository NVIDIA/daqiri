#!/usr/bin/env bash
# Platform selection for the DAQIRI bench suite.
#
# Source this from run_spark_bench.sh / run_spark_mq_bench.sh /
# setup_spark_wire_loopback_netns.sh. It loads examples/bench_platform_<P>.env
# (selected by $BENCH_PLATFORM) and exposes:
#   - all profile variables (BENCH_MEM_KIND, CORE_*, DPDK_*_PCI, WIRE_*, ...)
#   - bench_fill_placeholders <file>  -> stdout with every @VAR@ substituted
#
# Add a platform by dropping in a new bench_platform_<name>.env next to this file.
#
# BENCH_PLATFORM defaults to "spark" (the documented reference platform). Set
# BENCH_PLATFORM=igx for the IGX Orin devkit.

_bench_platform_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_PLATFORM="${BENCH_PLATFORM:-spark}"
_bench_profile="$_bench_platform_dir/bench_platform_${BENCH_PLATFORM}.env"

if [[ ! -f "$_bench_profile" ]]; then
  echo "ERROR: unknown BENCH_PLATFORM='$BENCH_PLATFORM' (no $_bench_profile)" >&2
  echo "       available:" "$(cd "$_bench_platform_dir" && ls bench_platform_*.env 2>/dev/null | sed -E 's/bench_platform_(.*)\.env/\1/' | tr '\n' ' ')" >&2
  return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1090
source "$_bench_profile"
echo "bench platform: $BENCH_PLATFORM  (mem_kind=$BENCH_MEM_KIND, master=$CORE_MASTER)  [set BENCH_PLATFORM=<name> to change]" >&2

# Substitute @VAR@ tokens in a config template, writing the result to stdout.
# Every placeholder used by the config templates must be listed here.
bench_fill_placeholders() {
  local f="$1"
  sed -E \
    -e "s|@MEM_KIND@|${BENCH_MEM_KIND}|g" \
    -e "s|@NUM_BUFS_TX@|${BENCH_NUM_BUFS_TX}|g" \
    -e "s|@NUM_BUFS_RX@|${BENCH_NUM_BUFS_RX}|g" \
    -e "s|@DPDK_TX_PCI@|${DPDK_TX_PCI}|g" \
    -e "s|@DPDK_RX_PCI@|${DPDK_RX_PCI}|g" \
    -e "s|@CORE_MASTER@|${CORE_MASTER}|g" \
    -e "s|@CORE_DPDK_TXQ@|${CORE_DPDK_TXQ}|g" \
    -e "s|@CORE_DPDK_RXQ@|${CORE_DPDK_RXQ}|g" \
    -e "s|@CORE_DPDK_TXW@|${CORE_DPDK_TXW}|g" \
    -e "s|@CORE_DPDK_RXW@|${CORE_DPDK_RXW}|g" \
    -e "s|@CORE_MQ_TXQ0@|${CORE_MQ_TXQ0}|g" \
    -e "s|@CORE_MQ_TXW0@|${CORE_MQ_TXW0}|g" \
    -e "s|@CORE_MQ_TXQ1@|${CORE_MQ_TXQ1}|g" \
    -e "s|@CORE_MQ_TXW1@|${CORE_MQ_TXW1}|g" \
    -e "s|@CORE_MQ_RXQ0@|${CORE_MQ_RXQ0}|g" \
    -e "s|@CORE_MQ_RXW0@|${CORE_MQ_RXW0}|g" \
    -e "s|@CORE_MQ_RXQ1@|${CORE_MQ_RXQ1}|g" \
    -e "s|@CORE_MQ_RXW1@|${CORE_MQ_RXW1}|g" \
    -e "s|@CORE_ROCE_SRV_TXQ@|${CORE_ROCE_SRV_TXQ}|g" \
    -e "s|@CORE_ROCE_SRV_RXQ@|${CORE_ROCE_SRV_RXQ}|g" \
    -e "s|@CORE_ROCE_SRV_W@|${CORE_ROCE_SRV_W}|g" \
    -e "s|@CORE_ROCE_CLI_TXQ@|${CORE_ROCE_CLI_TXQ}|g" \
    -e "s|@CORE_ROCE_CLI_RXQ@|${CORE_ROCE_CLI_RXQ}|g" \
    -e "s|@CORE_ROCE_CLI_W@|${CORE_ROCE_CLI_W}|g" \
    "$f"
}
