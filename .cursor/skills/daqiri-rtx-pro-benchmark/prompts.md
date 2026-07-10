# Agent prompts (RTX PRO 6000 benchmarking)

Read `.cursor/skills/daqiri-rtx-pro-benchmark/SKILL.md` first.

**Wire closed-loop** (dual-port L2 loop, real NIC traffic) is the primary benchmark class on any host. **SW loopback** is optional build sanity when no link is available.

---

## Prompt A — Wire smoke + issue #17

1. **Preflight:** container, hugepages, GPUs; `source ./scripts/discover_rtx_pro_topology.sh` (or set `ETH_DST_ADDR` / BDFs manually); confirm L2 link if running wire modes.
2. **Build:** `./examples/run_rtx_pro_bench.sh dpdk build-only` with arch 120 if needed.
3. **Wire smoke:** `sudo ./examples/run_rtx_pro_bench.sh dpdk nic-smoke --seconds 30` — RX Gbps > 0, rising phy counters.
4. **Issue #17:** `dpdk`, `dpdk-hds`, `rdma`, `ibverbs` with `issue17` mode.
5. Record wire results in `examples/rtx_pro_6000_baseline.md` (host-specific section).

Stop sweeps if wire RX is zero.

---

## Prompt B — Build sanity only (no link)

When no L2 loop is available:

```bash
sudo ./examples/run_rtx_pro_bench.sh dpdk sw-smoke --seconds 10
```

Non-wire only. Run Prompt A when a link exists.

---

## Prompt C — Plot sweep CSV

```bash
python3 scripts/plot_rtx_pro_bench.py --csv bench-results/<run>/runs.csv --x payload --y gbps --series batch
```

Label SW vs wire explicitly when comparing (`--compare sw:<csv> nic:<csv>`).
