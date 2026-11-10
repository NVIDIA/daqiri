# Tegra MGBE userspace driver

DAQIRI's optional `mgbe` engine exposes the custom Tegra MGBE DMA polling
driver through the normal raw-Ethernet packet API. Its kernel control shim
retains NVIDIA's hardware setup, allocates coherent host buffers and gives one
application exclusive access to one RX/TX queue pair. The userspace path polls
descriptors and returns buffers explicitly; it does not use DPDK or AF_XDP.

The initial implementation targets the pinned IGX Thor kernel/driver package
and requires 4-KiB system pages. It supports CPU host buffers with direct or
indirect polling, including mixed modes. Direct TX sends one packet per call;
indirect TX sends bursts through pinned workers. It rejects GPU memory,
offloads, timestamps, flow rules and reorder. The ordinary
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

For indirect operation, set `poll_mode: indirect`, `cpu_core` and `batch_size`
(1..64) on each indirect queue. On RX, `timeout_us` publishes a partial batch
after the first packet has waited that long; zero waits for a full batch.
For example, replace the queue entries above with:

```yaml
rx:
  queues:
    - {name: RX, id: 0, poll_mode: indirect, cpu_core: 8, batch_size: 32,
       timeout_us: 100, memory_regions: [RX]}
tx:
  queues:
    - {name: TX, id: 0, poll_mode: indirect, cpu_core: 8, batch_size: 32,
       memory_regions: [TX]}
```

Choose worker CPUs available on your host and run application work on separate
cores. RX and TX on the same core share one poller; different cores use separate
pollers. Indirect mode passes mapped DMA buffer ownership between worker and
application without copying packet payloads. A successful send transfers burst
ownership to the engine, and TX buffers are reused only after completion.
Application threads may own different bursts concurrently; each burst must have
one owner at a time. Stop application threads before shutdown.

Both modes still expose **one hardware RX/TX queue pair, ID 0**, with no RSS.
The complete indirect sample is `drivers/mgbe/config/daqiri_mgbe_indirect.yaml`.

Before runtime, load the matching shim in exclusive-port mode and quiesce the
kernel netdev. Replacing `nvethernet` affects all its MGBE ports; the supplied
loader checks ownership and preserves a stock rollback path. Applications
must release outstanding bursts before shutdown. Link loss invalidates the
session and requires reopening it.

The experimental driver has an unresolved intermittent first-frame RX visibility
issue observed on isolated cores. Validate cold startup as well as sustained
traffic for the deployment.

See the [driver source and complete build/install/rollback instructions](https://github.com/NVIDIA/daqiri/tree/main/drivers/mgbe)
for the pinned vendor source, headers, container commands, installed artifacts
and cabled echo smoke test. The source-tree instructions live at
`drivers/mgbe/README.md` and the complete sample is
`drivers/mgbe/config/daqiri_mgbe.yaml`.
