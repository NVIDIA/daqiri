# Python API Reference

DAQIRI exposes Python bindings through the `daqiri` package. The package re-exports
the compiled pybind11 module, so application code can use one import:

```python
import daqiri
```

The Python API mirrors the C++ free-function API. Most functions return a
`daqiri.Status` value, or a tuple whose first item is a `Status`. Packet storage is
still owned by DAQIRI, so received and transmitted bursts must be explicitly freed.

## Build and Import

Build the bindings by enabling `DAQIRI_BUILD_PYTHON` when configuring DAQIRI:

```bash
cmake -S . -B build \
  -DBUILD_SHARED_LIBS=ON \
  -DDAQIRI_BUILD_PYTHON=ON \
  -DDAQIRI_MGR="dpdk socket rdma"
cmake --build build -j
cmake --install build --prefix /opt/daqiri
```

The Python build requires `pybind11`. The installed package is placed under the
configured library directory, in `python/daqiri`. Add the parent directory, for
example `/opt/daqiri/lib/python`, to `PYTHONPATH` before importing `daqiri`.

PyYAML is required at runtime when calling `daqiri_init()` with a Python `dict`
or a config-like object that provides `as_dict()`.

For a complete executable Python benchmark, see the
[Python raw TX/RX benchmark example](https://github.com/NVIDIA/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx.py).

## Initialization

`daqiri.daqiri_init(config)` accepts more input forms than the C++ overload:

- YAML file path
- YAML string
- Python `dict`, converted to YAML with PyYAML
- `daqiri.NetworkConfig`
- config-like object with a `value` attribute or `as_dict()` method

```python
import daqiri

status = daqiri.daqiri_init("daqiri_bench_raw_tx_rx.yaml")
if status != daqiri.Status.SUCCESS:
    raise RuntimeError(f"DAQIRI init failed: {status}")

try:
    # RX/TX work here.
    pass
finally:
    daqiri.print_stats()
    daqiri.shutdown()
```

Initialize from a Python dictionary:

```python
import daqiri

config = {
    "version": 1,
    "stream_type": "raw",
    "master_core": 3,
    "log_level": "warn",
    "memory_regions": [
        {
            "name": "RX_CPU",
            "kind": "huge",
            "affinity": 0,
            "access": ["local"],
            "num_bufs": 51200,
            "buf_size": 1024,
        }
    ],
    "interfaces": [
        {
            "name": "rx_port",
            "address": "0000:03:00.0",
            "rx": {
                "queues": [
                    {
                        "name": "rxq0",
                        "id": 0,
                        "cpu_core": "8",
                        "batch_size": 1024,
                        "memory_regions": ["RX_CPU"],
                    }
                ]
            },
        }
    ],
}

status = daqiri.daqiri_init(config)
```

Parse without starting the manager:

```python
status, parsed = daqiri.parse_network_config("daqiri_bench_raw_tx_rx.yaml")
if status == daqiri.Status.SUCCESS:
    print(parsed.common.manager_type)
```

## RX Workflow

`get_rx_burst()` is non-blocking. It returns `(Status, BurstParams | None)`.
`Status.SUCCESS` means a burst is ready. Other statuses, including `NULL_PTR`, mean
there is no burst to process or an error occurred.

```python
import time
import daqiri

port_id = daqiri.get_port_id("rx_port")

while True:
    status, burst = daqiri.get_rx_burst(port_id, 0)
    if status != daqiri.Status.SUCCESS or burst is None:
        time.sleep(0.0001)
        continue

    try:
        for idx in range(daqiri.get_num_packets(burst)):
            status, payload = daqiri.get_packet_bytes(burst, idx)
            if status == daqiri.Status.SUCCESS:
                process_packet(payload)
    finally:
        daqiri.free_all_packets_and_burst_rx(burst)
```

Available RX overloads:

```python
status, burst = daqiri.get_rx_burst(port_id, queue_id)
status, burst = daqiri.get_rx_burst(port_id)
status, burst = daqiri.get_rx_burst()
status, burst = daqiri.get_rx_burst_for_connection(conn_id, server=True)
```

## TX Workflow

TX code creates burst metadata, asks DAQIRI for packet buffers, fills the buffers,
sets packet lengths, then sends the burst.

```python
import time
import daqiri

port_id = daqiri.get_port_id("tx_port")
batch_size = 1024
packet = bytes([0] * 1064)

burst = daqiri.create_tx_burst_params()
daqiri.set_header(burst, port_id, 0, batch_size, 1)

if not daqiri.is_tx_burst_available(burst):
    daqiri.free_tx_metadata(burst)
    time.sleep(0.0001)
else:
    status = daqiri.get_tx_packet_burst(burst)
    if status != daqiri.Status.SUCCESS:
        daqiri.free_tx_metadata(burst)
    else:
        failed = False
        for idx in range(daqiri.get_num_packets(burst)):
            status = daqiri.copy_buffer_to_packet(burst, idx, packet)
            if status != daqiri.Status.SUCCESS:
                failed = True
                break

            status = daqiri.set_packet_lengths(burst, idx, [len(packet)])
            if status != daqiri.Status.SUCCESS:
                failed = True
                break

        if failed:
            daqiri.free_all_packets_and_burst_tx(burst)
        else:
            status = daqiri.send_tx_burst(burst)
```

Header helpers are available when DAQIRI is responsible for filling packet headers:

```python
daqiri.set_eth_header(burst, idx, "aa:bb:cc:dd:ee:ff")
daqiri.set_ipv4_header(burst, idx, ip_len, daqiri.IPPROTO_UDP, "1.2.3.4", "5.6.7.8")
daqiri.set_udp_header(burst, idx, udp_len, 4096, 4096)
daqiri.set_udp_payload(burst, idx, payload)
```

## Pointer and Buffer Semantics

`get_packet_ptr()` and `get_segment_packet_ptr()` return integer addresses. They do
not return Python-managed memory objects and they do not transfer ownership to
Python.

Use copy helpers unless the application deliberately needs raw addresses for CUDA or
foreign-function interop:

```python
status = daqiri.copy_buffer_to_segment_packet(
    burst,
    seg=0,
    idx=0,
    data=b"payload",
    nbytes=None,
    src_offset=0,
    dst_offset=0,
)

status, data = daqiri.get_segment_packet_bytes(
    burst,
    seg=0,
    idx=0,
    nbytes=None,
    src_offset=0,
)
```

The copy helpers handle CPU pointers and CUDA device pointers. Device copies use
`cudaMemcpy` internally.

## Socket and RDMA

Socket helpers return connection IDs and queue routing information:

```python
status, conn_id = daqiri.socket_connect_to_server("192.0.2.10", 5000)
if status == daqiri.Status.SUCCESS:
    status, port, queue = daqiri.socket_get_port_queue(conn_id)
```

The returned port and queue can be used with the normal burst APIs:

```python
status, conn_id = daqiri.socket_connect_to_server("192.0.2.10", 5000)
if status == daqiri.Status.SUCCESS:
    status, port, queue = daqiri.socket_get_port_queue(conn_id)

    status, rx_burst = daqiri.get_rx_burst(port, queue)
    if status == daqiri.Status.SUCCESS and rx_burst is not None:
        try:
            for idx in range(daqiri.get_num_packets(rx_burst)):
                status, data = daqiri.get_packet_bytes(rx_burst, idx)
        finally:
            daqiri.free_all_packets_and_burst_rx(rx_burst)

    tx_burst = daqiri.create_tx_burst_params()
    daqiri.set_header(tx_burst, port, queue, 1, 1)
    if daqiri.get_tx_packet_burst(tx_burst) == daqiri.Status.SUCCESS:
        daqiri.copy_buffer_to_packet(tx_burst, 0, b"hello")
        daqiri.set_packet_lengths(tx_burst, 0, [5])
        daqiri.send_tx_burst(tx_burst)
    else:
        daqiri.free_tx_metadata(tx_burst)
```

RDMA helpers follow the same tuple-returning style:

```python
status, conn_id = daqiri.rdma_connect_to_server("192.0.2.10", 5000)
if status == daqiri.Status.SUCCESS:
    burst = daqiri.create_tx_burst_params()
    daqiri.rdma_set_header(
        burst,
        daqiri.RDMAOpCode.SEND,
        conn_id,
        False,
        1,
        0,
        "TX_GPU",
    )
```

For server-side lookup:

```python
status, conn_id = daqiri.rdma_get_server_conn_id("192.0.2.10", 5000)
status, conn_id = daqiri.socket_get_server_conn_id("192.0.2.10", 5000)
```

## Reorder Bursts

GPU reorder users can provide a CUDA stream address as an integer. Pass `0` to use
the default stream.

```python
status = daqiri.set_reorder_cuda_stream("rx_port", "rx_reorder_0", 0)
if status != daqiri.Status.SUCCESS:
    raise RuntimeError(status)

status, burst = daqiri.get_rx_burst()
if status == daqiri.Status.SUCCESS and burst is not None:
    try:
        if burst.hdr.hdr.burst_flags & daqiri.DAQIRI_BURST_FLAG_REORDERED:
            daqiri.synchronize_burst_event(burst)
            status, info = daqiri.get_reorder_burst_info(burst)
            if status == daqiri.Status.SUCCESS:
                print(info.batch_id, info.aggregate_len)
    finally:
        daqiri.free_all_packets_and_burst_rx(burst)
```

## GIL Behavior

The binding releases the Python GIL around blocking or long-running DAQIRI calls
where the pybind11 wrapper declares `py::gil_scoped_release`, including initialization
from YAML strings/files, RX burst dequeue, TX burst allocation/send, file writes, and
burst event synchronization. Python code still needs its own synchronization for
shared Python objects used by worker threads.

## Constants

| Name | Description |
| --- | --- |
| `ADV_NETWORK_HEADER_SIZE_BYTES` | Advanced network header size constant from the C++ API. |
| `MAX_NUM_RX_QUEUES` | Maximum configured RX queues. |
| `MAX_NUM_TX_QUEUES` | Maximum configured TX queues. |
| `MAX_INTERFACES` | Maximum configured interfaces. |
| `MAX_NUM_SEGS` | Maximum packet segments per burst. |
| `DAQIRI_BURST_FLAG_REORDERED` | Burst flag indicating a reordered aggregate. |
| `DAQIRI_BURST_FLAG_REORDER_TIMEOUT` | Burst flag indicating a reorder timeout aggregate. |
| `MEM_ACCESS_LOCAL` | Local memory access flag. |
| `MEM_ACCESS_RDMA_WRITE` | RDMA write memory access flag. |
| `MEM_ACCESS_RDMA_READ` | RDMA read memory access flag. |
| `IPPROTO_UDP` | UDP IP protocol number used by header helpers. |

## Enums

| Enum | Values |
| --- | --- |
| `Status` | `SUCCESS`, `NULL_PTR`, `NO_FREE_BURST_BUFFERS`, `NO_FREE_PACKET_BUFFERS`, `NOT_READY`, `INVALID_PARAMETER`, `NO_SPACE_AVAILABLE`, `NOT_SUPPORTED`, `GENERIC_FAILURE`, `CONNECT_FAILURE`, `INTERNAL_ERROR` |
| `RDMAOpCode` | `CONNECT`, `SEND`, `RECEIVE`, `RDMA_WRITE`, `RDMA_WRITE_IMM`, `RDMA_READ`, `RDMA_READ_IMM`, `INVALID` |
| `RDMACompletionType` | `RX`, `TX`, `INVALID` |
| `ManagerType` | `UNKNOWN`, `DEFAULT`, `DPDK`, `SOCKET`, `RDMA` |
| `Direction` | `RX`, `TX`, `TX_RX` |
| `BufferLocation` | `CPU`, `GPU`, `CPU_GPU_SPLIT` |
| `MemoryKind` | `HOST`, `HOST_PINNED`, `HUGE`, `DEVICE`, `INVALID` |
| `StreamType` | `RAW`, `SOCKET`, `INVALID` |
| `SocketProtocol` | `TCP`, `UDP`, `ROCE`, `INVALID` |
| `LoopbackType` | `DISABLED`, `LOOPBACK_TYPE_SW` |
| `RDMAMode` | `CLIENT`, `SERVER`, `INVALID` |
| `RDMATransportMode` | `RC`, `UC`, `UD`, `INVALID` |
| `SocketMode` | `CLIENT`, `SERVER`, `INVALID` |
| `FlowType` | `QUEUE` |
| `FlowMatchType` | `NORMAL`, `FLEX_ITEM` |
| `ReorderMethod` | `INVALID`, `SEQ_BATCH_NUMBER`, `SEQ_PACKETS_PER_BATCH` |
| `ReorderDataType` | `SAME`, `INT4`, `INT8`, `INT16`, `INT32`, `FP16`, `BF16`, `FP32`, `FP64`, `INVALID` |
| `ReorderEndianness` | `HOST`, `NETWORK`, `INVALID` |
| `LogLevel` | `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL`, `OFF` |
| `ErrorGlobalStats` | `OUT_OF_RX_BUFFERS`, `RX_QUEUE_FULL`, `METADATA_BUF_DEPLETED`, `SENTINEL` |

## Data and Config Classes

All exposed classes have a default constructor and read/write attributes. Config
classes mirror the C++ configuration structs, with Python attribute names that omit
the trailing underscore from the C++ member name.

| Class | Purpose |
| --- | --- |
| `BurstParams` | Opaque burst handle used by RX, TX, free, and packet-access helpers. Exposes `hdr`, `rdma_conn_id`, and `rdma_wr_id`. |
| `BurstHeader` | Wrapper for `BurstHeaderParams`. |
| `BurstHeaderParams` | Burst metadata such as packet count, port, queue, segment count, byte totals, and reorder flags. |
| `ReorderBurstInfo` | Metadata for reordered aggregate bursts. |
| `NetworkConfig` | Top-level parsed DAQIRI configuration. |
| `CommonConfig` | Global manager, direction, stream, protocol, loopback, and core settings. |
| `InterfaceConfig` | Per-interface address, socket/RoCE/RDMA, RX, and TX configuration. |
| `RxConfig` | RX flow isolation, timestamps, queues, flows, flex items, and reorder configs. |
| `TxConfig` | TX accurate-send flag, queues, and flows. |
| `CommonQueueConfig` | Shared queue fields including name, ID, batch size, split boundary, CPU core, memory regions, and offloads. |
| `RxQueueConfig` | RX queue wrapper with common queue fields and timeout. |
| `TxQueueConfig` | TX queue wrapper with common queue fields. |
| `MemoryRegionConfig` | Memory region kind, affinity, access flags, sizes, counts, and ownership. |
| `FlowAction` | Flow action type and target ID. |
| `FlowMatch` | Flow match fields for UDP, IPv4, and flex item matching. |
| `FlowConfig` | Named flow rule combining action and match. |
| `FlexItemConfig` | Flexible parser item configuration. |
| `FlexItemMatch` | Flexible parser match value and mask. |
| `SocketConfig` | Socket client/server endpoint and timing settings. |
| `RoCEConfig` | RoCE transport settings. |
| `RDMAConfig` | RDMA mode, transport mode, and port. |
| `ReorderConfig` | Reorder name, type, memory region, payload offset, flows, method, and data type conversion. |
| `ReorderBitFieldConfig` | Bit offset and width for extracting reorder fields. |
| `ReorderSeqBatchNumberConfig` | Sequence-number, batch-number, and packets-per-batch field config. |
| `ReorderSeqPacketsPerBatchConfig` | Sequence-number and packets-per-batch field config. |
| `ReorderDataTypesConfig` | Optional reorder input/output data type conversion settings. |

## Function Reference

### Initialization and Parsing

| Function | Returns |
| --- | --- |
| `daqiri_init(config)` | `Status` |
| `daqiri_init_from_yaml_string(yaml_string)` | `Status` |
| `daqiri_init_from_yaml_file(yaml_path)` | `Status` |
| `parse_network_config(yaml_string_or_path)` | `(Status, NetworkConfig)` |
| `parse_network_config_from_yaml_string(yaml_string)` | `(Status, NetworkConfig)` |
| `parse_network_config_from_yaml_file(yaml_path)` | `(Status, NetworkConfig)` |

### String and Type Helpers

| Function | Purpose |
| --- | --- |
| `get_manager_type()` | Return the active `ManagerType`. |
| `manager_type_from_string(str)` / `manager_type_to_string(type)` | Convert manager types. |
| `stream_type_from_string(str)` / `stream_type_to_string(type)` | Convert stream types. |
| `socket_protocol_from_string(str)` / `socket_protocol_to_string(protocol)` | Convert socket protocols. |
| `reorder_data_type_from_string(str)` / `reorder_data_type_to_string(type)` | Convert reorder data types. |
| `reorder_endianness_from_string(str)` / `reorder_endianness_to_string(endianness)` | Convert reorder endianness values. |
| `log_level_from_string(str)` / `log_level_to_string(level)` | Convert log levels. |

### Burst Metadata and Packet Access

| Function | Purpose |
| --- | --- |
| `create_burst_params()` | Allocate generic burst metadata. |
| `create_tx_burst_params()` | Allocate TX burst metadata. |
| `set_header(burst, port, q, num, segs)` | Set burst header metadata. |
| `set_num_packets(burst, num)` / `get_num_packets(burst)` | Set or read packet count. |
| `get_q_id(burst)` | Read queue ID. |
| `get_burst_tot_byte(burst)` | Read total byte count for a burst. |
| `get_packet_ptr(burst, idx)` | Return segment 0 packet address as an integer. |
| `get_segment_packet_ptr(burst, seg, idx)` | Return segment packet address as an integer. |
| `get_packet_length(burst, idx)` / `get_segment_packet_length(burst, seg, idx)` | Read packet or segment length. |
| `get_packet_flow_id(burst, idx)` | Read packet flow ID. |
| `get_packet_rx_timestamp(burst, idx)` | Return `(Status, timestamp_ns)`. |
| `copy_buffer_to_packet(burst, idx, data, nbytes=None, src_offset=0, dst_offset=0)` | Copy a Python buffer into segment 0. |
| `copy_buffer_to_segment_packet(burst, seg, idx, data, nbytes=None, src_offset=0, dst_offset=0)` | Copy a Python buffer into a segment. |
| `get_packet_bytes(burst, idx, nbytes=None, src_offset=0)` | Return `(Status, bytes)` from segment 0. |
| `get_segment_packet_bytes(burst, seg, idx, nbytes=None, src_offset=0)` | Return `(Status, bytes)` from a segment. |
| `set_packet_lengths(burst, idx, lens)` | Set segment lengths for one packet. |
| `set_all_packet_lengths(burst, lens)` | Set segment lengths for every packet. |
| `set_packet_tx_time(burst, idx, time)` | Set scheduled TX time for one packet. |

### RX, TX, and Header Helpers

| Function | Purpose |
| --- | --- |
| `get_rx_burst(...)` | Dequeue an RX burst from a queue, port, or any port. |
| `get_rx_burst_for_connection(conn_id, server)` | Dequeue an RX burst for an RDMA/socket connection. |
| `is_tx_burst_available(burst)` | Check whether TX packet buffers are available. |
| `get_tx_packet_burst(burst)` | Populate a TX burst with packet buffers. |
| `send_tx_burst(burst)` | Enqueue a populated TX burst. |
| `set_eth_header(burst, idx, dst_addr)` | Fill the Ethernet header destination address. |
| `set_ipv4_header(burst, idx, ip_len, proto, src_host, dst_host)` | Fill an IPv4 header. |
| `set_udp_header(burst, idx, udp_len, src_port, dst_port)` | Fill a UDP header. |
| `set_udp_payload(burst, idx, data)` | Copy UDP payload bytes. |

### Free Functions

| Function | Purpose |
| --- | --- |
| `free_packet(burst, idx)` | Free all segments for one packet. |
| `free_packet_segment(burst, seg, idx)` | Free one segment for one packet. |
| `free_all_segment_packets(burst, seg)` | Free one segment across all packets. |
| `free_all_packets_and_burst_rx(burst)` | Free all RX packets and RX burst metadata. |
| `free_all_packets_and_burst_tx(burst)` | Free all TX packets and TX burst metadata. |
| `free_segment_packets_and_burst(burst, seg)` | Free one segment and burst metadata. |
| `free_rx_burst(burst)` / `free_tx_burst(burst)` | Free only burst metadata. |
| `free_rx_metadata(burst)` / `free_tx_metadata(burst)` | Free only RX or TX metadata. |

### File I/O, Ports, Filters, and Lifecycle

| Function | Purpose |
| --- | --- |
| `daqiri_write_raw_to_file(burst, absolute_path, file_prefix, packet_data_offset)` | Write burst packets as raw files. |
| `daqiri_write_pcap_to_file(burst, absolute_path, file_prefix)` | Append burst packets to a pcap file. |
| `get_mac_addr(port)` | Return `(Status, "aa:bb:cc:dd:ee:ff")`. |
| `format_eth_addr(addr)` | Return six MAC-address bytes. |
| `get_port_id(key)` | Resolve an interface key to a port ID. |
| `drop_all_traffic(port)` / `allow_all_traffic(port)` | Update port traffic policy. |
| `get_num_rx_queues(port_id)` | Return configured RX queue count for a port. |
| `flush_port_queue(port, queue)` | Flush packets pending on a port queue. |
| `shutdown()` | Shut down the active manager. |
| `print_stats()` | Print manager statistics. |

### Reorder, Socket, and RDMA

| Function | Purpose |
| --- | --- |
| `set_reorder_cuda_stream(interface_name, reorder_name, stream=0)` | Set CUDA stream for a GPU reorder plan. |
| `get_reorder_burst_info(burst)` | Return `(Status, ReorderBurstInfo)`. |
| `synchronize_burst_event(burst)` | Synchronize the CUDA event attached to a burst. |
| `socket_connect_to_server(server_addr, server_port[, src_addr])` | Return `(Status, conn_id)`. |
| `socket_get_port_queue(conn_id)` | Return `(Status, port, queue)`. |
| `socket_get_server_conn_id(server_addr, server_port)` | Return `(Status, conn_id)`. |
| `rdma_connect_to_server(server_addr, server_port[, src_addr])` | Return `(Status, conn_id)`. |
| `rdma_get_port_queue(conn_id)` | Return `(Status, port, queue)`. |
| `rdma_get_server_conn_id(server_addr, server_port)` | Return `(Status, conn_id)`. |
| `rdma_set_header(burst, op_code, conn_id, is_server, num_pkts, wr_id, local_mr_name)` | Fill RDMA TX metadata. |
| `rdma_get_opcode(burst)` | Return the RDMA operation code for a burst. |
