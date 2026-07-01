---
hide:
  - navigation
---

# Configuration YAML Reference

DAQIRI is configured through a YAML file or a `NetworkConfig` struct built in code.
Either form defines memory regions, NIC interfaces, TX/RX queues, and flow rules, and
is passed to `daqiri_init()` at startup. The struct form is useful for customers who
want to interoperate with existing configuration code.

See `examples/daqiri_bench_*.yaml` for complete working examples.

!!! tip "Where to start"
    This is a long reference. If you are new, start from a working `examples/daqiri_bench_*.yaml` and the [Configuration YAML Walkthrough](../tutorials/configuration-walkthrough.md), then use the sections below to look up individual fields:

    - **Common Configuration** — `version`, `master_core`, `stream_type`, `engine`, `log_level` (apply to every config).
    - **Memory regions** — host, hugepage, and GPU buffer pools.
    - **RX / TX queues and flows** — queue setup, flow steering, and per-transport options.
    - **Socket / RoCE** — `socket_config` endpoints (`udp://`, `tcp://`, `roce://`).

OpenTelemetry metrics do not add YAML fields. Metrics-enabled builds use the
same interface, queue, and flow names from the active configuration as metric
labels, and applications are still responsible for configuring the OpenTelemetry
SDK/exporter before running DAQIRI.

## Common Configuration

These settings apply globally to both TX and RX:

- **`version`**: Config version. Only `1` is valid currently.
  - type: `integer`
- **`master_core`**: CPU core used to fork and join network threads. This core is not used
  for packet processing and can be bound to a non-isolated core. Should differ from isolated
  cores assigned to queues.
  - type: `integer`
- **`stream_type`**: Packet I/O stream class.
  - type: `string`
  - values: `raw`, `socket`
- **`engine`**: Optional implementation engine for the selected stream type. Omit this
  unless you need a specific implementation override. For `stream_type: "raw"` the
  default is `dpdk`; set `engine: "ibverbs"` to use the Multi-Packet (striding) Receive
  Queue engine on Mellanox/mlx5 NICs instead. RoCE configs infer `ibverbs` from
  `roce://` endpoint URIs by default.
  - type: `string`
  - values: `dpdk`, `socket`, `ibverbs`
- **`log_level`**: Engine log level.
  - type: `string`
  - values: `trace`, `debug`, `info`, `warn` (default), `error`, `critical`, `off`
- **`loopback`**: Enable software loopback for testing without a physical link.
  - type: `string`
  - values: `""` (disabled, default), `"sw"` (software loopback)
- **`tx_meta_buffers`**: Metadata buffers for transmit. One buffer is used for each burst
  of packets.
  - type: `integer`
  - default: `256`
- **`rx_meta_buffers`**: Metadata buffers for receive. One buffer is used for each burst
  of packets.
  - type: `integer`
  - default: `256`

## Memory Regions

`memory_regions:` — List of regions where packet buffers are stored. The number of regions
and their `kind` determines the receive mode (CPU-only, header-data split, or batched GPU).

- **`name`**: Memory region name. Referenced by queue configurations.
  - type: `string`
- **`kind`**: Memory type.
  - type: `string`
  - values:
    - `huge` — CPU hugepages (recommended for CPU buffers)
    - `device` — GPU VRAM (discrete GPUs only; requires GPUDirect via peermem or DMA-BUF)
    - `host_pinned` — Pinned CPU pages allocated via `cudaHostAlloc`. **Recommended on
      integrated GPUs (e.g. NVIDIA GB10 / DGX Spark)**, where the NIC cannot peer-DMA
      into device memory and CUDA reports DMA-BUF unsupported. On discrete-GPU systems,
      prefer `device` for high-throughput RX/TX paths.
    - `host` — Regular CPU memory (not recommended)
- **`affinity`**: GPU ID for `device` memory, or NUMA node ID for CPU memory.
  - type: `integer`
- **`access`**: Memory access permissions.
  - type: `list`
  - values: `local`, `rdma_read`, `rdma_write`
- **`num_bufs`**: Number of buffers in this region. Higher values give more processing
  headroom but consume more memory (GPU BAR1 for `device`). Too low risks dropped packets
  on RX or higher latency on TX. Rule of thumb: 3x-5x `batch_size`. For Raw Ethernet
  (`stream_type: "raw"`), `num_bufs` below 1.5x the NIC ring size deadlocks the worker;
  `daqiri_init` auto-bumps such MRs to 3x the ring (24576 with the default 8192) and
  logs a `WARN`.
  - type: `integer`
- **`buf_size`**: Size of each buffer in bytes. Should match the expected packet size, or
  the segment size when using header-data split.
  - type: `integer`

### Example: Header-Data Split

Two regions — a small CPU region for headers and a GPU region for payload:

```yaml
memory_regions:
- name: "RX_CPU"
  kind: "huge"
  affinity: 0
  access:
    - local
  num_bufs: 51200
  buf_size: 64        # ETH+IP+UDP headers (~42 bytes, padded)
- name: "RX_GPU"
  kind: "device"
  affinity: 0
  access:
    - local
  num_bufs: 51200
  buf_size: 1000      # payload
```

## Interfaces

`interfaces:` — List of NIC interfaces to configure.

- **`name`**: Interface name. Used to look up port IDs at runtime via `get_port_id()`.
  - type: `string`
- **`address`**: PCIe BDF address (from `lspci`) or Linux interface name for Raw Ethernet
  (`stream_type: "raw"`), or IP address for RoCE (`stream_type: "socket"` with a
  `roce://` endpoint).
  - type: `string`

### Socket and RDMA Endpoint Configuration

Socket-style streams use a `socket_config` block for endpoint role and addressing.
Endpoint addresses are URI strings. Supported schemes are `tcp://`, `udp://`, and
`roce://` (`rdma://` is still accepted as a legacy alias).

- **`socket_config.mode`**: Connection role.
  - type: `string`
  - values: `client`, `server`
- **`socket_config.local_addr`**: Local bind endpoint, for example
  `tcp://127.0.0.1:6001`, `roce://10.100.3.1:4096`, or
  `roce://10.100.1.1` for a RoCE client whose source port is chosen by RDMA CM.
  Required for server mode and RoCE client mode.
- **`socket_config.remote_addr`**: Remote peer endpoint, for example
  `udp://10.250.0.2:5021`. Required for TCP/UDP client mode. RoCE clients choose
  the peer in application code (for example by calling `rdma_connect_to_server`),
  not in DAQIRI config.
- **`socket_config.local_ip`** / **`socket_config.local_port`** and
  **`socket_config.remote_ip`** / **`socket_config.remote_port`**: Legacy endpoint
  fields accepted for older configs when a top-level engine override provides the
  transport.

When using RoCE, set `stream_type: "socket"` and use `roce://` endpoint addresses
plus a `roce_config` block for transport settings. A RoCE URI may include
`?engine=ibverbs`; when omitted, `ibverbs` is the default and only supported RoCE
engine.

- **`roce_config.transport_mode`**: RDMA transport type.
  - type: `string`
  - values: `RC` (Reliable Connected), `UC` (Unreliable Connected)

## Receive Configuration (rx)

### Queues

`rx.queues:` — List of receive queues on the interface.

- **`name`**: Queue name.
  - type: `string`
- **`id`**: Integer ID used for flow steering and burst retrieval.
  - type: `integer`
- **`cpu_core`**: CPU core ID for the RX worker thread. Should be an isolated core for best
  performance.
  - type: `string`
- **`batch_size`**: Number of packets per batch passed from the NIC to the application. Larger
  values increase throughput; smaller values reduce latency.
  - type: `integer`
- **`memory_regions`**: List of memory region names (defined in [Memory Regions](#memory-regions)).
  The order determines segment mapping: first region = segment 0, second = segment 1, etc.
  A single region means all packet data lands in one place; two regions enables header-data
  split.
  - type: `list`
- **`timeout_us`**: Timeout in microseconds. A partial batch is delivered if this time elapses
  before `batch_size` packets are collected. Set to `0` to disable (wait for full batch only).
  - type: `integer`
  - default: `0`

### Flex Items

`rx.flex_items:` — Flexible parser items for custom flow matching beyond standard UDP fields.

- **`name`**: Name of the flex item.
  - type: `string`
- **`id`**: ID of the flex item. Scoped per interface; the same numeric ID on two
  interfaces may refer to different parser settings.
  - type: `integer`
- **`offset`**: Byte offset after the UDP header where matching begins. Must be a multiple
  of 4 and less than 28.
  - type: `integer`
- **`udp_dst_port`**: UDP destination port for flex item activation.
  - type: `integer`

### Flows

`rx.flows:` — Static startup flow rules that steer packets to specific queues based on
match criteria. This sequence may be omitted; a queues-only RX config can add DPDK RX
flows later with the dynamic flow API. For Raw Ethernet on the DPDK and ibverbs engines,
RX flows can also perform hardware VLAN pop or tunnel decapsulation before queue delivery.

- **`name`**: Flow name.
  - type: `string`
- **`id`**: Flow ID. Retrievable at runtime via `get_packet_flow_id()`.
  - type: `integer`
- **`action`**: Legacy single action map. Existing configs may keep using
  `action: {type: queue, id: ...}`.
- **`actions`**: Ordered action list. Use this for tunnel/VLAN transforms.
  RX transform flows must end with `type: queue`.
  - **`type: queue`**: Steer matched packets to an RX queue.
    - **`id`**: Queue ID under `rx.queues` on the same interface.
  - **`type: vlan_pop`**: Pop one VLAN tag in hardware.
  - **`type: tunnel_decap`**: Decapsulate a hardware tunnel before queue delivery.
    - **`tunnel.type`**: `vxlan`, `gre`, or `nvgre`.
    - **`outer_eth_src` / `outer_eth_dst`**: Outer Ethernet addresses.
    - **`outer_ipv4_src` / `outer_ipv4_dst`**: Outer IPv4 addresses. IPv6 outer
      headers are not supported in v1.
    - **VXLAN fields**: `vni`, optional `outer_udp_src`, `outer_udp_dst` default `4789`.
    - **GRE fields**: optional `gre_protocol` default `0x0800`.
    - **NVGRE fields**: `tni`, optional `flow_id`.
- **`match`**: Criteria for matching packets.
  - **`udp_src`**: UDP source port or port range (e.g., `1000-1010`).
    - type: `integer` or `string`
  - **`udp_dst`**: UDP destination port or port range.
    - type: `integer` or `string`
  - **`ipv4_len`**: IPv4 payload length.
    - type: `integer`
  - **`flex_item_id`**: Flex item ID (the `id` field from an entry under `rx.flex_items` on
    the same interface). Cannot be combined with UDP/IP matching.
    - type: `integer`
  - **`val`**: 32-bit value to match (with flex items).
    - type: `integer`
  - **`mask`**: 32-bit mask applied before matching (with flex items).
    - type: `integer`

For Raw Ethernet (`stream_type: "raw"`), each flow rule is programmed into the NIC during
`daqiri_init()`. If any rule cannot be installed, or the send-to-kernel fallback cannot be
created when `flow_isolation: true`, initialization fails with a critical log and
`daqiri_init()` returns an error status.

A single RX interface must use either standard UDP/IP flows or flex-item flows, not both.
Both classes install conflicting DPDK group-0 jump rules, so only one is reachable when mixed.
`daqiri_init` rejects such configs with a clear error.
Flex-item flows cannot be combined with VLAN/tunnel transform actions in v1.

### Flow Isolation

`rx.flow_isolation:` — When `true`, only packets matching an explicit flow rule are delivered
to the application. Static startup flows install send-to-kernel fallback rules per flow class
(standard or flex-item), so unmatched traffic in those classes is steered back to the Linux
kernel. Queues-only configs can set `flow_isolation: true` and then install dynamic RX flows
after `daqiri_init()`; until a dynamic rule is added, application traffic is not delivered to
DAQIRI RX queues. When `false`, unmatched packets go to a default queue. Mixing standard and
flex-item flow classes on one interface is not supported.

- type: `boolean`
- default: `false`

### Dynamic Flow Capacity

`rx.dynamic_flow_capacity:` — DPDK template-table capacity reserved for dynamic RX flow
rules on this interface. `0` disables DPDK template/async setup on startup. Set a positive
value to opt in to the template fast path when it is available; legacy fallback paths still
accept dynamic RX flow operations but do not use a template table.

- type: `integer`
- default: `0`

### Hardware Timestamps

`rx.hardware_timestamps:` — Enable per-packet hardware RX timestamps for Raw Ethernet
(`stream_type: "raw"`).
When enabled, DAQIRI requires `RTE_ETH_RX_OFFLOAD_TIMESTAMP` support from the NIC/PMD and
initialization fails if DAQIRI cannot provide nanosecond timestamps for the selected PMD.
Timestamps are returned by `get_packet_rx_timestamp()` in nanoseconds in the NIC timestamp
clock domain, not wall-clock time.

- type: `boolean`
- default: `false`

### RX Reorder Configs

`rx.reorder_configs:` — Optional automatic packet reordering/aggregation plans. Implemented
for Raw Ethernet (`stream_type: "raw"`) only in v1. GPU reorder requires CUDA-addressable
packet buffers (`device` or `host_pinned` memory regions). CPU reorder requires CPU-addressable
packet buffers (`host`, `host_pinned`, or `huge` memory regions).

v1 source-memory requirement:
- Reorder queues must use exactly one RX source memory region.
- Header-data split RX queues are not supported with `rx.reorder_configs`.

v1 batch-size requirement:
- For each emitted reordered batch, packets are expected to have identical on-wire length.
- `payload_byte_offset` is applied uniformly to all packets in the batch, so mixed packet sizes
  in the same reorder batch are not supported.
- Timeout-flushed reordered bursts set `DAQIRI_BURST_FLAG_REORDER_TIMEOUT` in
  `burst->hdr.hdr.burst_flags`. All reordered bursts set `DAQIRI_BURST_FLAG_REORDERED`.
  `burst->hdr.hdr.max_pkt` contains the number of source packets represented by the aggregate.
- Reordered bursts expose `ReorderBurstInfo::batch_id` via
  `daqiri::get_reorder_burst_info(...)`. With `seq_batch_number`, the batch ID is copied from
  the configured batch-number field. With `seq_packets_per_batch`, the batch ID is derived from
  `sequence_number / packets_per_batch`.

- **`name`**: Reorder config name. Must be unique per interface.
  - type: `string`
- **`reorder_type`**: Reorder implementation (`gpu` or `cpu`).
  - type: `string`
  - values: `gpu`, `cpu`
- **`memory_region`**: Output memory region where reordered payload is written.
  - type: `string`
  - requirements: for `gpu`, must reference a `device` or `host_pinned` memory region; for
    `cpu`, must reference a `host`, `host_pinned`, or `huge` memory region
- **`payload_byte_offset`**: Byte offset in each packet where copied payload starts. Bytes before
  this offset are skipped.
  - type: `integer`
- **`data_types`**: Optional payload data type conversion for GPU reorder. If omitted, payload
  bytes are copied as-is.
  - `input_type`: On-wire input element type. Values: `int4`, `int8`, `int16`, `int32`
  - `output_type`: Reordered output element type. Values: `fp16`, `bf16`, `fp32`, `fp64`, `int32`
  - `endianness`: Optional input byte order. Values: `host`, `network`; default: `host`
  - requirements: conversion is supported for `reorder_type: "gpu"`; the output memory region
    buffer must hold the converted batch size
  - notes: `int4` is interpreted as two signed 4-bit two's-complement values per byte, high
    nibble first; `network` endianness swaps byte-multiple input types wider than 8 bits
- **`flow_ids`**: List of RX flow IDs this reorder config applies to.
  - type: `list[integer]`
  - notes: flow IDs cannot overlap across reorder configs on the same interface
- **`method`**: Exactly one method must be configured:
  - **`seq_batch_number`**
    - `sequence_number.bit_offset`
    - `sequence_number.bit_width` (1..32)
    - `batch_number.bit_offset`
    - `batch_number.bit_width` (1..32)
    - Derived constraint: `2^seq_bits` must be divisible by `2^batch_bits`
  - **`seq_packets_per_batch`**
    - `sequence_number.bit_offset`
    - `sequence_number.bit_width` (1..32)
    - `packets_per_batch` (>0)
    - Constraint: `2^seq_bits % packets_per_batch == 0`

Example conversion from packed signed 4-bit payload samples to FP16:

```yaml
data_types:
  input_type: "int4"
  output_type: "fp16"
  endianness: "host"
```

After `daqiri_init()`, each GPU reorder config must be assigned a CUDA stream. CPU reorder
configs do not use CUDA streams:

```cpp
daqiri::set_reorder_cuda_stream("rx_port", "rx_reorder_0", stream);
```

## Transmit Configuration (tx)

### Queues

`tx.queues:` — List of transmit queues on the interface.

- **`name`**: Queue name.
  - type: `string`
- **`id`**: Integer ID used for burst submission.
  - type: `integer`
- **`cpu_core`**: CPU core ID for the TX worker thread. Should be an isolated core for best
  performance.
  - type: `string`
- **`batch_size`**: Number of packets per batch sent to the NIC. Larger values increase
  throughput; smaller values reduce latency.
  - type: `integer`
- **`memory_regions`**: List of memory region names. Same segment mapping rules as RX.
  - type: `list`
- **`offloads`**: List of hardware offloads to enable. Each offload installs an RTE Flow rule
  during `daqiri_init()`; initialization fails if the NIC cannot program the rule.
  - type: `list`
  - values: `tx_eth_src` (auto-fill source MAC address)
- **`pacing_mbps`**: Packet-pacing rate cap for this queue, in megabits per second of L2 frame
  bytes (the data the application transmits, excluding preamble/IFG/FCS). The NIC meters the queue
  out so its long-run average TX rate stays at or below this value; the limit is enforced on an
  average basis and idle gaps do not accumulate burst credit. `0` (the default) disables pacing
  and sends at line rate. Supported only by the default `dpdk` raw engine on a NIC with hardware
  send scheduling (ConnectX-7 or later); on devices without it, `pacing_mbps` is ignored with a
  warning and TX runs at line rate. The `ibverbs` raw engine does **not** support `pacing_mbps`
  and `daqiri_init()` fails if it is set on an `ibverbs` queue.
  - type: `integer`
  - default: `0`

### Transmit Flows

`tx.flows:` — Raw Ethernet hardware transform rules for outgoing packets. Supported on
the DPDK and ibverbs raw engines only. TX flows match the packet as supplied by the
application, then push or encapsulate headers in hardware; the application buffer remains
the pre-encap packet.

- **`name`** / **`id`**: Flow label and ID.
- **`actions`**: Ordered transform action list. TX flows cannot contain `queue`.
  - **`type: vlan_push`**: Push one VLAN tag.
    - **`vlan_id`**: VLAN ID, `0..4095`.
    - **`pcp`**: Priority, `0..7`, default `0`.
    - **`dei`**: Drop eligible indicator, `0..1`, default `0`.
    - **`ethertype`**: VLAN TPID, default `0x8100`.
  - **`type: tunnel_encap`**: Encapsulate in `vxlan`, `gre`, or `nvgre`.
    - **`tunnel.type`**: `vxlan`, `gre`, or `nvgre`.
    - **`outer_eth_src` / `outer_eth_dst`** and **`outer_ipv4_src` /
      `outer_ipv4_dst`** are required.
    - **VXLAN fields**: `vni`, optional `outer_udp_src`, `outer_udp_dst` default `4789`.
    - **GRE fields**: optional `gre_protocol` default `0x0800`.
    - **NVGRE fields**: `tni`, optional `flow_id`.
- **`match`**: Same standard UDP/IP match keys as RX flows. Omit `match` for a
  catch-all TX transform.

DAQIRI validates tunnel overhead against the configured packet buffer size and
the supported jumbo-frame bound. For RX decap/pop, MTU sizing accounts for the
outer wire frame while packet buffers contain the post-decap frame.

### Accurate Send

`tx.accurate_send:` — Enable hardware-timed packet transmission using PTP timestamps. When
enabled, use `set_packet_tx_time()` to schedule packets. Requires ConnectX-7 or later.

- type: `boolean`
- default: `false`

## Complete Example (Raw Ethernet, Header-Data Split)

```yaml
%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: "raw"
    master_core: 3
    debug: false
    log_level: "info"

    memory_regions:
    - name: "Data_TX_CPU"
      kind: "huge"
      affinity: 0
      num_bufs: 51200
      buf_size: 64
    - name: "Data_TX_GPU"
      kind: "device"
      affinity: 0
      num_bufs: 51200
      buf_size: 1064
    - name: "Data_RX_CPU"
      kind: "huge"
      affinity: 0
      num_bufs: 51200
      buf_size: 64
    - name: "Data_RX_GPU"
      kind: "device"
      affinity: 0
      num_bufs: 51200
      buf_size: 1000

    interfaces:
    - name: "tx_port"
      address: <PCIe BDF>
      tx:
        queues:
        - name: "tx_q_0"
          id: 0
          batch_size: 10240
          cpu_core: 11
          memory_regions:
            - "Data_TX_CPU"
            - "Data_TX_GPU"
          offloads:
            - "tx_eth_src"
    - name: "rx_port"
      address: <PCIe BDF>
      rx:
        flow_isolation: true
        queues:
        - name: "rx_q_0"
          id: 0
          cpu_core: 9
          batch_size: 10240
          memory_regions:
            - "Data_RX_CPU"
            - "Data_RX_GPU"
        flows:
        - name: "flow_0"
          id: 0
          action:
            type: queue
            id: 0
          match:
            udp_src: 4096
            udp_dst: 4096
            ipv4_len: 1050
```

## Complete Example (RDMA, Client/Server)

```yaml
%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: "socket"
    master_core: 3
    debug: false
    log_level: "info"

    memory_regions:
    - name: "DATA_TX"
      kind: "host_pinned"
      affinity: 0
      num_bufs: 20
      buf_size: 9000000
    - name: "DATA_RX"
      kind: "host_pinned"
      affinity: 0
      num_bufs: 20
      buf_size: 9000000

    interfaces:
    - name: my_server
      address: 10.100.3.1
      socket_config:
        mode: server
        local_addr: "roce://10.100.3.1:4096"
      roce_config:
        transport_mode: RC
      rx:
        queues:
        - name: "Server_RX_Queue"
          id: 0
          cpu_core: 8
          batch_size: 1
      tx:
        queues:
        - name: "Server_TX_Queue"
          id: 0
          cpu_core: 8
          batch_size: 1
```
