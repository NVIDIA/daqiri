# C++ API Usage

This guide covers C++ initialization, RX/TX workflows, buffer lifecycle calls, file
writing, utility helpers, and status codes. The function calls below follow the
six-step lifecycle introduced in the [API Guide](index.md): init → RX/TX → access
→ free → shutdown. Include the canonical public header,
[`daqiri/daqiri.h`](https://github.com/NVIDIA/daqiri/blob/main/include/daqiri/daqiri.h),
in C++ applications.

For the terminology used here (*burst*, *segment*, *flow*, *queue*, *memory
region*, *zero-copy ownership*, *RX reorder*), keep the
[Concepts](../concepts.md) page open in a second tab.

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

If GPU RX `reorder_configs` are configured for the DPDK backend, set one CUDA stream
per GPU reorder plan before pulling reordered bursts. CPU reorder configs do not use a
CUDA stream. See the [Configuration YAML Reference](configuration.md#rx-reorder-configs-dpdk-v1)
for reorder configuration constraints.

```cpp
cudaStream_t stream = /* your stream */;
auto st = daqiri::set_reorder_cuda_stream("rx_port", "rx_reorder_0", stream);
if (st != daqiri::Status::SUCCESS) {
    // handle setup error
}
```

## Receiving Packets

### RX Step 1 — Get a burst

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

### RX Step 2 — Access packet data

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

### RX Step 3 — Free buffers

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

## Reordered RX Bursts

For an overview of what RX reorder is and when to use it, see
[Concepts → RX Packet Aggregation and Reorder](../concepts.md#rx-packet-aggregation-and-reorder).
This section covers how to consume reordered bursts from C++.

Reordered RX bursts can be identified from `burst->hdr.hdr.burst_flags`:

- `DAQIRI_BURST_FLAG_REORDERED` means the burst contains one aggregated reorder buffer.
- `DAQIRI_BURST_FLAG_REORDER_TIMEOUT` means that aggregate was emitted by the timeout path
  rather than by filling the configured `packets_per_batch`.
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

### GPU packet processing on reordered bursts

When using batched GPU mode, packets arrive in CUDA-addressable buffers — each at an
arbitrary GPU address. Launch your own CUDA work directly on the packet pointers. Packet
reordering and aggregation should be configured through `rx.reorder_configs`; see
`raw_reorder_seq_bench.cpp` and `raw_reorder_quantize_bench.cpp` for complete examples
that consume DAQIRI's built-in reordered bursts.

```cpp
__global__ void noop_packet_kernel(void *packet) {
    (void)packet;
}

if (daqiri::get_num_packets(burst) > 0) {
    void *packet = daqiri::get_packet_ptr(burst, 0);
    noop_packet_kernel<<<1, 1, 0, stream>>>(packet);
}

// Free once the kernel completes
daqiri::free_all_packets_and_burst_rx(burst);
```

## Transmitting Packets

### TX Step 1 — Allocate a burst

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

### TX Step 2 — Fill packets

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

### TX Step 3 — Send

```cpp
daqiri::send_tx_burst(burst);
```

The burst is enqueued to the TX worker thread, which sends it to the NIC via DMA.

### Timed Transmission

For precise packet scheduling (requires ConnectX-7+):

```cpp
daqiri::set_packet_tx_time(burst, idx, ptp_timestamp);
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

## Function Reference

This section summarizes the C++ functions available through `daqiri/daqiri.h`. The
workflow sections above show the common call order and ownership rules.

### Initialization, Parsing, and Lifecycle

| Function | Purpose |
| --- | --- |
| `daqiri_init(NetworkConfig &config)` | Initialize DAQIRI from an already-populated config object. |
| `daqiri_init(const std::string &yaml_string_or_path)` | Initialize from a YAML string or YAML file path. |
| `daqiri_init_from_yaml_string(const std::string &yaml_string)` | Initialize from YAML content. |
| `daqiri_init_from_yaml_file(const std::string &yaml_path)` | Initialize from a YAML file path. |
| `parse_network_config(...)` | Parse YAML into `NetworkConfig` without starting the manager. |
| `get_manager_type()` | Return the active manager type after initialization. |
| `get_manager_type(config)` | Return the manager type selected by a config object. |
| `shutdown()` | Stop DAQIRI and release manager-owned resources. |
| `print_stats()` | Print manager/backend statistics. |

### Burst Metadata

| Function | Purpose |
| --- | --- |
| `create_burst_params()` | Allocate generic burst metadata. |
| `create_tx_burst_params()` | Allocate TX burst metadata. |
| `set_header(burst, port, q, num, segs)` | Set burst port, queue, packet count, and segment count metadata. |
| `set_num_packets(burst, num)` / `get_num_packets(burst)` | Set or read the number of packets in a burst. |
| `get_q_id(burst)` | Return the queue ID recorded on a burst. |
| `get_burst_tot_byte(burst)` | Return the burst total-byte counter. |

### Packet and Segment Access

| Function | Purpose |
| --- | --- |
| `get_packet_ptr(burst, idx)` | Return the segment-0 packet pointer. |
| `get_segment_packet_ptr(burst, seg, idx)` | Return a packet pointer for a specific segment. |
| `get_packet_length(burst, idx)` | Return the logical packet length. |
| `get_segment_packet_length(burst, seg, idx)` | Return the length of one packet segment. |
| `get_packet_flow_id(burst, idx)` | Return the matched flow ID, or `0` when no flow matched. |
| `get_packet_rx_timestamp(burst, idx, &timestamp_ns)` | Return the hardware RX timestamp when enabled and available. |

### RX and Reorder

| Function | Purpose |
| --- | --- |
| `get_rx_burst(&burst, port, q)` | Dequeue a burst from a specific port and queue. |
| `get_rx_burst(&burst, port)` | Dequeue from any queue on a specific port. |
| `get_rx_burst(&burst)` | Dequeue from any queue on any port. |
| `get_rx_burst(&burst, conn_id, server)` | Dequeue from an RDMA/socket connection ring. |
| `set_reorder_cuda_stream(interface_name, reorder_name, stream)` | Set the CUDA stream for a configured GPU reorder plan. |
| `get_reorder_burst_info(burst, &info)` | Read metadata for a reordered aggregate burst. |

### TX and Header Fill

| Function | Purpose |
| --- | --- |
| `is_tx_burst_available(burst)` | Check whether buffers are available for a TX burst. |
| `get_tx_packet_burst(burst)` | Populate a TX burst with packet buffers. |
| `send_tx_burst(burst)` | Enqueue a populated TX burst. |
| `set_packet_lengths(burst, idx, lens)` | Set segment lengths for one packet. |
| `set_all_packet_lengths(burst, lens)` | Set segment lengths for every packet in a burst. |
| `set_packet_tx_time(burst, idx, time)` | Set scheduled transmit time for one packet. |
| `set_eth_header(burst, idx, dst_addr)` | Fill the Ethernet destination header. |
| `set_ipv4_header(burst, idx, ip_len, proto, src_host, dst_host)` | Fill an IPv4 header. |
| `set_udp_header(burst, idx, udp_len, src_port, dst_port)` | Fill a UDP header. |
| `set_udp_payload(burst, idx, data, len)` | Copy UDP payload bytes. |
| `rdma_set_header(burst, op_code, conn_id, is_server, num_pkts, wr_id, local_mr_name)` | Fill RDMA TX metadata. |
| `rdma_get_opcode(burst)` | Return the RDMA operation code recorded on a burst. |

### Buffer Release

| Function | Purpose |
| --- | --- |
| `free_packet(burst, idx)` | Free all segments for one packet. |
| `free_packet_segment(burst, seg, idx)` | Free one segment for one packet. |
| `free_all_segment_packets(burst, seg)` | Free one segment across all packets in a burst. |
| `free_segment_packets_and_burst(burst, seg)` | Free one segment across all packets and free burst metadata. |
| `free_all_packets_and_burst_rx(burst)` | Free all RX packet buffers and RX burst metadata. |
| `free_all_packets_and_burst_tx(burst)` | Free all TX packet buffers and TX burst metadata. |
| `free_rx_burst(burst)` / `free_tx_burst(burst)` | Free burst metadata only. |
| `free_rx_metadata(burst)` / `free_tx_metadata(burst)` | Free RX or TX metadata only. |

### File I/O

| Function | Purpose |
| --- | --- |
| `daqiri_write_raw_to_file(burst, absolute_path, file_prefix, packet_data_offset)` | Write each packet to a separate raw binary file. |
| `daqiri_write_raw_to_file_async(..., &handle)` | Submit asynchronous raw packet writes. |
| `daqiri_write_pcap_to_file(burst, absolute_path, file_prefix)` | Append burst packets to a classic pcap file. |
| `daqiri_write_pcap_to_file_async(..., &handle)` | Submit asynchronous pcap writes. |
| `daqiri_file_write_poll(handle, &status)` | Poll an asynchronous file-write handle. |
| `daqiri_file_write_wait(handle, &status)` | Wait for asynchronous file writes to complete. |
| `daqiri_file_write_destroy(handle)` | Release asynchronous file-write resources. |

### Ports, Traffic, Socket, and RDMA

| Function | Purpose |
| --- | --- |
| `get_mac_addr(port, mac)` | Copy a port MAC address into a six-byte buffer. |
| `format_eth_addr(dst, addr)` | Convert a MAC address string into a six-byte buffer. |
| `get_port_id(key)` | Resolve an interface name or PCIe address to a port ID. |
| `get_num_rx_queues(port_id)` | Return the configured or backend-reported RX queue count. |
| `drop_all_traffic(port)` | Install a high-priority drop rule on a port. |
| `allow_all_traffic(port)` | Remove a drop rule installed by `drop_all_traffic()`. |
| `flush_port_queue(port, queue)` | Drain stale packets from a port queue. |
| `socket_connect_to_server(server_addr, server_port, &conn_id)` | Connect a socket client to a server. |
| `socket_connect_to_server(server_addr, server_port, src_addr, &conn_id)` | Connect a socket client using an explicit source address. |
| `socket_get_port_queue(conn_id, &port, &queue)` | Resolve a socket connection to port/queue routing. |
| `socket_get_server_conn_id(server_addr, server_port, &conn_id)` | Look up a server-side socket connection ID. |
| `rdma_connect_to_server(server_addr, server_port, &conn_id)` | Connect an RDMA client to a server. |
| `rdma_connect_to_server(server_addr, server_port, src_addr, &conn_id)` | Connect an RDMA client using an explicit source address. |
| `rdma_get_port_queue(conn_id, &port, &queue)` | Resolve an RDMA connection to port/queue routing. |
| `rdma_get_server_conn_id(server_addr, server_port, &conn_id)` | Look up a server-side RDMA connection ID. |

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
