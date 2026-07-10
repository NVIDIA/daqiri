# RTX PRO 6000 benchmark baseline

Reference measurements and commands for **discrete Blackwell dGPU** hosts (`CMAKE_CUDA_ARCHITECTURES=120`, `kind: device` buffers). The benchmark *patterns* below apply to any host with a suitable NIC topology; prefilled YAML values are examples — override BDFs, MACs, and GPUs via discovery or runner flags.

## Two benchmark classes

| Class | Config pattern | Uses NIC / SerDes | Meaningful for line rate |
|---|---|---|---|
| **Wire closed-loop** | `daqiri_bench_raw_tx_rx*_nic.yaml`, HDS/RoCE/ibverbs wire YAMLs | Yes | **Yes** — primary path |
| **SW loopback** | `daqiri_bench_raw_sw_loopback*.yaml`, `loopback: sw` | No | No — build sanity only |

Wire closed-loop needs **L2 connectivity** between the TX port and RX port: QSFP/DAC between two ports on one card, a loopback optic, or a switch. One process sends on port A and receives on port B; `tx_phy_packets` / `rx_phy_packets` confirm packets crossed the link. This is the same *class* of test as Spark's p0→p1 wire loopback, but on discrete-GPU servers there is typically **one PF per port** (no on-chip eswitch shortcut).

SW loopback (`loopback: sw`) stays in the tree for compile/GPUDirect sanity when no link is available. Throughput is not comparable to wire Gbps.

## Topology (fill per host)

| Field | How to set |
|---|---|
| TX BDF / RX BDF | `scripts/discover_rtx_pro_topology.sh`, or `--tx-bdf` / `--rx-bdf` |
| `eth_dst_addr` | RX port MAC (`ETH_DST_ADDR` from discovery) |
| GPU affinity | `--tx-gpu` / `--rx-gpu`, or edit YAML `memory_regions[].affinity` |
| CPU cores | YAML `cpu_core` / queue cores — tune from `nvidia-smi topo -m` |

Example values from one reference box (replace on other hosts):

| Role | PCIe BDF | Iface (example) | CUDA GPU |
|------|----------|-----------------|----------|
| TX | `0000:61:00.0` | `ens1f0np0` | 0 |
| RX | `0000:61:00.1` | `ens1f1np1` | 1 |

Fully generic template (angle-bracket placeholders): `daqiri_bench_raw_tx_rx_rtx_pro_6000.yaml`. Prefilled dual-port example: `daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml`.

## Benchmark ladder

```
build-only
optional: dpdk sw-smoke              # no NIC; non-wire

wire (primary — any host with L2 loop):
  ├── dpdk nic-smoke                 # GPUDirect closed-loop
  ├── dpdk issue17                   # none / fft / gemm
  ├── dpdk-hds issue17               # HDS + workloads
  ├── rdma issue17                   # RoCE + workloads
  ├── ibverbs issue17                # ibverbs RX + DPDK TX
  └── dpdk sweep / drop-curve        # after nic-smoke passes
```

## Commands

```bash
# Build
./examples/run_rtx_pro_bench.sh dpdk build-only \
  --build-targets raw_gpudirect,raw_hds,rdma,socket

# Discover (host-side, before DPDK bind) — or set ETH_DST_ADDR / BDFs manually
source ./scripts/discover_rtx_pro_topology.sh

# Optional build sanity (no link required)
sudo ./examples/run_rtx_pro_bench.sh dpdk sw-smoke --seconds 10

# Wire ladder (run as root)
sudo ./examples/run_rtx_pro_bench.sh dpdk nic-smoke --seconds 30
sudo ./examples/run_rtx_pro_bench.sh dpdk issue17 --seconds 30
sudo ./examples/run_rtx_pro_bench.sh dpdk-hds issue17 --seconds 30
sudo ./examples/run_rtx_pro_bench.sh rdma issue17 --seconds 30
sudo ./examples/run_rtx_pro_bench.sh ibverbs issue17 --seconds 30

# Override topology without editing tracked YAML:
sudo ./examples/run_rtx_pro_bench.sh dpdk nic-smoke \
  --tx-bdf 0000:61:00.0 --rx-bdf 0000:61:00.1 --rx-mac aa:bb:cc:dd:ee:ff
```

## Issue #17 matrix (wire)

| Backend | Config | Workloads | Binary |
|---|---|---|---|
| DPDK GPUDirect | `daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml` | `none`, `fft`, `gemm` | `daqiri_bench_raw_gpudirect` |
| DPDK HDS | `daqiri_bench_raw_tx_rx_hds_rtx_pro_6000.yaml` | `none`, `fft`, `gemm` | `daqiri_bench_raw_hds` |
| RoCE | `daqiri_bench_rdma_tx_rx_rtx_pro_6000.yaml` | `none`, `fft`, `gemm` | `daqiri_bench_rdma` |
| ibverbs | `daqiri_bench_raw_rx_ibverbs_rtx_pro_6000.yaml` | `none`, `fft`, `gemm` | `daqiri_bench_raw_gpudirect` + DPDK TX |

Runner writes `runs.csv` and `summary.csv` (total data rate, sent, received, dropped).

### Pass / fail (wire)

- RX packets and RX Gbps > 0
- `tx_phy_packets` / `rx_phy_packets` rise with traffic
- No `NO_FREE_BURST_BUFFERS` / `NO_FREE_PACKET_BUFFERS`
- DPDK drops not exploding

## RTX vs Spark (do not copy Spark settings)

| | Spark (GB10) | RTX PRO 6000 class |
|---|---|---|
| CUDA arch | `121` | **`120`** |
| Buffer kind | `host_pinned` | **`device`** |
| Wire loopback | on-chip eswitch possible | **dual-port L2 loop** (cable/optic/switch) |
| SW loopback | ~94 Gbps, wire-ish | ~1580 Gbps, **not wire** |

## Reference box measurements

Host: x86_64 EPYC · RTX PRO 6000 Blackwell · branch `ccrozier-rtx-pro-6000-bench` · 2026-07-10

| Config | Mode | TX Gbps | RX Gbps | Notes |
|---|---|---|---|---|
| `daqiri_bench_raw_sw_loopback_rtx_pro_6000.yaml` | SW loopback | 1579.5 | 1579.5 | Non-wire; build sanity |
| `daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml` | Wire | 381.5 | 0 | Pre-link-fix run; flat `rx_phy_packets` |
| HDS / RoCE wire | — | — | — | Record after wire path passes |

Re-run wire configs after discovery on any host and append rows here.
