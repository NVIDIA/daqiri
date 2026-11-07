# Tegra MGBE userspace driver

DAQIRI's optional `mgbe` engine exposes the custom Tegra MGBE DMA polling
driver through the normal raw-Ethernet packet API. Its kernel control shim
retains NVIDIA's hardware setup, allocates coherent host buffers and gives one
application exclusive access to one RX/TX queue pair. The userspace path polls
descriptors and returns buffers explicitly; it does not use DPDK or AF_XDP.

The initial implementation targets the pinned IGX Thor kernel/driver package
and requires 4-KiB system pages. It supports CPU host buffers, direct RX and
single-packet direct TX on one application thread. It rejects GPU memory,
offloads, timestamps, flow rules, reorder and indirect polling. The ordinary
`ibverbs`/`dpdk` raw defaults remain unchanged.

Build with `-DDAQIRI_ENGINE="mgbe"`, or add `mgbe` to an existing engine list.
Select it explicitly in YAML:

```yaml
daqiri:
  cfg:
    version: 1
    stream_type: raw
    engine: mgbe
    master_core: 0
    memory_regions:
      - {name: RX, kind: host, affinity: 0, num_bufs: 1024, buf_size: 1514}
      - {name: TX, kind: host, affinity: 0, num_bufs: 1024, buf_size: 1514}
    interfaces:
      - name: mgbe
        address: mgbe0_0
        rx:
          queues:
            - {name: RX, id: 0, poll_mode: direct, memory_regions: [RX]}
        tx:
          queues:
            - {name: TX, id: 0, poll_mode: direct, memory_regions: [TX]}
```

Both host regions describe the kernel-owned DMA pools. Their slot counts and
frame capacities must match; slot counts must be powers of two from 64 through
4096. `buf_size` is the maximum L2 frame excluding FCS; the IP MTU is fourteen
bytes smaller. External memory bindings are unsupported.

Before runtime, load the matching shim in exclusive-port mode and quiesce the
kernel netdev. Replacing `nvethernet` affects all its MGBE ports; the supplied
loader checks ownership and preserves a stock rollback path. Applications
must release outstanding bursts before shutdown. Link loss invalidates the
session and requires reopening it.

See the [driver source and complete build/install/rollback instructions](https://github.com/NVIDIA/daqiri/tree/main/drivers/mgbe)
for the pinned vendor source, headers, container commands, installed artifacts
and cabled echo smoke test. The source-tree instructions live at
`drivers/mgbe/README.md` and the complete sample is
`drivers/mgbe/config/daqiri_mgbe.yaml`.
