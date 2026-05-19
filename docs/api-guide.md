# API Guide

This guide covers the core DAQIRI API for receiving and transmitting packets. Include
the canonical public header, [`daqiri/daqiri.h`](https://github.com/NVIDIA/daqiri/blob/main/include/daqiri/daqiri.h), in C++ applications.

## Key Concepts

### BurstParams

All packet data flows through the `BurstParams` structure. A burst is a batch of packets
grouped together for efficient transfer between the NIC and the application.

`BurstParams` provides:
- Pointers to packet buffers (CPU or GPU memory)
- Packet metadata: packet count, port/queue IDs, segment count, byte totals
- Per-packet lengths, flow IDs, and optional RX hardware timestamps

Interact with `BurstParams` only through the helper functions described below — the
internal layout is opaque.

### Zero-Copy Design

Only pointers are passed between the NIC, DAQIRI internals, and the application. When
you receive packets, you are reading the same buffers the NIC DMA'd into — no copies
are made.

This means **you must explicitly free buffers** when done processing. Failure to free
buffers will exhaust the memory pool, causing the NIC to drop packets and DAQIRI to
return `NO_FREE_BURST_BUFFERS` or `NO_FREE_PACKET_BUFFERS` errors.

### Segments

A **segment** is a contiguous memory region (in CPU or GPU memory) that holds part
of a packet. A single packet can span multiple segments, which lets different parts
of the packet live in different memory domains.

The most common case is header-data split (HDS), where:
- Segment 0 = headers (CPU memory)
- Segment 1 = payload (GPU memory)

For CPU-only or batched-GPU modes, there is a single segment (segment 0).

## Initialization

```cpp
#include <daqiri/daqiri.h>

// Initialize from a YAML config file
auto status = daqiri::daqiri_init("path/to/config.yaml");

// Or build the configuration in code
daqiri::NetworkConfig config;
// Populate configuration struct
auto status = daqiri::daqiri_init(config);
```

After `daqiri_init()` returns `Status::SUCCESS`, all memory regions are allocated, NIC
queues are configured, and worker threads are running.

If GPU RX `reorder_configs` are configured (DPDK backend), set one CUDA stream per GPU reorder
plan before pulling reordered bursts. CPU reorder configs do not use a CUDA stream:

```cpp
cudaStream_t stream = /* your stream */;
auto st = daqiri::set_reorder_cuda_stream("rx_port", "rx_reorder_0", stream);
if (st != daqiri::Status::SUCCESS) {
    // handle setup error
}
```

Reorder batch sizing requirement (v1):
- Packets within a single reordered batch are expected to be the same size.
- Mixed packet sizes in the same reorder batch are not supported.
- Reorder queues must use one RX source memory region. Header-data split RX queues are not
  supported for reorder in v1.
- `reorder_type: "gpu"` requires `device` or `host_pinned` packet/output memory.
- `reorder_type: "cpu"` requires `host`, `host_pinned`, or `huge` packet/output memory.
- If a reorder config includes `data_types`, `payload_len` and `aggregate_len` describe the
  converted output element size, not the on-wire byte count.
- `data_types.endianness: "network"` interprets byte-multiple input types wider than 8 bits as
  network byte order before conversion.

Reordered RX bursts can be identified from `burst->hdr.hdr.burst_flags`:
- `DAQIRI_BURST_FLAG_REORDERED` means the burst contains one aggregated reorder buffer.
- `DAQIRI_BURST_FLAG_REORDER_TIMEOUT` means that aggregate was emitted by the timeout path rather
  than by filling the configured `packets_per_batch`.
- For reordered bursts, `burst->hdr.hdr.max_pkt` is the logical number of source packets in the
  aggregate, while `burst->hdr.hdr.num_pkts` remains `1` because the consumer receives one
  aggregate buffer.
- The aggregate batch number is available through `daqiri::get_reorder_burst_info(...)`.
  For `seq_batch_number`, this is the configured batch-number field. For
  `seq_packets_per_batch`, it is derived as `sequence_number / packets_per_batch`, so sequence
  numbers `0..1023` map to batch `0` when `packets_per_batch` is `1024`, `1024..2047` map to
  batch `1`, and so on.

```cpp
daqiri::ReorderBurstInfo info{};
if ((burst->hdr.hdr.burst_flags & daqiri::DAQIRI_BURST_FLAG_REORDERED) != 0U) {
    if (burst->event != nullptr) {
        cudaEventSynchronize(burst->event);
    }
    auto st = daqiri::get_reorder_burst_info(burst, &info);
    if (st == daqiri::Status::SUCCESS) {
        // info.batch_id identifies the aggregate batch.
    }
}
```

## Receiving Packets

### Step 1: Get a burst

```cpp
daqiri::BurstParams *burst;
int port_id = 0;
int queue_id = 0;
auto status = daqiri::get_rx_burst(&burst, port_id, queue_id);
```

`get_rx_burst()` is non-blocking. It returns `Status::SUCCESS` when a complete batch is
available, or `Status::NULL_PTR` when no burst is ready yet. There are also overloads
that dequeue from any queue on a port, or from any queue on any port:

```cpp
// From any queue on port 0
daqiri::get_rx_burst(&burst, 0);

// From any queue on any port (round-robin)
daqiri::get_rx_burst(&burst);
```

### Step 2: Access packet data

For a single-segment configuration (CPU-only or batched GPU):

```cpp
for (int i = 0; i < daqiri::get_num_packets(burst); i++) {
    void *pkt = daqiri::get_packet_ptr(burst, i);
    uint32_t len = daqiri::get_packet_length(burst, i);
    uint16_t flow = daqiri::get_packet_flow_id(burst, i);
    uint64_t rx_ts_ns = 0;
    if (daqiri::get_packet_rx_timestamp(burst, i, &rx_ts_ns) == daqiri::Status::SUCCESS) {
        // rx_ts_ns is in the NIC timestamp clock domain.
    }
    // process packet...
}
```

RX hardware timestamps are available only when the DPDK backend is configured with
`rx.hardware_timestamps: true` and the NIC supports `RTE_ETH_RX_OFFLOAD_TIMESTAMP`.
DAQIRI converts the NIC timestamp counter to nanoseconds internally using DPDK's
matching device clock when available, or the PMD's nanosecond timestamp format when
the driver already supplies nanoseconds. DAQIRI does not expose NIC clock reads or
convert timestamps to wall-clock time. For reordered aggregate bursts,
`get_packet_rx_timestamp(burst, 0, &ts)` returns the timestamp of the first source
packet accepted into the aggregate.

For header-data split (two segments):

```cpp
for (int i = 0; i < daqiri::get_num_packets(burst); i++) {
    void *hdr = daqiri::get_segment_packet_ptr(burst, 0, i);  // CPU pointer
    void *pay = daqiri::get_segment_packet_ptr(burst, 1, i);  // GPU pointer
    uint32_t hdr_len = daqiri::get_segment_packet_length(burst, 0, i);
    uint32_t pay_len = daqiri::get_segment_packet_length(burst, 1, i);
}
```

### Step 3: Free buffers

When you are done processing, free the burst to return buffers to the pool:

```cpp
// Free all packets and the burst metadata
daqiri::free_all_packets_and_burst_rx(burst);
```

You can also free individual packets or segments if your pipeline releases buffers
incrementally:

```cpp
// Free a single packet (all segments)
daqiri::free_packet(burst, idx);

// Free one segment of a packet
daqiri::free_packet_segment(burst, seg, idx);

// Free all packets for one segment, then the burst
daqiri::free_all_segment_packets(burst, seg);
daqiri::free_rx_burst(burst);
```

### GPU Packet Aggregation

When using batched GPU mode, packets arrive in scattered buffers — each at an arbitrary
GPU address. For workloads that need contiguous data, DAQIRI provides a CUDA reorder
kernel (`simple_packet_reorder` in `src/kernels.cu`) that copies scattered packets into
a flat output buffer:

```cpp
// Collect GPU pointers from the burst
for (int p = 0; p < daqiri::get_num_packets(burst); p++) {
    h_dev_ptrs[p] = daqiri::get_packet_ptr(burst, p);
}

// Reorder into a contiguous GPU buffer
simple_packet_reorder(output_buffer, h_dev_ptrs, packet_len, num_packets);

// Free once the kernel completes
daqiri::free_all_packets_and_burst_rx(burst);
```

## Writing Bursts to Storage

Received bursts can be written to local storage either as raw packet data or as a classic
pcap capture. Raw writes create one output file per packet named
`<file_prefix>_<packet_index>` and truncate existing files. PCAP writes append full
packets to `<file_prefix>.pcap`; DAQIRI writes the pcap headers directly and does not
depend on `libpcap`.

Host-backed packet segments use standard POSIX file writes and do not require GPUDirect
Storage. CUDA device-backed packet segments require `DAQIRI_ENABLE_GDS=ON` and working
NVIDIA cuFile support. If a device-backed segment is encountered without GDS support,
DAQIRI logs a warning and returns `NOT_SUPPORTED`.

For regular cuFile/GDS mode, the runtime system must also have the `nvidia-fs` kernel
module loaded and the destination storage stack supported by cuFile. Check with:

```bash
lsmod | grep nvidia_fs
/usr/local/cuda/gds/tools/gdscheck.py -p
```

The target filesystem must pass cuFile validation before DAQIRI can register the output
file. For local NVMe, `gdscheck.py -p` should report `NVMe : Supported`; ext4 mounts
must expose `data=ordered`, while XFS is also supported by GDS.

For raw writes, the `packet_data_offset` argument skips bytes from the logical packet
before writing. For header-data split bursts, DAQIRI walks segment 0, segment 1, and any
later segments as one contiguous packet, so an offset that skips the CPU header can write
only the GPU payload.

```cpp
auto st = daqiri::daqiri_write_raw_to_file(
    burst,
    "/mnt/nvme/capture",
    "packet_group_0",
    60);
if (st != daqiri::Status::SUCCESS) {
    // handle write error
}
```

PCAP writes always include full logical packets and append packet records by default. If
the pcap file does not exist or is empty, DAQIRI writes a classic pcap v2.4 global
header first. If it already exists, it must contain a compatible Ethernet,
microsecond-resolution pcap header.

```cpp
auto st = daqiri::daqiri_write_pcap_to_file(
    burst,
    "/mnt/nvme/capture",
    "packet_group_0");
```

For asynchronous writes, keep the burst and its packet buffers alive until the file-write
handle completes:

```cpp
daqiri::FileWriteHandle *handle = nullptr;
auto st = daqiri::daqiri_write_raw_to_file_async(
    burst, "/mnt/nvme/capture", "packet_group_0", 60, &handle);

daqiri::FileWriteStatus status{};
if (st == daqiri::Status::SUCCESS) {
    st = daqiri::daqiri_file_write_wait(handle, &status);
    daqiri::daqiri_file_write_destroy(handle);
}
```

The async API is a DAQIRI handle rather than a CUDA stream because one burst can fan out
to many file offsets and, for device-backed segments, cuFile handles. Host-backed
segments may complete during submission. Device-backed segments use cuFile batch I/O,
which is submitted and polled with a `CUfileBatchHandle_t`.

See `examples/gds_write_example.cpp` for a sample that sends one deterministic burst and
writes raw or pcap output with the synchronous API, the asynchronous API, or both. Use
`daqiri_example_gds_write_sw_loopback.yaml` for local software loopback, or
`daqiri_example_gds_write_tx_rx.yaml` after replacing its PCIe, MAC, and IP placeholders
to send real Ethernet/IPv4/UDP frames out of a NIC and receive them back through a
hardware RX port.

## Transmitting Packets

### Step 1: Allocate a burst

```cpp
auto burst = daqiri::create_tx_burst_params();
daqiri::set_header(burst, port_id, queue_id, batch_size, num_segments);

auto status = daqiri::get_tx_packet_burst(burst);
if (status != daqiri::Status::SUCCESS) {
    // No buffers available — retry later
}
```

You can check availability before allocating:

```cpp
if (daqiri::is_tx_burst_available(burst)) {
    daqiri::get_tx_packet_burst(burst);
}
```

For connection-oriented transports such as TCP socket mode, attach the connection ID before
sending when you need to target a specific peer. RX bursts from those transports can be
inspected with the matching getter:

```cpp
daqiri::set_connection_id(burst, conn_id);
auto rx_conn_id = daqiri::get_connection_id(rx_burst);
```

### Step 2: Fill packets

Use the header helper functions for standard UDP packets:

```cpp
for (int i = 0; i < daqiri::get_num_packets(burst); i++) {
    daqiri::set_eth_header(burst, i, dst_mac);
    daqiri::set_ipv4_header(burst, i, ip_payload_len, IPPROTO_UDP, src_ip, dst_ip);
    daqiri::set_udp_header(burst, i, udp_payload_len, src_port, dst_port);
    daqiri::set_udp_payload(burst, i, payload_ptr, payload_size);
    daqiri::set_packet_lengths(burst, i, {total_pkt_len});
}
```

Or construct raw packets by writing directly into the packet buffer returned by
`get_packet_ptr()`.

### Step 3: Send

```cpp
daqiri::send_tx_burst(burst);
```

The burst is enqueued to the TX worker thread, which sends it to the NIC via DMA.

### Timed Transmission

For precise packet scheduling (requires ConnectX-7+):

```cpp
daqiri::set_packet_tx_time(burst, idx, ptp_timestamp);
```

## Utility Functions

```cpp
// Get MAC address of a port
char mac[6];
daqiri::get_mac_addr(port_id, mac);

// Look up port ID by interface name or PCIe address
int port = daqiri::get_port_id("rx_port");

// Traffic control
daqiri::drop_all_traffic(port_id);    // drop all incoming packets
daqiri::allow_all_traffic(port_id);   // restore normal traffic

// Drain stale packets from a queue
daqiri::flush_port_queue(port_id, queue_id);

// Print NIC statistics
daqiri::print_stats();

// Shutdown and cleanup
daqiri::shutdown();
```

## Status Codes

All functions that can fail return `daqiri::Status`:

| Status | Meaning |
|--------|---------|
| `SUCCESS` | Operation completed successfully |
| `NULL_PTR` | Burst or internal pointer not initialized / no data ready |
| `NO_FREE_BURST_BUFFERS` | Metadata buffer pool exhausted (increase `tx/rx_meta_buffers`) |
| `NO_FREE_PACKET_BUFFERS` | Packet buffer pool exhausted (free buffers faster or increase `num_bufs`) |
| `NOT_READY` | System not yet initialized |
| `INVALID_PARAMETER` | Invalid argument passed |
| `NO_SPACE_AVAILABLE` | Ring or queue is full |
| `NOT_SUPPORTED` | Operation not supported by the current backend or build options |
| `GENERIC_FAILURE` | Unspecified failure |
| `CONNECT_FAILURE` | RDMA connection failed |
| `INTERNAL_ERROR` | Internal error in the backend |
