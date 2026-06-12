# RTX PRO 6000 benchmark baseline

Host: x86_64 EPYC · GPU: NVIDIA RTX PRO 6000 Blackwell Server Edition · `CMAKE_CUDA_ARCHITECTURES=120` · branch `ccrozier-rtx-pro-6000-bench` · 2026-06-12

## Hardware limitations (read first)

| What | Limit |
|---|---|
| **Cliff 800 Gbps target** | Two ~400G ports with an **active L2 link** (QSFP cable, loopback optic, or switch). Not achievable without that link. |
| **This server vs DGX Spark** | One PF per physical port. **No** Spark-style on-chip eswitch loopback (two PFs on the same port). |
| **`loopback: "sw"`** | Does **not** use the NIC. Measures in-process DPDK path only; can exceed line rate (not comparable to Gbps on the wire). |
| **`carrier=1`** | Link up on a port ≠ p0 and p1 are cabled **to each other**. Our dual-port run proved this: TX on p0, RX 0 on p1. |
| **Starting point** | SW smoke test validates GPUDirect build. Real NIC numbers need a completed L2 loop; use `tx_phy_packets` / `rx_phy_packets` to confirm wire vs internal. |

## Results (this dev box)

| Config | GPU (CUDA) | Mode | Duration | TX Gbps | RX Gbps | Notes |
|---|---|---|---|---|---|---|
| `daqiri_bench_raw_sw_loopback_rtx_pro_6000.yaml` | 0 | `loopback: sw` | 30s | 1579.5 | 1579.5 | No NIC; GPUDirect smoke baseline |
| `daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml` | 0 TX / 1 RX | real NIC `61:00.0`→`61:00.1` | 30s | 381.5 | 0 | NIC TX path works; **no RX** — p0/p1 not looped (phy_pkts ≪ vport) |
| `daqiri_bench_raw_tx_rx_rtx_pro_6000_nic_same_port.yaml` | 0 | same PF `61:00.0` | — | — | — | `daqiri_init` failed (extmem pool on single port); not a baseline |
| `daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml` | CPU huge | `loopback: sw` + reorder | 30s | — | 160.2 | GPU reorder kernel path; CPU buffers not GPUDirect |
| `daqiri_bench_socket_udp_tx_rx.yaml` (`iterations: 0`) | host | kernel UDP `127.0.0.1` | 15s | 4.7 | 4.7 | Kernel socket baseline; stock YAML uses `iterations: 1000` (not a perf test) |

### NIC dual-port run detail

- Card: `0000:61:00.0` (ens1f0np0, p0) TX → `0000:61:00.1` (ens1f1np1, p1) RX
- `eth_dst_addr`: `c4:70:bd:c2:8a:93` (p1 MAC)
- DPDK `tx_phy_packets` / `rx_phy_packets` on port 0: **2** / **76** vs **177M** TX vport packets → traffic did not cross SerDes between ports

## Commands

```bash
# SW smoke (no NIC)
sudo ./build/examples/daqiri_bench_raw_gpudirect \
  ./build/examples/daqiri_bench_raw_sw_loopback_rtx_pro_6000.yaml --seconds 30

# Real NIC dual-port (needs p0↔p1 L2 loop)
sudo ./build/examples/daqiri_bench_raw_gpudirect \
  ./build/examples/daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml --seconds 30
```

## What we can still run now (#17, partial)

| Test | Status | Notes |
|---|---|---|
| Raw SW loopback GPUDirect | Done | Best no-cable perf number |
| Raw NIC TX (one port) | Done | Proves mlx5 + GPUDirect TX |
| Reorder SW loopback | Done | ~160 Gbps RX, CPU-side buffers |
| Socket UDP (`iterations: 0`) | Done | ~5 Gbps kernel baseline |
| Socket TCP (`iterations: 0`) | Easy | Same yaml tweak |
| HDS / RoCE / NIC closed-loop | Blocked | Need filled YAML + L2 loop |
| FFT / GEMM workloads | Not in repo | #17 asks for these |

## Follow-ups

- Install QSFP cable (or passive loopback) between p0 and p1 on `61:00.x`, re-run `_nic.yaml`
- Scale to cross-card 800 Gbps using `daqiri_bench_raw_tx_rx_rtx_pro_6000.yaml` template + second card
- Tune CPU cores from `nvidia-smi topo -m` once RX path is up
- Add RTX socket YAML with `iterations: 0` for repeatable kernel baseline
