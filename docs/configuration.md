# Configuration Reference

DAQIRI is configured through a YAML file or a `NetworkConfig` struct built in code.
Either form defines memory regions, NIC interfaces, TX/RX queues, and flow rules, and
is passed to `daqiri_init()` at startup. The struct form is useful for customers who
want to interoperate with existing configuration code.

See `examples/daqiri_bench_*.yaml` for complete working examples.

## Common Configuration

These settings apply globally to both TX and RX:

- **`version`**: Config version. Only `1` is valid currently.
  - type: `integer`
- **`master_core`**: CPU core used to fork and join network threads. This core is not used
  for packet processing and can be bound to a non-isolated core. Should differ from isolated
  cores assigned to queues.
  - type: `integer`
- **`manager`**: Backend networking library.
  - type: `string`
  - values: `dpdk` (default), `rdma`
- **`log_level`**: Backend log level.
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
  on RX or higher latency on TX. Rule of thumb: 3x-5x `batch_size`. For the DPDK
  backend, `num_bufs` below 1.5x the NIC ring size deadlocks the worker; `daqiri_init`
  auto-bumps such MRs to 3x the ring (24576 with the default 8192) and logs a `WARN`.
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
- **`address`**: PCIe BDF address (from `lspci`) or Linux interface name for DPDK, or IP
  address for RDMA.
  - type: `string`

### RDMA Configuration

When using RDMA, set `stream_type: "socket"` and `protocol: "roce"`. Each interface then
uses a `socket_config` block for endpoint role/addressing plus a `roce_config` block for
RDMA transport settings:

- **`socket_config.mode`**: Connection role.
  - type: `string`
  - values: `client`, `server`
- **`socket_config.local_ip`** / **`socket_config.local_port`**: Server bind address/port.
- **`socket_config.remote_ip`** / **`socket_config.remote_port`**: Client peer address/port.
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
- **`id`**: ID of the flex item.
  - type: `integer`
- **`offset`**: Byte offset after the UDP header where matching begins. Must be a multiple
  of 4 and less than 28.
  - type: `integer`
- **`udp_dst_port`**: UDP destination port for flex item activation.
  - type: `integer`

### Flows

`rx.flows:` — Flow rules that steer packets to specific queues based on match criteria.

- **`name`**: Flow name.
  - type: `string`
- **`id`**: Flow ID. Retrievable at runtime via `get_packet_flow_id()`.
  - type: `integer`
- **`action`**: What to do with matched packets.
  - **`type`**: Action type. Only `queue` is currently supported.
    - type: `string`
  - **`id`**: Queue ID to steer matched packets to.
    - type: `integer`
- **`match`**: Criteria for matching packets.
  - **`udp_src`**: UDP source port or port range (e.g., `1000-1010`).
    - type: `integer` or `string`
  - **`udp_dst`**: UDP destination port or port range.
    - type: `integer` or `string`
  - **`ipv4_len`**: IPv4 payload length.
    - type: `integer`
  - **`flex_item_id`**: Flex item ID (from `rx.flex_items`). Cannot be combined with UDP/IP
    matching.
    - type: `integer`
  - **`val`**: 32-bit value to match (with flex items).
    - type: `integer`
  - **`mask`**: 32-bit mask applied before matching (with flex items).
    - type: `integer`

### Flow Isolation

`rx.flow_isolation:` — When `true`, only packets matching an explicit flow rule are delivered.
Unmatched packets are dropped. When `false`, unmatched packets go to a default queue.

- type: `boolean`
- default: `false`

### Hardware Timestamps

`rx.hardware_timestamps:` — Enable per-packet hardware RX timestamps for the DPDK backend.
When enabled, DAQIRI requires `RTE_ETH_RX_OFFLOAD_TIMESTAMP` support from the NIC/PMD and
initialization fails if DAQIRI cannot provide nanosecond timestamps for the selected PMD.
Timestamps are returned by `get_packet_rx_timestamp()` in nanoseconds in the NIC timestamp
clock domain, not wall-clock time.

- type: `boolean`
- default: `false`

### RX Reorder Configs (DPDK v1)

`rx.reorder_configs:` — Optional automatic packet reordering/aggregation plans. In v1 this is
implemented for the DPDK backend only. GPU reorder requires CUDA-addressable packet buffers
(`device` or `host_pinned` memory regions). CPU reorder requires CPU-addressable packet buffers
(`host`, `host_pinned`, or `huge` memory regions).

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
- **`reorder_type`**: Reorder backend implementation.
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
- **`offloads`**: List of hardware offloads to enable.
  - type: `list`
  - values: `tx_eth_src` (auto-fill source MAC address)

### Accurate Send

`tx.accurate_send:` — Enable hardware-timed packet transmission using PTP timestamps. When
enabled, use `set_packet_tx_time()` to schedule packets. Requires ConnectX-7 or later.

- type: `boolean`
- default: `false`

## Complete Example (DPDK, Header-Data Split)

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
    protocol: "roce"
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
        local_ip: 10.100.3.1
        local_port: 4096
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
