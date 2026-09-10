#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Uniform controller for one-way DAQIRI benchmark roles. It supports a physical
# cross-host path and a single-host cable loopback: both run an RX role first,
# wait for it to initialize, run a timed TX role, and retain the RX grace period
# outside the throughput window. It saves role logs, invocation metadata, and
# optional before/after counter snapshots per repetition on the controller host.
#
# It does not configure NICs, addresses, routes, namespaces, CPU placement, or
# packet pacing. Those are properties of role-specific YAML files and a prepared
# local profile. Consequently this controller contains no platform, interface,
# address, PCI, MAC, or CPU identifiers and works with raw Ethernet, RoCE, TCP,
# and UDP benchmark binaries.

set -uo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_crosshost_bench.sh \
    --topology crosshost|loopback --tx-host SSH_DEST --rx-host SSH_DEST --workdir REMOTE_DIR \
    --bench REMOTE_BENCH --tx-config REMOTE_YAML --rx-config REMOTE_YAML \
    [options]

  scripts/run_crosshost_bench.sh --suite spark-loopback-report \
    --loopback-tx-netdev NETDEV --loopback-rx-netdev NETDEV [options]

Required arguments:
  --topology MODE         crosshost (default) or loopback. Loopback requires
                          both role hosts to be `local`.
  --tx-host DEST           SSH destination for the transmit role, or `local`.
  --rx-host DEST           SSH destination for the receive role, or `local`.
  --workdir DIR            Working directory on both hosts.
  --bench PATH             Benchmark executable path, relative to --workdir or absolute.
  --tx-bench PATH          TX executable override (default: --bench).
  --rx-bench PATH          RX executable override (default: --bench).
  --tx-config PATH         TX-role YAML path, relative to --workdir or absolute.
  --rx-config PATH         RX-role YAML path, relative to --workdir or absolute.

Options:
  --suite NAME            Run a named suite. `spark-loopback-report` runs the
                          complete local I/O report and does not take role args.
  --loopback-tx-netdev N  Cabled transmit port for spark-loopback-report.
  --loopback-rx-netdev N  Cabled receive port for spark-loopback-report.
  --skip-raw              Skip raw phases when resuming spark-loopback-report.
  --skip-to-udp           Skip raw, RoCE, and TCP when resuming that suite.
  --seconds N              TX run duration in seconds (default: 30).
  --rx-seconds N           RX run duration (default: seconds + startup delay + grace;
                         required with --rx-ready-pattern).
  --startup-delay N        Seconds to wait after starting RX (default: 3).
  --rx-ready-pattern TEXT  Start TX only after RX log contains this fixed string.
  --ready-timeout N        Seconds to wait for --rx-ready-pattern (default: 60).
  --rx-grace N             Extra RX seconds after TX completes (default: 5).
  --repeats N              Independent repetitions (default: 3).
  --tx-arg ARG             Add one argument to the TX benchmark; repeat as needed.
  --rx-arg ARG             Add one argument to the RX benchmark; repeat as needed.
  --tx-env NAME=VALUE      Add one environment assignment to the TX benchmark.
  --rx-env NAME=VALUE      Add one environment assignment to the RX benchmark.
  --tx-prefix ARG          Prepend one TX launcher argument; repeat as needed.
  --rx-prefix ARG          Prepend one RX launcher argument; repeat as needed.
  --ssh-option OPTION      Pass one `-o OPTION` setting to ssh; repeat as needed.
  --output-dir DIR         Local directory for logs (default: bench-results/<topology>-<UTC timestamp>).
  --protocol NAME          Result label, for example raw, roce, tcp, or udp.
  --tx-engine NAME         Transmit-engine result label.
  --rx-engine NAME         Receive-engine result label.
  --pace MODE              Result label: unpaced, software, or nic.
  --tag NAME=VALUE         Add reproducibility metadata; repeat as needed.
  --tx-snapshot COMMAND    Collect this command on TX before TX and after RX.
  --rx-snapshot COMMAND    Collect this command on RX before TX and after RX.
  --require-snapshots      Treat a requested snapshot failure as a failed repetition.
  --dry-run                Print the role commands without contacting either host.
  -h, --help               Show this help text.

Examples:
  # Raw Ethernet: role-specific configs, no --mode argument.
  scripts/run_crosshost_bench.sh \
    --topology crosshost --tx-host tx-host --rx-host rx-host --workdir /work/daqiri \
    --bench ./build/examples/daqiri_bench_raw_gpudirect \
    --tx-config examples/raw-tx.yaml --rx-config examples/raw-rx.yaml

  # Socket or RoCE: pass the role mode as separate, safely quoted arguments.
  scripts/run_crosshost_bench.sh \
    --topology crosshost --tx-host tx-host --rx-host rx-host --workdir /work/daqiri \
    --bench ./build/examples/daqiri_bench_socket \
    --tx-config examples/socket-client.yaml --rx-config examples/socket-server.yaml \
    --tx-arg --mode --tx-arg client --rx-arg --mode --rx-arg server

Requested snapshots are preserved verbatim; use them to collect the application,
kernel, NIC, and PHY evidence appropriate to the transport. This controller does
not configure the testbed or infer loss from matching PHY counters.
EOF
}

die() {
  echo "error: $*" >&2
  exit 2
}

need_value() {
  [[ $# -ge 2 && -n "${2:-}" ]] || die "missing value for $1"
}

is_nonnegative_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

TX_HOST=""
RX_HOST=""
TOPOLOGY="crosshost"
SUITE=""
LOOPBACK_TX_NETDEV="${LOOPBACK_TX_NETDEV:-}"
LOOPBACK_RX_NETDEV="${LOOPBACK_RX_NETDEV:-}"
SKIP_RAW="${SKIP_RAW:-}"
SKIP_TO_UDP="${SKIP_TO_UDP:-}"
WORKDIR=""
BENCH=""
TX_BENCH=""
RX_BENCH=""
TX_CONFIG=""
RX_CONFIG=""
RUN_SECONDS=30
RX_SECONDS=""
RX_SECONDS_SET=0
STARTUP_DELAY=3
RX_READY_PATTERN=""
READY_TIMEOUT=60
RX_GRACE=5
REPEATS=3
OUTPUT_DIR=""
DRY_RUN=0
PROTOCOL=""
TX_ENGINE=""
RX_ENGINE=""
PACE=""
TX_SNAPSHOT=""
RX_SNAPSHOT=""
REQUIRE_SNAPSHOTS=0
TX_ARGS=()
RX_ARGS=()
TX_ENV=()
RX_ENV=()
TX_PREFIX=()
RX_PREFIX=()
SSH_OPTIONS=()
TAGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --suite) need_value "$@"; SUITE="$2"; shift 2 ;;
    --topology) need_value "$@"; TOPOLOGY="$2"; shift 2 ;;
    --loopback-tx-netdev) need_value "$@"; LOOPBACK_TX_NETDEV="$2"; shift 2 ;;
    --loopback-rx-netdev) need_value "$@"; LOOPBACK_RX_NETDEV="$2"; shift 2 ;;
    --skip-raw) SKIP_RAW=1; shift ;;
    --skip-to-udp) SKIP_TO_UDP=1; shift ;;
    --tx-host) need_value "$@"; TX_HOST="$2"; shift 2 ;;
    --rx-host) need_value "$@"; RX_HOST="$2"; shift 2 ;;
    --workdir) need_value "$@"; WORKDIR="$2"; shift 2 ;;
    --bench) need_value "$@"; BENCH="$2"; shift 2 ;;
    --tx-bench) need_value "$@"; TX_BENCH="$2"; shift 2 ;;
    --rx-bench) need_value "$@"; RX_BENCH="$2"; shift 2 ;;
    --tx-config) need_value "$@"; TX_CONFIG="$2"; shift 2 ;;
    --rx-config) need_value "$@"; RX_CONFIG="$2"; shift 2 ;;
    --seconds) need_value "$@"; RUN_SECONDS="$2"; shift 2 ;;
    --rx-seconds) need_value "$@"; RX_SECONDS="$2"; RX_SECONDS_SET=1; shift 2 ;;
    --startup-delay) need_value "$@"; STARTUP_DELAY="$2"; shift 2 ;;
    --rx-ready-pattern) need_value "$@"; RX_READY_PATTERN="$2"; shift 2 ;;
    --ready-timeout) need_value "$@"; READY_TIMEOUT="$2"; shift 2 ;;
    --rx-grace) need_value "$@"; RX_GRACE="$2"; shift 2 ;;
    --repeats) need_value "$@"; REPEATS="$2"; shift 2 ;;
    --tx-arg) need_value "$@"; TX_ARGS+=("$2"); shift 2 ;;
    --rx-arg) need_value "$@"; RX_ARGS+=("$2"); shift 2 ;;
    --tx-env) need_value "$@"; TX_ENV+=("$2"); shift 2 ;;
    --rx-env) need_value "$@"; RX_ENV+=("$2"); shift 2 ;;
    --tx-prefix) need_value "$@"; TX_PREFIX+=("$2"); shift 2 ;;
    --rx-prefix) need_value "$@"; RX_PREFIX+=("$2"); shift 2 ;;
    --ssh-option) need_value "$@"; SSH_OPTIONS+=("-o" "$2"); shift 2 ;;
    --output-dir) need_value "$@"; OUTPUT_DIR="$2"; shift 2 ;;
    --protocol) need_value "$@"; PROTOCOL="$2"; shift 2 ;;
    --tx-engine) need_value "$@"; TX_ENGINE="$2"; shift 2 ;;
    --rx-engine) need_value "$@"; RX_ENGINE="$2"; shift 2 ;;
    --pace) need_value "$@"; PACE="$2"; shift 2 ;;
    --tag) need_value "$@"; TAGS+=("$2"); shift 2 ;;
    --tx-snapshot) need_value "$@"; TX_SNAPSHOT="$2"; shift 2 ;;
    --rx-snapshot) need_value "$@"; RX_SNAPSHOT="$2"; shift 2 ;;
    --require-snapshots) REQUIRE_SNAPSHOTS=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

case "$TOPOLOGY" in
  crosshost|loopback) ;;
  *) die "--topology must be crosshost or loopback" ;;
esac
if [[ -n "$SUITE" ]]; then
  [[ "$SUITE" == "spark-loopback-report" ]] || die "unknown suite: $SUITE"
  [[ "$TOPOLOGY" == "loopback" ]] || die "spark-loopback-report requires --topology loopback"
  [[ -n "$LOOPBACK_TX_NETDEV" && -n "$LOOPBACK_RX_NETDEV" ]] || \
    die "spark-loopback-report requires --loopback-tx-netdev and --loopback-rx-netdev"
else
  [[ -n "$TX_HOST" ]] || die "--tx-host is required"
  [[ -n "$RX_HOST" ]] || die "--rx-host is required"
  if [[ "$TOPOLOGY" == "loopback" && ( "$TX_HOST" != "local" || "$RX_HOST" != "local" ) ]]; then
    die "--topology loopback requires --tx-host local --rx-host local"
  fi
  [[ -n "$WORKDIR" ]] || die "--workdir is required"
  [[ -n "$BENCH" ]] || die "--bench is required"
  [[ -n "$TX_CONFIG" ]] || die "--tx-config is required"
  [[ -n "$RX_CONFIG" ]] || die "--rx-config is required"
  is_positive_integer "$RUN_SECONDS" || die "--seconds must be a positive integer"
  is_nonnegative_integer "$STARTUP_DELAY" || die "--startup-delay must be a non-negative integer"
  is_positive_integer "$READY_TIMEOUT" || die "--ready-timeout must be a positive integer"
  is_nonnegative_integer "$RX_GRACE" || die "--rx-grace must be a non-negative integer"
  is_positive_integer "$REPEATS" || die "--repeats must be a positive integer"

  TX_BENCH=${TX_BENCH:-$BENCH}
  RX_BENCH=${RX_BENCH:-$BENCH}

  if [[ -n "$RX_READY_PATTERN" && "$RX_SECONDS_SET" -eq 0 ]]; then
    die "--rx-seconds is required with --rx-ready-pattern"
  fi
  if [[ -z "$RX_SECONDS" ]]; then
    RX_SECONDS=$((RUN_SECONDS + STARTUP_DELAY + RX_GRACE))
  fi
  is_positive_integer "$RX_SECONDS" || die "--rx-seconds must be a positive integer"

  if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR="bench-results/${TOPOLOGY}-$(date -u +%Y%m%dT%H%M%SZ)"
  fi
fi

REMOTE_SCRIPT='set -euo pipefail
workdir="$1"
bench="$2"
config="$3"
seconds="$4"
env_count="$5"
prefix_count="$6"
shift 6
env_args=()
for ((index = 0; index < env_count; index++)); do
  env_args+=("$1")
  shift
done
prefix_args=()
for ((index = 0; index < prefix_count; index++)); do
  prefix_args+=("$1")
  shift
done
cd "$workdir"
exec env "${env_args[@]}" "${prefix_args[@]}" "$bench" "$config" --seconds "$seconds" "$@"
'

format_role_command() {
  local host="$1" bench="$2" config="$3" role_seconds="$4"
  local env_name="$5" prefix_name="$6" args_name="$7"
  local -n role_env="$env_name"
  local -n role_prefix="$prefix_name"
  local -n role_args="$args_name"
  if [[ "$host" == "local" ]]; then
    printf 'bash -s --'
  else
    printf 'ssh'
    if ((${#SSH_OPTIONS[@]})); then
      printf ' %q' "${SSH_OPTIONS[@]}"
    fi
    printf ' %q' "$host" bash -s --
  fi
  printf ' %q' "$WORKDIR" "$bench" "$config" "$role_seconds" "${#role_env[@]}" "${#role_prefix[@]}"
  if ((${#role_env[@]})); then
    printf ' %q' "${role_env[@]}"
  fi
  if ((${#role_prefix[@]})); then
    printf ' %q' "${role_prefix[@]}"
  fi
  if ((${#role_args[@]})); then
    printf ' %q' "${role_args[@]}"
  fi
  printf '\n'
}

launch_role() {
  local host="$1" bench="$2" config="$3" role_seconds="$4" log="$5"
  local env_name="$6" prefix_name="$7" args_name="$8"
  local -n role_env="$env_name"
  local -n role_prefix="$prefix_name"
  local -n role_args="$args_name"
  if [[ "$host" == "local" ]]; then
    bash -s -- \
      "$WORKDIR" "$bench" "$config" "$role_seconds" "${#role_env[@]}" "${#role_prefix[@]}" \
      "${role_env[@]}" "${role_prefix[@]}" "${role_args[@]}" \
      <<< "$REMOTE_SCRIPT" > "$log" 2>&1 &
  else
    ssh "${SSH_OPTIONS[@]}" "$host" bash -s -- \
      "$WORKDIR" "$bench" "$config" "$role_seconds" "${#role_env[@]}" "${#role_prefix[@]}" \
      "${role_env[@]}" "${role_prefix[@]}" "${role_args[@]}" \
      <<< "$REMOTE_SCRIPT" > "$log" 2>&1 &
  fi
  ROLE_PID=$!
}

run_snapshot() {
  local host="$1" command="$2" output="$3"
  [[ -n "$command" ]] || return 0
  if [[ "$host" == "local" ]]; then
    bash -lc "$command" > "$output" 2>&1
  else
    ssh "${SSH_OPTIONS[@]}" "$host" bash -lc "$command" > "$output" 2>&1
  fi
}

csv_escape() {
  local value="$1"
  value=${value//\"/\"\"}
  printf '"%s"' "$value"
}

tags_value() {
  local tag value=""
  for tag in "${TAGS[@]}"; do
    value+="${value:+;}$(printf '%q' "$tag")"
  done
  printf '%s' "$value"
}

wait_for_rx_ready() {
  local rx_pid="$1" rx_log="$2"
  local elapsed=0
  while ! grep -Fq -- "$RX_READY_PATTERN" "$rx_log" 2>/dev/null; do
    if ! kill -0 "$rx_pid" 2>/dev/null; then
      return 1
    fi
    if (( elapsed >= READY_TIMEOUT )); then
      return 1
    fi
    sleep 1
    ((elapsed += 1))
  done
}

run_spark_loopback_report() {
  local repo_dir example_dir build_dir raw_seconds socket_seconds rdma_seconds
  local report_dir phase_log failures=0 eth_dst_addr namespaces_active=0 raw_tx_pci raw_rx_pci
  repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
  example_dir="$repo_dir/examples"
  build_dir="${DAQIRI_BUILD_DIR:-$repo_dir/build}"
  raw_seconds="${RAW_SECONDS:-30}"
  socket_seconds="${SOCKET_SECONDS:-30}"
  rdma_seconds="${RDMA_SECONDS:-30}"

  if (( DRY_RUN )); then
    cat <<EOF
suite=spark-loopback-report topology=loopback
raw: DPDK payload and multi-queue phases in the default namespace
wire namespaces: RoCE, TCP, and UDP phases
raw paced: ibverbs TX / DPDK RX small-packet phases after namespace teardown
EOF
    return
  fi

  for netdev in "$LOOPBACK_TX_NETDEV" "$LOOPBACK_RX_NETDEV"; do
    [[ -r "/sys/class/net/$netdev/address" ]] || die "cannot read MAC address for loopback interface $netdev"
  done
  [[ "$LOOPBACK_TX_NETDEV" != "$LOOPBACK_RX_NETDEV" ]] || \
    die "loopback TX and RX interfaces must be different ports"
  for duration in "$raw_seconds" "$socket_seconds" "$rdma_seconds"; do
    is_positive_integer "$duration" || die "suite sample durations must be positive integers"
  done
  [[ -x "$example_dir/run_spark_bench.sh" ]] || die "missing loopback cell adapter"
  [[ -x "$example_dir/run_spark_mq_bench.sh" ]] || die "missing multi-queue cell adapter"
  [[ -x "$example_dir/run_spark_raw_paced_tx.sh" ]] || die "missing paced-TX cell adapter"

  eth_dst_addr="${ETH_DST_ADDR:-$(<"/sys/class/net/$LOOPBACK_RX_NETDEV/address")}"
  raw_tx_pci="$(basename "$(readlink -f "/sys/class/net/$LOOPBACK_TX_NETDEV/device")")"
  raw_rx_pci="$(basename "$(readlink -f "/sys/class/net/$LOOPBACK_RX_NETDEV/device")")"
  export DAQIRI_BUILD_DIR="$build_dir"
  export LD_LIBRARY_PATH="$build_dir/src:$build_dir:/opt/daqiri/lib:${LD_LIBRARY_PATH:-}"
  report_dir="${OUTPUT_DIR:-$repo_dir/bench-results/loopback-$(date -u +%Y%m%dT%H%M%SZ)}"
  mkdir -p "$report_dir"
  phase_log="$report_dir/phases.log"

  run_suite_phase() {
    local name="$1"
    shift
    echo "[$(date -u +%FT%TZ)] BEGIN $name" | tee -a "$phase_log"
    if "$@" 2>&1 | tee "$report_dir/$name.log"; then
      echo "[$(date -u +%FT%TZ)] PASS  $name" | tee -a "$phase_log"
    else
      echo "[$(date -u +%FT%TZ)] FAIL  $name (continuing)" | tee -a "$phase_log" >&2
      failures=$((failures + 1))
    fi
  }

  cleanup_loopback_namespaces() {
    if (( namespaces_active )); then
      echo "[$(date -u +%FT%TZ)] restoring loopback namespace state" | tee -a "$phase_log" >&2
      env CLIENT_IF="$LOOPBACK_TX_NETDEV" SERVER_IF="$LOOPBACK_RX_NETDEV" \
        "$repo_dir/scripts/setup_spark_wire_loopback_netns.sh" down || true
      namespaces_active=0
    fi
  }

  if [[ -z "$SKIP_RAW" ]]; then
    if ip netns list 2>/dev/null | grep -q '^dq_wire_'; then
      die "wire namespaces are already present; tear them down before raw phases"
    fi
    run_suite_phase raw-payload \
      env ETH_DST_ADDR="$eth_dst_addr" RUN_SECONDS="$raw_seconds" REPEATS=3 BATCHES_OVERRIDE=10240 \
      "$example_dir/run_spark_bench.sh" dpdk sweep
    run_suite_phase raw-256b-scaling \
      env ETH_DST_ADDR="$eth_dst_addr" RUN_SECONDS="$raw_seconds" REPEATS=3 PAYLOADS=256 \
      "$example_dir/run_spark_mq_bench.sh"
  else
    echo "[$(date -u +%FT%TZ)] raw phases skipped" | tee -a "$phase_log"
  fi

  run_suite_phase wire-namespaces-up \
    env CLIENT_IF="$LOOPBACK_TX_NETDEV" SERVER_IF="$LOOPBACK_RX_NETDEV" \
    "$repo_dir/scripts/setup_spark_wire_loopback_netns.sh" up
  namespaces_active=1
  trap cleanup_loopback_namespaces EXIT
  trap 'cleanup_loopback_namespaces; exit 130' INT TERM

  if [[ -z "$SKIP_TO_UDP" ]]; then
    run_suite_phase roce-payload \
      env RUN_SECONDS="$rdma_seconds" REPEATS=3 "$example_dir/run_spark_bench.sh" rdma sweep
    run_suite_phase tcp-message-size \
      env RUN_SECONDS="$socket_seconds" REPEATS=3 PAIRS_OVERRIDE=1 \
      "$example_dir/run_spark_bench.sh" socket-tcp sweep
    run_suite_phase tcp-worker-scaling \
      env RUN_SECONDS="$socket_seconds" REPEATS=3 PAYLOADS_OVERRIDE=1048576 PAIRS_OVERRIDE="1 2 4" \
      "$example_dir/run_spark_bench.sh" socket-tcp sweep
  else
    echo "[$(date -u +%FT%TZ)] RoCE and TCP phases skipped" | tee -a "$phase_log"
  fi

  run_suite_phase udp-8000b-knee \
    env RUN_SECONDS="$socket_seconds" REPEATS=3 PAIRS_OVERRIDE=1 PAYLOADS_OVERRIDE=8000 RATE_TARGETS="22 24 28" \
    "$example_dir/run_spark_bench.sh" socket-udp rate-sweep
  run_suite_phase udp-1000b-knee \
    env RUN_SECONDS="$socket_seconds" REPEATS=3 PAIRS_OVERRIDE=1 PAYLOADS_OVERRIDE=1000 RATE_TARGETS="5 6 7" \
    "$example_dir/run_spark_bench.sh" socket-udp rate-sweep
  run_suite_phase udp-65507b-knee \
    env RUN_SECONDS="$socket_seconds" REPEATS=3 PAIRS_OVERRIDE=1 PAYLOADS_OVERRIDE=65507 RATE_TARGETS="16 17 18" \
    "$example_dir/run_spark_bench.sh" socket-udp rate-sweep
  run_suite_phase udp-2worker-knee \
    env RUN_SECONDS="$socket_seconds" REPEATS=3 PAYLOADS_OVERRIDE=8000 PAIRS_OVERRIDE=2 RATE_TARGETS="22 24 28" \
    "$example_dir/run_spark_bench.sh" socket-udp rate-sweep
  run_suite_phase udp-4worker-knee \
    env RUN_SECONDS="$socket_seconds" REPEATS=3 PAYLOADS_OVERRIDE=8000 PAIRS_OVERRIDE=4 RATE_TARGETS="18 20 22" \
    "$example_dir/run_spark_bench.sh" socket-udp rate-sweep
  run_suite_phase roce-small-message-window \
    env RUN_SECONDS="$rdma_seconds" REPEATS=3 PAYLOADS_OVERRIDE="8192 4096" RDMA_TX_NB=512 \
    "$example_dir/run_spark_bench.sh" rdma sweep

  run_suite_phase wire-namespaces-down \
    env CLIENT_IF="$LOOPBACK_TX_NETDEV" SERVER_IF="$LOOPBACK_RX_NETDEV" \
    "$repo_dir/scripts/setup_spark_wire_loopback_netns.sh" down
  namespaces_active=0
  trap - EXIT INT TERM
  run_suite_phase raw-256b-ibverbs-paced-tx \
    env ETH_DST_ADDR="$eth_dst_addr" TX_PCI="$raw_tx_pci" RX_PCI="$raw_rx_pci" RUN_SECONDS="$raw_seconds" REPEATS=3 PAYLOADS=256 PACINGS="40000 50000 60000" \
    "$example_dir/run_spark_raw_paced_tx.sh"
  run_suite_phase raw-64b-ibverbs-paced-tx \
    env ETH_DST_ADDR="$eth_dst_addr" TX_PCI="$raw_tx_pci" RX_PCI="$raw_rx_pci" RUN_SECONDS="$raw_seconds" REPEATS=3 PAYLOADS=64 PACINGS="21000 22000 23000 24000 24500 25000" \
    "$example_dir/run_spark_raw_paced_tx.sh"

  echo "Report phase log: $phase_log"
  (( failures == 0 )) || return 1
}

if [[ -n "$SUITE" ]]; then
  run_spark_loopback_report
  exit $?
fi

if (( DRY_RUN )); then
  echo "topology=$TOPOLOGY protocol=${PROTOCOL:-unspecified} tx_engine=${TX_ENGINE:-unspecified} rx_engine=${RX_ENGINE:-unspecified} pace=${PACE:-unspecified}"
  if [[ -n "$RX_READY_PATTERN" ]]; then
    echo "RX starts first; TX starts after its log contains '$RX_READY_PATTERN'."
  else
    echo "RX starts first; TX starts after ${STARTUP_DELAY}s."
  fi
  format_role_command "$RX_HOST" "$RX_BENCH" "$RX_CONFIG" "$RX_SECONDS" RX_ENV RX_PREFIX RX_ARGS
  format_role_command "$TX_HOST" "$TX_BENCH" "$TX_CONFIG" "$RUN_SECONDS" TX_ENV TX_PREFIX TX_ARGS
  [[ -n "$RX_SNAPSHOT" ]] && echo "RX snapshots: $RX_SNAPSHOT"
  [[ -n "$TX_SNAPSHOT" ]] && echo "TX snapshots: $TX_SNAPSHOT"
  exit 0
fi

mkdir -p "$OUTPUT_DIR"
{
  printf 'topology=%q\n' "$TOPOLOGY"
  printf 'tx_host=%q\n' "$TX_HOST"
  printf 'rx_host=%q\n' "$RX_HOST"
  printf 'workdir=%q\n' "$WORKDIR"
  printf 'bench=%q\n' "$BENCH"
  printf 'tx_bench=%q\n' "$TX_BENCH"
  printf 'rx_bench=%q\n' "$RX_BENCH"
  printf 'tx_config=%q\n' "$TX_CONFIG"
  printf 'rx_config=%q\n' "$RX_CONFIG"
  printf 'tx_seconds=%q\n' "$RUN_SECONDS"
  printf 'rx_seconds=%q\n' "$RX_SECONDS"
  printf 'startup_delay=%q\n' "$STARTUP_DELAY"
  printf 'rx_ready_pattern=%q\n' "$RX_READY_PATTERN"
  printf 'ready_timeout=%q\n' "$READY_TIMEOUT"
  printf 'rx_grace=%q\n' "$RX_GRACE"
  printf 'repeats=%q\n' "$REPEATS"
  printf 'protocol=%q\n' "$PROTOCOL"
  printf 'tx_engine=%q\n' "$TX_ENGINE"
  printf 'rx_engine=%q\n' "$RX_ENGINE"
  printf 'pace=%q\n' "$PACE"
  printf 'tags=%q\n' "$(tags_value)"
  printf 'tx_snapshot=%q\n' "$TX_SNAPSHOT"
  printf 'rx_snapshot=%q\n' "$RX_SNAPSHOT"
} > "$OUTPUT_DIR/invocation.txt"

child_pids=()
cleanup() {
  local pid
  for pid in "${child_pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
}
trap cleanup INT TERM

overall_rc=0
printf 'rep,tx_exit,rx_exit\n' > "$OUTPUT_DIR/exit-status.csv"
printf 'rep,topology,protocol,tx_engine,rx_engine,pace,tx_seconds,rx_seconds,rx_grace,tx_exit,rx_exit,tags\n' > "$OUTPUT_DIR/runs.csv"
for ((rep = 1; rep <= REPEATS; rep++)); do
  rx_log="$OUTPUT_DIR/r${rep}-rx.log"
  tx_log="$OUTPUT_DIR/r${rep}-tx.log"
  echo "[$rep/$REPEATS] starting RX on $RX_HOST" >&2
  launch_role "$RX_HOST" "$RX_BENCH" "$RX_CONFIG" "$RX_SECONDS" "$rx_log" RX_ENV RX_PREFIX RX_ARGS
  rx_pid=$ROLE_PID
  child_pids+=("$rx_pid")

  if [[ -n "$RX_READY_PATTERN" ]]; then
    echo "[$rep/$REPEATS] waiting for RX readiness" >&2
    if ! wait_for_rx_ready "$rx_pid" "$rx_log"; then
      wait "$rx_pid"
      rx_rc=$?
      printf '%s,%s,%s\n' "$rep" 125 "$rx_rc" >> "$OUTPUT_DIR/exit-status.csv"
      printf '%s,' "$rep" >> "$OUTPUT_DIR/runs.csv"
      csv_escape "$TOPOLOGY" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
      csv_escape "$PROTOCOL" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
      csv_escape "$TX_ENGINE" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
      csv_escape "$RX_ENGINE" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
      csv_escape "$PACE" >> "$OUTPUT_DIR/runs.csv"
      printf ',%s,%s,%s,125,%s,' "$RUN_SECONDS" "$RX_SECONDS" "$RX_GRACE" "$rx_rc" >> "$OUTPUT_DIR/runs.csv"
      csv_escape "$(tags_value)" >> "$OUTPUT_DIR/runs.csv"; printf '\n' >> "$OUTPUT_DIR/runs.csv"
      overall_rc=1
      echo "[$rep/$REPEATS] RX did not become ready within ${READY_TIMEOUT}s; see $rx_log" >&2
      continue
    fi
  else
    sleep "$STARTUP_DELAY"
  fi
  snapshot_failed=0
  if ! run_snapshot "$RX_HOST" "$RX_SNAPSHOT" "$OUTPUT_DIR/r${rep}-rx-before.txt"; then
    echo "[$rep/$REPEATS] RX pre-transfer snapshot failed" >&2
    snapshot_failed=1
  fi
  if ! run_snapshot "$TX_HOST" "$TX_SNAPSHOT" "$OUTPUT_DIR/r${rep}-tx-before.txt"; then
    echo "[$rep/$REPEATS] TX pre-transfer snapshot failed" >&2
    snapshot_failed=1
  fi
  echo "[$rep/$REPEATS] starting TX on $TX_HOST" >&2
  launch_role "$TX_HOST" "$TX_BENCH" "$TX_CONFIG" "$RUN_SECONDS" "$tx_log" TX_ENV TX_PREFIX TX_ARGS
  tx_pid=$ROLE_PID
  child_pids+=("$tx_pid")

  wait "$tx_pid"
  tx_rc=$?
  wait "$rx_pid"
  rx_rc=$?
  if ! run_snapshot "$TX_HOST" "$TX_SNAPSHOT" "$OUTPUT_DIR/r${rep}-tx-after.txt"; then
    echo "[$rep/$REPEATS] TX post-transfer snapshot failed" >&2
    snapshot_failed=1
  fi
  if ! run_snapshot "$RX_HOST" "$RX_SNAPSHOT" "$OUTPUT_DIR/r${rep}-rx-after.txt"; then
    echo "[$rep/$REPEATS] RX post-transfer snapshot failed" >&2
    snapshot_failed=1
  fi
  printf '%s,%s,%s\n' "$rep" "$tx_rc" "$rx_rc" >> "$OUTPUT_DIR/exit-status.csv"
  printf '%s,' "$rep" >> "$OUTPUT_DIR/runs.csv"
  csv_escape "$TOPOLOGY" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
  csv_escape "$PROTOCOL" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
  csv_escape "$TX_ENGINE" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
  csv_escape "$RX_ENGINE" >> "$OUTPUT_DIR/runs.csv"; printf ',' >> "$OUTPUT_DIR/runs.csv"
  csv_escape "$PACE" >> "$OUTPUT_DIR/runs.csv"
  printf ',%s,%s,%s,%s,%s,' "$RUN_SECONDS" "$RX_SECONDS" "$RX_GRACE" "$tx_rc" "$rx_rc" >> "$OUTPUT_DIR/runs.csv"
  csv_escape "$(tags_value)" >> "$OUTPUT_DIR/runs.csv"; printf '\n' >> "$OUTPUT_DIR/runs.csv"
  if (( tx_rc != 0 || rx_rc != 0 || (snapshot_failed && REQUIRE_SNAPSHOTS) )); then
    overall_rc=1
    echo "[$rep/$REPEATS] failed (TX=$tx_rc, RX=$rx_rc); see $tx_log and $rx_log" >&2
  else
    echo "[$rep/$REPEATS] complete; logs: $tx_log, $rx_log" >&2
  fi
done

exit "$overall_rc"
