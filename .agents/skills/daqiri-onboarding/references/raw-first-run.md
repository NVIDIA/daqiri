# Raw Ethernet First Run

Use this path for raw Ethernet, DPDK, ibverbs raw, GPUDirect, ConnectX, cabled loopback, hardware loopback, hardware reorder, packet pacing, and raw flow steering.

## Path Selection

Prefer in this order:

1. Cabled raw TX/RX loopback or cross-host run when a cable/peer exists.
2. Single-port hardware loopback when a ConnectX NIC exists but no cable/peer is available.
3. Software loopback only when no usable NIC path exists or the user asks for a hardware-free smoke test.

Use `daqiri_bench_raw_gpudirect` for the normal raw first run. Use `daqiri_bench_raw_latency` only when latency sweep is the stated goal. Use reorder or named-endpoint examples only after basic TX/RX is working.

## Prerequisite Checks

Ask for or run:

```bash
ibdev2netdev -v
lspci -nn | grep -Ei 'mellanox|nvidia|network|ethernet'
nvidia-smi --query-gpu=index,name,uuid --format=csv
grep Huge /proc/meminfo
ip -br link
```

If hugepages are short, DAQIRI preflight usually prints the exact `echo N | sudo tee ...` command. Prefer running once and using that recommendation instead of guessing.

For IGX Thor or hybrid iGPU/dGPU hosts, select the discrete GPU by UUID in both `NVIDIA_VISIBLE_DEVICES` and `CUDA_VISIBLE_DEVICES`. Inside the container, that GPU becomes CUDA ordinal `0`; configs should use `memory_regions[*].affinity: 0`.

For DGX Spark / GB10, use `kind: "host_pinned"` for GPU-accessible packet buffers (or `--memory-kind host_pinned` when generating configs). GB10 uses unified CPU/GPU physical memory with no separate GPU VRAM. The NIC and GPU access the same buffers without a host-to-device staging copy; peermem and CUDA device-memory DMA-BUF registration do not apply. Explain this as the expected platform path, rather than reporting that "device-memory GPUDirect is blocked." A successful `host_pinned` run validates this shared-memory path, not discrete-GPU device-memory GPUDirect RDMA. See `docs/concepts.md` and the DGX Spark profile in `docs/tutorials/system_configuration.md`.

## Build and Container

Recommended build:

```bash
BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
```

Recommended run shell:

```bash
docker run --rm -it --privileged \
  --runtime=nvidia \
  --network=host \
  -v /dev/hugepages:/dev/hugepages \
  daqiri:local bash
```

## Cabled Loopback

Start from `daqiri_bench_raw_tx_rx.yaml` or generate a raw pair. Replace at minimum:

- TX and RX PCIe interface addresses.
- Destination MAC address.
- IP addresses/ports if used by the selected flow.
- GPU/CPU affinity and CPU core placement.
- Memory kind and size when adapting to host capabilities.

Run RX/server side before TX/client side for split-host or split-role tests.

```bash
/opt/daqiri/bin/daqiri_bench_raw_gpudirect \
  /opt/daqiri/bin/daqiri_bench_raw_tx_rx.yaml \
  --seconds 10
```

For source builds:

```bash
./build/examples/daqiri_bench_raw_gpudirect \
  ./examples/daqiri_bench_raw_tx_rx.yaml \
  --seconds 10
```

## Cross-Host Spark Shape

When two DGX Spark systems are cross-cabled, split the TX/RX host routes and run setup on both hosts:

```bash
sudo scripts/setup_spark_xhost_net.sh --role tx
sudo scripts/setup_spark_xhost_net.sh --role rx
ping -c 3 <peer-ip>
ip route get <peer-ip>
```

Use generated TX/RX YAML files when possible. Verify the route names the cabled netdev, not `lo`.

## Hardware Loopback

Use this when one ConnectX NIC is available but no external cable/peer is available. Label results as hardware-loopback throughput. Measure `vport_loopback_bytes`; do not add TX and RX rates for the same returned traffic. Physical `*_bytes_phy` and `*_packets_phy` counters should remain flat because no packet crosses the SerDes.

## Measuring Throughput

For trusted raw throughput, run at least 10 seconds and discard startup/shutdown samples.

```bash
mlnx_perf -i <netdev> -t 1
```

- Cabled run: compare sender TX PHY and receiver RX PHY counters.
- Hardware loopback: report stable `vport_loopback_bytes`.
- Always include DAQIRI per-queue packet/drop/no-buffer stats.
- Report application payload throughput separately from physical wire rate.

## Triage

- Hugepage failure: use DAQIRI preflight recommendation or inspect `/proc/meminfo`.
- Zero packets: check MAC, PCIe address, cable/peer, flow steering, RX started before TX, and whether the selected config is RX-only.
- Drops or `NO_FREE_BURST_BUFFERS` / `NO_FREE_PACKET_BUFFERS`: verify the application frees bursts and increase buffers only after understanding ownership.
- No PHY counter movement on cabled tests: route may be loopback/vport-only, wrong port pair, or not physically cabled.
- eCPRI raw DPDK flows: firmware steering may be required; HW steering can install but not match on ConnectX-class NICs.
- Hardware reorder capability failure: verify programmable flex parsing settings and cold reboot.
