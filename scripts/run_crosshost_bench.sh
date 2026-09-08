#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Controller for a one-way DAQIRI benchmark split across two hosts. It starts the
# receive role first, waits for it to initialize, starts the transmit role, and
# saves one stdout/stderr log per role and repetition on the controller host.
#
# It does not configure NICs, addresses, routes, namespaces, CPU placement, or
# packet pacing. Those are properties of the role-specific YAML files and the
# prepared hosts. Consequently, this script contains no platform or topology
# identifiers and works with raw Ethernet, RoCE, TCP, and UDP benchmark binaries.

set -uo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_crosshost_bench.sh \
    --tx-host SSH_DEST --rx-host SSH_DEST --workdir REMOTE_DIR \
    --bench REMOTE_BENCH --tx-config REMOTE_YAML --rx-config REMOTE_YAML \
    [options]

Required arguments:
  --tx-host DEST           SSH destination for the transmit role, or `local`.
  --rx-host DEST           SSH destination for the receive role, or `local`.
  --workdir DIR            Working directory on both hosts.
  --bench PATH             Benchmark executable path, relative to --workdir or absolute.
  --tx-bench PATH          TX executable override (default: --bench).
  --rx-bench PATH          RX executable override (default: --bench).
  --tx-config PATH         TX-role YAML path, relative to --workdir or absolute.
  --rx-config PATH         RX-role YAML path, relative to --workdir or absolute.

Options:
  --seconds N              TX run duration in seconds (default: 30).
  --rx-seconds N           RX run duration (default: seconds + startup delay + grace;
                         required with --rx-ready-pattern).
  --startup-delay N        Seconds to wait after starting RX (default: 3).
  --rx-ready-pattern TEXT  Start TX only after RX log contains this fixed string.
  --ready-timeout N        Seconds to wait for --rx-ready-pattern (default: 60).
  --rx-grace N             Extra RX seconds after TX completes (default: 5).
  --repeats N              Independent repetitions (default: 1).
  --tx-arg ARG             Add one argument to the TX benchmark; repeat as needed.
  --rx-arg ARG             Add one argument to the RX benchmark; repeat as needed.
  --tx-env NAME=VALUE      Add one environment assignment to the TX benchmark.
  --rx-env NAME=VALUE      Add one environment assignment to the RX benchmark.
  --tx-prefix ARG          Prepend one TX launcher argument; repeat as needed.
  --rx-prefix ARG          Prepend one RX launcher argument; repeat as needed.
  --ssh-option OPTION      Pass one `-o OPTION` setting to ssh; repeat as needed.
  --output-dir DIR         Local directory for logs (default: bench-results/xhost-<UTC timestamp>).
  --dry-run                Print the role commands without contacting either host.
  -h, --help               Show this help text.

Examples:
  # Raw Ethernet: role-specific configs, no --mode argument.
  scripts/run_crosshost_bench.sh \
    --tx-host tx-host --rx-host rx-host --workdir /work/daqiri \
    --bench ./build/examples/daqiri_bench_raw_gpudirect \
    --tx-config examples/raw-tx.yaml --rx-config examples/raw-rx.yaml

  # Socket or RoCE: pass the role mode as separate, safely quoted arguments.
  scripts/run_crosshost_bench.sh \
    --tx-host tx-host --rx-host rx-host --workdir /work/daqiri \
    --bench ./build/examples/daqiri_bench_socket \
    --tx-config examples/socket-client.yaml --rx-config examples/socket-server.yaml \
    --tx-arg --mode --tx-arg client --rx-arg --mode --rx-arg server

The controller records benchmark logs only. Confirm wire transit and loss with
the host counters appropriate to the testbed before treating a result as valid.
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
REPEATS=1
OUTPUT_DIR=""
DRY_RUN=0
TX_ARGS=()
RX_ARGS=()
TX_ENV=()
RX_ENV=()
TX_PREFIX=()
RX_PREFIX=()
SSH_OPTIONS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
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
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ -n "$TX_HOST" ]] || die "--tx-host is required"
[[ -n "$RX_HOST" ]] || die "--rx-host is required"
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
  OUTPUT_DIR="bench-results/xhost-$(date -u +%Y%m%dT%H%M%SZ)"
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

if (( DRY_RUN )); then
  if [[ -n "$RX_READY_PATTERN" ]]; then
    echo "RX starts first; TX starts after its log contains '$RX_READY_PATTERN'."
  else
    echo "RX starts first; TX starts after ${STARTUP_DELAY}s."
  fi
  format_role_command "$RX_HOST" "$RX_BENCH" "$RX_CONFIG" "$RX_SECONDS" RX_ENV RX_PREFIX RX_ARGS
  format_role_command "$TX_HOST" "$TX_BENCH" "$TX_CONFIG" "$RUN_SECONDS" TX_ENV TX_PREFIX TX_ARGS
  exit 0
fi

mkdir -p "$OUTPUT_DIR"
{
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
  printf 'repeats=%q\n' "$REPEATS"
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
      overall_rc=1
      echo "[$rep/$REPEATS] RX did not become ready within ${READY_TIMEOUT}s; see $rx_log" >&2
      continue
    fi
  else
    sleep "$STARTUP_DELAY"
  fi
  echo "[$rep/$REPEATS] starting TX on $TX_HOST" >&2
  launch_role "$TX_HOST" "$TX_BENCH" "$TX_CONFIG" "$RUN_SECONDS" "$tx_log" TX_ENV TX_PREFIX TX_ARGS
  tx_pid=$ROLE_PID
  child_pids+=("$tx_pid")

  wait "$tx_pid"
  tx_rc=$?
  wait "$rx_pid"
  rx_rc=$?
  printf '%s,%s,%s\n' "$rep" "$tx_rc" "$rx_rc" >> "$OUTPUT_DIR/exit-status.csv"
  if (( tx_rc != 0 || rx_rc != 0 )); then
    overall_rc=1
    echo "[$rep/$REPEATS] failed (TX=$tx_rc, RX=$rx_rc); see $tx_log and $rx_log" >&2
  else
    echo "[$rep/$REPEATS] complete; logs: $tx_log, $rx_log" >&2
  fi
done

exit "$overall_rc"
