---
name: daqiri-rtx-pro-benchmark
description: Builds, runs, sweeps, and plots DAQIRI raw-Ethernet benchmarks on RTX PRO 6000 Blackwell discrete-dGPU hosts. Primary path is dual-port L2 wire closed-loop (real NIC traffic); SW loopback is optional build sanity. Use when benchmarking RTX Pro 6000, run_rtx_pro_bench.sh, or bench-results plotting.
alwaysApply: true
---

# RTX PRO 6000 Benchmarking

## When to use

- Host: x86_64 workstation/server with **RTX PRO 6000 Blackwell** (`CMAKE_CUDA_ARCHITECTURES=120`)
- **Wire path:** dual-port NIC with L2 loop (cable, optic, or switch) between TX and RX ports
- **SW path:** optional `loopback: sw` when no link is available (non-wire)
- Goal: wire closed-loop → issue #17 workloads → sweeps → plots

## Quick start

```bash
# 1. Discover topology (host-side, before DPDK bind) — or pass --tx-bdf/--rx-bdf/--rx-mac
./scripts/discover_rtx_pro_topology.sh

# 2. Build (inside privileged container as current user)
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF \
  -DDAQIRI_MGR="dpdk socket rdma ibverbs" -DCMAKE_CUDA_ARCHITECTURES=120
cmake --build build -j

# 3. Wire ladder (run as root) — requires L2 connectivity between TX and RX ports
source scripts/discover_rtx_pro_topology.sh
./examples/run_rtx_pro_bench.sh dpdk nic-smoke --seconds 30
./examples/run_rtx_pro_bench.sh dpdk issue17 --seconds 30

# Optional build sanity only (no NIC, any host):
# ./examples/run_rtx_pro_bench.sh dpdk sw-smoke --seconds 10
```

## Preflight checklist

- [ ] Privileged container, `--runtime=nvidia`, `--network=host`, `-v /dev/hugepages:/dev/hugepages`
- [ ] Build with `-DCMAKE_CUDA_ARCHITECTURES=120`
- [ ] Run benchmarks as **root** inside container
- [ ] Discovery or manual `ETH_DST_ADDR`, TX/RX BDFs, GPU indices
- [ ] For wire modes: L2 link up (`carrier=1` on both ports is necessary, not sufficient — check phy counters after run)
- [ ] Hugepages sized per DAQIRI preflight

## Config ladder

| Step | Config | Notes |
|------|--------|-------|
| **Wire DPDK** | `daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml` | Prefilled dual-port example; override via discovery/CLI |
| Generic wire | `daqiri_bench_raw_tx_rx_rtx_pro_6000.yaml` | Placeholder BDFs/MACs for custom topology |
| HDS wire | `daqiri_bench_raw_tx_rx_hds_rtx_pro_6000.yaml` | CPU header + GPU payload |
| RoCE wire | `daqiri_bench_rdma_tx_rx_rtx_pro_6000.yaml` | `kind: device` |
| ibverbs RX | `daqiri_bench_raw_rx_ibverbs_rtx_pro_6000.yaml` | `engine: ibverbs`; paired DPDK TX |
| Build sanity | `daqiri_bench_raw_sw_loopback_rtx_pro_6000.yaml` | `loopback: sw`, no NIC |

**Never edit tracked YAMLs during sweeps.** Runner writes temp configs under `bench-results/<timestamp>-rtx-pro-<backend>-<mode>/`.

## Runner

```bash
./examples/run_rtx_pro_bench.sh <backend> <mode> [options]

# backends: dpdk, dpdk-hds, rdma/roce, ibverbs, socket-udp, socket-tcp
# wire modes: nic-smoke, issue17, sweep, drop-curve, drop-curve-matrix
# optional: sw-smoke (build sanity, dpdk only)
# build-only

# options: --seconds, --workload, --tx-bdf, --rx-bdf, --rx-mac, --tx-gpu, --rx-gpu
```

Environment: `ETH_DST_ADDR`, `RTX_TX_BDF`, `RTX_RX_BDF`, `WORKLOAD`, `DAQIRI_BUILD_DIR`.

## Pass / fail

**Wire:** RX > 0, phy counters rise, no buffer-pool errors, drops stable.

**SW loopback:** completes with TX/RX Gbps; labelled non-wire in `summary.csv`. No phy-counter checks.

## RTX vs Spark

| | Spark (GB10) | RTX PRO 6000 class |
|---|---|---|
| CUDA arch | 121 | **120** |
| Buffer kind | `host_pinned` | **`device`** |
| Wire loopback | eswitch on some topologies | **dual-port L2 loop** |
| SW loopback | wire-ish | **not wire** |

See [reference.md](reference.md) and [examples/rtx_pro_6000_baseline.md](../../../examples/rtx_pro_6000_baseline.md).
