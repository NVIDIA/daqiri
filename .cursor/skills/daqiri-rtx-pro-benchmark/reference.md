# RTX PRO 6000 benchmark reference

## Wire vs SW loopback

| | Wire closed-loop | SW loopback (`loopback: sw`) |
|---|---|---|
| NIC / SerDes | Yes | No |
| Config | `*_nic.yaml`, HDS/RoCE/ibverbs wire YAMLs | `*_sw_loopback*.yaml` |
| Line-rate meaningful | Yes | No |
| When to use | Primary benchmarking | Build sanity, no link available |

Wire closed-loop requires **L2 connectivity** between the configured TX port and RX port (direct cable, loopback optic, or switch). After a run, `tx_phy_packets` / `rx_phy_packets` in DPDK extended stats must rise with traffic — that is the wire proof. `carrier=1` alone does not prove the two ports talk to each other.

## Topology (per host)

Set before each session (before DPDK bind):

```bash
./scripts/discover_rtx_pro_topology.sh
# exports ETH_DST_ADDR, RTX_TX_BDF, RTX_RX_BDF (example defaults for one dev box)
```

Or pass overrides to the runner: `--tx-bdf`, `--rx-bdf`, `--rx-mac`, `--tx-gpu`, `--rx-gpu`.

Generic placeholder template: `examples/daqiri_bench_raw_tx_rx_rtx_pro_6000.yaml`.  
Prefilled example (edit or override): `examples/daqiri_bench_raw_tx_rx_rtx_pro_6000_nic.yaml`.

## Host-side checks (before DPDK bind)

```bash
lspci -d 15b3:
ibdev2netdev
cat /sys/class/net/<tx_iface>/carrier /sys/class/net/<rx_iface>/carrier
cat /sys/class/net/<rx_iface>/address    # ETH_DST_ADDR
nvidia-smi topo -m
grep Huge /proc/meminfo
```

## Benchmark order

1. `build-only` — arch 120
2. `dpdk nic-smoke` — wire sanity (skip if no L2 loop yet; use `sw-smoke` for build check only)
3. `issue17` — wire workloads on dpdk / dpdk-hds / rdma / ibverbs
4. `sweep` / `drop-curve` — after wire nic-smoke passes

## Failure triage

| Symptom | Likely cause |
|---------|----------------|
| RX Gbps = 0, TX > 0 | No L2 loop; wrong `eth_dst_addr`; flow mismatch |
| Flat `rx_phy_packets` | Traffic not on wire |
| SW OK, wire fails | Topology/MAC, not build |
| `NO_FREE_*` | RX path or rate; burst not freed |

Avoid `daqiri_bench_raw_tx_rx_rtx_pro_6000_nic_same_port.yaml` (same-port TX+RX failed init on reference box).

## Alignment with Spark / IGX methodology

| Principle | Spark (`run_spark_bench.sh`) | IGX (docs) | RTX runner |
|---|---|---|---|
| Primary path | Dual-port wire closed-loop (`daqiri_bench_raw_tx_rx_spark.yaml`) | Wire saturation at line rate | Same class: `*_nic.yaml` + L2 loop |
| Native smoke shape | 8000 B payload, 10240 batch, unpaced | — | Same headline matrix |
| MAC discovery | `ETH_DST_ADDR` placeholder in YAML | — | Same (`<00:00:00:00:00:00>`) |
| Flow steering | `flow_isolation` + UDP 4096 match | — | Same on wire YAMLs |
| App vs queue cores | Separate `bench_tx`/`bench_rx` cpu_core | — | Required on wire YAMLs |
| Wire proof | phy counters documented | `rx_packets_phy` ≈ vport | `wire_phy_ok` fail-fast |
| Drops | imissed/ierrors/rx_nombuf | — | Same parsers |
| SW loopback | Separate YAML, not in runner smoke | — | Optional `sw-smoke` only |
| Sweeps | payload × batch × target_gbps | — | Same modes |

RTX-specific (not on Spark): `kind: device`, arch 120, issue #17 workloads, HDS/ibverbs backends.

## Baseline doc

Host-specific numbers: [examples/rtx_pro_6000_baseline.md](../../../examples/rtx_pro_6000_baseline.md)
