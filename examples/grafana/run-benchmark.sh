#!/usr/bin/env bash
set -euo pipefail

DAQIRI_BIN="${DAQIRI_BIN:-/opt/daqiri/bin/daqiri_bench_raw_gpudirect}"
DAQIRI_CONFIG="${DAQIRI_CONFIG:-/workspace/daqiri/examples/daqiri_bench_raw_tx_rx.yaml}"
DAQIRI_SECONDS="${DAQIRI_SECONDS:-60}"
DAQIRI_OTEL_PROMETHEUS_ENDPOINT="${DAQIRI_OTEL_PROMETHEUS_ENDPOINT:-0.0.0.0:9464}"
export DAQIRI_OTEL_PROMETHEUS_ENDPOINT

echo "Starting DAQIRI Grafana metrics run"
echo "  binary:   ${DAQIRI_BIN}"
echo "  config:   ${DAQIRI_CONFIG}"
echo "  seconds:  ${DAQIRI_SECONDS}"
echo "  metrics:  http://${DAQIRI_OTEL_PROMETHEUS_ENDPOINT}/metrics"

exec "${DAQIRI_BIN}" "${DAQIRI_CONFIG}" --seconds "${DAQIRI_SECONDS}"
