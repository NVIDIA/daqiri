# DAQIRI Grafana Metrics Example

This example runs `daqiri_bench_raw_gpudirect` for 60 seconds with DAQIRI's
OpenTelemetry metrics exposed through a Prometheus pull endpoint, scraped by
Prometheus, and visualized in a local Grafana dashboard.

## Ports

| Service | URL |
| --- | --- |
| Grafana | <http://localhost:3000> |
| Prometheus | <http://localhost:9090> |
| DAQIRI metrics | <http://localhost:9464/metrics> |

Grafana is provisioned with the `DAQIRI OpenTelemetry Metrics` dashboard and a
Prometheus datasource. The default Grafana login is `admin` / `daqiri`; anonymous
viewer access is also enabled for the local example.

## Build

Build `daqiri:local` with DAQIRI metrics and the OpenTelemetry Prometheus exporter:

```bash
DAQIRI_ENABLE_OTEL_METRICS=ON DAQIRI_MGR="dpdk socket rdma" scripts/build-container.sh
```

## Run

Update `examples/daqiri_bench_raw_tx_rx.yaml` for your NIC, GPU, MAC, IP, and CPU
core values. Then start the stack:

```bash
cd examples/grafana
docker compose up
```

The DAQIRI benchmark container follows the repository run requirements from
`AGENTS.md`: it runs as root with `privileged: true`, host networking, all NVIDIA
GPUs exposed through the NVIDIA runtime, and `/dev/hugepages` mounted from the
host.

The DAQIRI service runs:

```bash
/opt/daqiri/bin/daqiri_bench_raw_gpudirect /workspace/daqiri/examples/daqiri_bench_raw_tx_rx.yaml --seconds 60
```

To use a different config or binary, override the environment variables:

```bash
DAQIRI_CONFIG=/workspace/daqiri/examples/daqiri_bench_raw_tx_rx.yaml docker compose up
```

Stop the stack with:

```bash
docker compose down
```

The DAQIRI and Prometheus services use host networking so DPDK and the Prometheus
scrape path can use the same host-visible network namespace. Grafana exposes port
`3000` through Docker port mapping.
