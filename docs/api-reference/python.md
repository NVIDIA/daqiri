---
hide:
  - navigation
---

# Python API Usage

This guide covers Python initialization, RX/TX workflows, buffer lifecycle calls,
file writes, and the bindings for utility, socket, and RDMA helpers. The function
calls below follow the six-step lifecycle introduced in the [API Guide](index.md):
init → RX/TX → access → free → shutdown. The bindings are produced from
[`python/daqiri_common_pybind.cpp`](https://github.com/NVIDIA/daqiri/blob/main/python/daqiri_common_pybind.cpp)
and mirror the C++ free-function API in [C++ API Usage](cpp.md).

For the terminology used here (*burst*, *segment*, *flow*, *queue*, *memory
region*, *zero-copy ownership*, *RX reorder*), keep the
[Concepts](../concepts.md) page open in a second tab.

Functions that can fail return a `daqiri.Status` value, or a tuple whose first
element is a `Status` and whose remaining elements are output values. Packet
storage is owned by DAQIRI, so received and transmitted bursts must be explicitly
freed — see
[Concepts → Zero-Copy Ownership](../concepts.md#zero-copy-ownership) for why a
missed free causes RX drops.

For a complete executable Python benchmark, see
[`examples/daqiri_bench_raw_tx_rx.py`](https://github.com/NVIDIA/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx.py).

## Building and Importing

Follow the build flow in [Getting Started](../getting-started.md) — container
or bare-metal — and add `-DDAQIRI_BUILD_PYTHON=ON` to the CMake configure step
(the default is `OFF`). The container path is recommended; it already provides
`pybind11` and the rest of the build dependencies. For example, inside the
container:

```bash
cmake -S . -B build \
  -DBUILD_SHARED_LIBS=ON \
  -DDAQIRI_BUILD_PYTHON=ON \
  -DDAQIRI_ENGINE="dpdk ibverbs"
cmake --build build -j
cmake --install build --prefix /opt/daqiri
```

A bare-metal build uses the same commands but additionally requires `pybind11`
on the host.

The installed package is placed under the configured library directory, in
`python/daqiri`. Add the parent directory (for example
`/opt/daqiri/lib/python`) to `PYTHONPATH` before importing:

```python
import daqiri
```

The `daqiri` package re-exports the compiled `_daqiri` pybind11 module, so
application code uses one import.

DAQIRI package versions use CalVer in `YYYY.MM.PATCH` form. The Python package
exposes `daqiri.__version__`, `daqiri.__version_info__`, and
`daqiri.__abi_version__`; the ABI version is intentionally separate from the
CalVer package version. The YAML `common.version` field remains the
configuration schema version.

```python
import daqiri

print(daqiri.__version__)
print(daqiri.version_string(), daqiri.abi_version())
```

After initialization, `daqiri.get_stream_type()` is the authoritative transport
query. For `StreamType.PCIE`, `get_engine_type()` returns
`EngineType.DEFAULT`; DMA-BUF is internal and is not an engine enum value.

PyYAML is required at runtime when calling `daqiri_init()` with a Python `dict`
or a config-like object that provides `as_dict()`.

## Initialization

`daqiri.daqiri_init(config)` accepts more input forms than the C++ overload:

- YAML file path
- YAML string
- Python `dict`, converted to YAML with PyYAML
- `daqiri.NetworkConfig` instance
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

Only one engine may be active at a time. A second `daqiri_init()` before `shutdown()`
returns `Status.INTERNAL_ERROR` without changing the live engine. After `shutdown()`, a
later initialization starts with a fresh engine instance and fresh resources.

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

Parse without starting the engine:

```python
status, parsed = daqiri.parse_network_config("daqiri_bench_raw_tx_rx.yaml")
if status == daqiri.Status.SUCCESS:
    print(parsed.common.engine_type)
```

If GPU RX `reorder_configs` are configured for the DPDK engine, set one CUDA
stream per GPU reorder plan before pulling reordered bursts. Pass the CUDA
stream as an integer address; pass `0` to use the default stream. See the
[Configuration YAML Reference](configuration.md#rx-reorder-configs)
for reorder configuration constraints.

```python
status = daqiri.set_reorder_cuda_stream("rx_port", "rx_reorder_0", 0)
if status != daqiri.Status.SUCCESS:
    raise RuntimeError(status)
```

## Receiving Packets

### RX Step 1 — Get a burst

`get_rx_burst()` is non-blocking. It returns `(Status, BurstParams | None)`.
`Status.SUCCESS` means a burst is ready; other statuses (including `NULL_PTR`)
mean no burst is ready yet or an error occurred.

```python
port_id = daqiri.get_port_id("rx_port")
status, burst = daqiri.get_rx_burst(port_id, 0)
```

Available overloads:

```python
status, burst = daqiri.get_rx_burst(port_id, queue_id)
status, burst = daqiri.get_rx_burst(port_id)             # any queue on a port
status, burst = daqiri.get_rx_burst()                    # any queue on any port
status, burst = daqiri.get_rx_burst_for_connection(conn_id, server=True)
```

### RX Step 2 — Access packet data

For a single-segment burst (CPU-only or batched GPU), copy data out with
`get_packet_bytes`:

```python
for idx in range(daqiri.get_num_packets(burst)):
    status, payload = daqiri.get_packet_bytes(burst, idx)
    if status == daqiri.Status.SUCCESS:
        process_packet(payload)
```

For header-data split (two segments):

```python
for idx in range(daqiri.get_num_packets(burst)):
    status, header  = daqiri.get_segment_packet_bytes(burst, 0, idx)
    status, payload = daqiri.get_segment_packet_bytes(burst, 1, idx)
```

Other per-packet accessors:

```python
length  = daqiri.get_packet_length(burst, idx)
seg_len = daqiri.get_segment_packet_length(burst, 0, idx)
flow    = daqiri.get_packet_flow_id(burst, idx)
status, rx_ts_ns = daqiri.get_packet_rx_timestamp(burst, idx)
```

RX hardware timestamps are available only when the DPDK engine is configured
with `rx.hardware_timestamps: true` and the NIC supports
`RTE_ETH_RX_OFFLOAD_TIMESTAMP`. See
[C++ API Usage → Receiving Packets](cpp.md#receiving-packets) for the clock
semantics; the Python wrapper exposes the same timestamps in nanoseconds.

### RX Step 3 — Free buffers

When you are done with a burst, free it:

```python
daqiri.free_all_packets_and_burst_rx(burst)
```

Or free incrementally:

```python
daqiri.free_packet(burst, idx)               # one packet (all segments)
daqiri.free_packet_segment(burst, seg, idx)  # one segment of one packet
daqiri.free_all_segment_packets(burst, seg)  # one segment across the burst
daqiri.free_rx_burst(burst)                  # metadata only
```

### Pointer and Buffer Semantics

`daqiri.get_packet_ptr()` and `daqiri.get_segment_packet_ptr()` return integer
addresses. They do not return Python-managed memory objects and they do not
transfer ownership to Python.

Prefer the copy helpers unless the application deliberately needs raw addresses
for CUDA or foreign-function interop. The copy helpers handle both CPU pointers
and CUDA device pointers; device copies use `cudaMemcpy` internally.

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

## Reordered RX Bursts

For an overview of what RX reorder is and when to use it, see
[Concepts → RX Packet Aggregation and Reorder](../concepts.md#rx-packet-aggregation-and-reorder).
This section covers how to consume reordered bursts from Python.

Reordered RX bursts are identified by flags on `burst.hdr.hdr.burst_flags`:

- `DAQIRI_BURST_FLAG_REORDERED` — burst contains one aggregated reorder buffer.
- `DAQIRI_BURST_FLAG_REORDER_TIMEOUT` — the aggregate was emitted by the
  timeout path rather than by filling the configured `packets_per_batch`.

For reordered bursts, `burst.hdr.hdr.max_pkt` is the logical number of source
packets in the aggregate, while `burst.hdr.hdr.num_pkts` remains `1` because the
consumer receives one aggregate buffer.

```python
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

`synchronize_burst_event()` waits for the CUDA event attached to the burst, if
any.

## Transmitting Packets

### TX Step 1 — Allocate a burst

```python
port_id = daqiri.get_port_id("tx_port")
batch_size = 1024

burst = daqiri.create_tx_burst_params()
daqiri.set_header(burst, port_id, 0, batch_size, 1)

if not daqiri.is_tx_burst_available(burst):
    daqiri.free_tx_metadata(burst)
    # retry later
else:
    status = daqiri.get_tx_packet_burst(burst)
    if status != daqiri.Status.SUCCESS:
        daqiri.free_tx_metadata(burst)
        # retry later
```

### TX Step 2 — Fill packets

Header helpers are available when DAQIRI is responsible for filling packet
headers. `set_ipv4_header()` takes the source and destination hosts as
**integers**, not dotted-quad strings — convert with `ipaddress.IPv4Address` (or
your own packer) before calling:

```python
import ipaddress

src_host = int(ipaddress.IPv4Address("1.2.3.4"))
dst_host = int(ipaddress.IPv4Address("5.6.7.8"))

daqiri.set_eth_header(burst, idx, "aa:bb:cc:dd:ee:ff")
daqiri.set_ipv4_header(burst, idx, ip_len, daqiri.IPPROTO_UDP, src_host, dst_host)
daqiri.set_udp_header(burst, idx, udp_len, 4096, 4096)
daqiri.set_udp_payload(burst, idx, payload_bytes)
```

Or copy a complete packet from a Python buffer:

```python
packet = bytes([0] * 1064)
for idx in range(daqiri.get_num_packets(burst)):
    status = daqiri.copy_buffer_to_packet(burst, idx, packet)
    status = daqiri.set_packet_lengths(burst, idx, [len(packet)])
```

### TX Step 3 — Send

```python
status = daqiri.send_tx_burst(burst)
# Do not free `burst` after a SUCCESS or NO_SPACE_AVAILABLE return — see the ownership note below.
```

`send_tx_burst()` takes ownership of the burst on success and on a full-ring
failure: on `SUCCESS` the worker thread owns it, and on `NO_SPACE_AVAILABLE` (the
TX ring is full) it has already freed the packets and the burst internally. In
both cases the application must **not** free or otherwise access the burst
afterwards. `NO_SPACE_AVAILABLE` is the only failure a correctly-configured
sender encounters at runtime.

### PCIe GPU-buffer ordering

PCIe uses the same Python burst calls, with one additional ownership rule:
finish every GPU write to a TX packet before `send_tx_burst()`, and finish every
GPU read from an RX packet before calling a packet/burst free helper. The free
call credits an RX slot back to the FPGA, and a successful send lets the FPGA
read the TX slot. DAQIRI cannot synchronize application-owned CUDA streams for
you.

Use the synchronization primitive of the CUDA Python library that owns the work
(for example, a CuPy stream synchronization) at those boundaries. Do not keep a
persistent kernel touching FPGA-owned slots. DAQIRI publishes an RX burst only
after applying the platform's required GPUDirect visibility operation; it
reclaims TX storage only after the FPGA reports that all reads completed.

PCIe supports no flows or network headers. Flow, MAC/header, scheduled-send,
timestamp, and socket/RDMA helpers return `Status.NOT_SUPPORTED`, while
`get_packet_flow_id()` returns `0`. See
[PCIe / GPUDirect Benchmarking](../benchmarks/pcie_benchmarking.md) for the
driver/FPGA protocol.

### Timed Transmission

For precise packet scheduling (requires ConnectX-7+):

```python
daqiri.set_packet_tx_time(burst, idx, ptp_timestamp_ns)
```

## Writing Bursts to Storage

Received bursts can be written to local storage as raw packet data or as a
classic pcap capture. The Python bindings expose the synchronous write
helpers:

```python
status = daqiri.daqiri_write_raw_to_file(
    burst, "/mnt/nvme/capture", "packet_group_0", packet_data_offset=60,
)

status = daqiri.daqiri_write_pcap_to_file(
    burst, "/mnt/nvme/capture", "packet_group_0",
)
```

CUDA device-backed packet segments require `DAQIRI_ENABLE_GDS=ON` and working
NVIDIA cuFile support. See
[C++ API Usage → Writing Bursts to Storage](cpp.md#writing-bursts-to-storage)
for the full GDS constraints; the asynchronous file-write API is C++-only at
this time.

## Socket and RDMA

Socket helpers return connection IDs and queue routing information. The
returned port and queue can then be used with the normal burst APIs:

```python
import socket

status, conn_id = daqiri.socket_connect_to_server("192.0.2.10", 5000)
if status == daqiri.Status.SUCCESS:
    status, port, queue = daqiri.socket_get_port_queue(conn_id)

    # Socket options use OS constants directly; DAQIRI does not map option names.
    daqiri.socket_setsockopt(conn_id, socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)

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
        # send_tx_burst() takes ownership of tx_burst on success and on a
        # full-ring failure, so do not free it here.
        daqiri.send_tx_burst(tx_burst)
    else:
        daqiri.free_tx_metadata(tx_burst)
```

`socket_setsockopt(conn_id, level, optname, value)` applies Linux `setsockopt`
to an existing TCP/UDP socket connection. `value` may be `bool`, `int`, `str`,
`bytes`, `bytearray`, or another bytes-like buffer. For structured options such
as timeouts, pack the native struct yourself and pass it as bytes.

RDMA follows the same tuple-returning style:

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

Look up server-side connection IDs:

```python
status, conn_id = daqiri.rdma_get_server_conn_id("192.0.2.10", 5000)
status, conn_id = daqiri.socket_get_server_conn_id("192.0.2.10", 5000)
```

## Utility Functions

```python
status, mac_str = daqiri.get_mac_addr(port_id)        # "aa:bb:cc:dd:ee:ff"
mac_bytes       = daqiri.format_eth_addr("aa:bb:cc:dd:ee:ff")
port            = daqiri.get_port_id("rx_port")
num_queues      = daqiri.get_num_rx_queues(port_id)

daqiri.drop_all_traffic(port_id)
daqiri.allow_all_traffic(port_id)
daqiri.flush_port_queue(port_id, queue_id)

daqiri.print_stats()
daqiri.shutdown()
```

## GIL Behavior

The bindings release the Python GIL around blocking or long-running DAQIRI
calls where the pybind11 wrapper declares `py::gil_scoped_release`. This
currently includes:

- `daqiri_init` — all input forms (YAML path/string, dict, `NetworkConfig`,
  `value`/`as_dict` config-like objects). The GIL is released around the
  underlying DAQIRI call; any Python-side conversion (PyYAML `dump`, `as_dict()`
  invocation) still runs with the GIL held.
- `daqiri_init_from_yaml_string` and `daqiri_init_from_yaml_file`
- `get_rx_burst` (all overloads) and `get_rx_burst_for_connection`
- `is_tx_burst_available`, `get_tx_packet_burst`, `send_tx_burst`
- `copy_buffer_to_packet`, `copy_buffer_to_segment_packet` — released around
  the underlying memory copy (host or `cudaMemcpy`).
- `get_packet_bytes`, `get_segment_packet_bytes` — released around the copy
  out of the DAQIRI buffer.
- `daqiri_write_raw_to_file`, `daqiri_write_pcap_to_file`
- `synchronize_burst_event`

Python code still needs its own synchronization for shared Python objects used
by worker threads.

## Function Reference

This section summarizes the Python functions exposed by the `daqiri` module.
The workflow sections above show the common call order and ownership rules.

### Initialization, Parsing, and Lifecycle

| Function | Returns |
| --- | --- |
| `daqiri_init(config)` | `Status` |
| `daqiri_init_from_yaml_string(yaml_string)` | `Status` |
| `daqiri_init_from_yaml_file(yaml_path)` | `Status` |
| `parse_network_config(yaml_string_or_path)` | `(Status, NetworkConfig)` |
| `parse_network_config_from_yaml_string(yaml_string)` | `(Status, NetworkConfig)` |
| `parse_network_config_from_yaml_file(yaml_path)` | `(Status, NetworkConfig)` |
| `get_stream_type()` | `StreamType` |
| `get_engine_type()` | `EngineType` |
| `shutdown()` | `None` |
| `print_stats()` | `None` |

### String and Type Helpers

| Function | Purpose |
| --- | --- |
| `version_string()` | Return the DAQIRI package version as `YYYY.MM.PATCH`. |
| `version_year()` / `version_month()` / `version_patch()` | Return the CalVer components. |
| `abi_version()` | Return the DAQIRI shared-library ABI version. |
| `engine_type_from_string(str)` / `engine_type_to_string(type)` | Convert engine types. |
| `stream_type_from_string(str)` / `stream_type_to_string(type)` | Convert stream types. |
| `reorder_data_type_from_string(str)` / `reorder_data_type_to_string(type)` | Convert reorder data types. |
| `reorder_endianness_from_string(str)` / `reorder_endianness_to_string(endianness)` | Convert reorder endianness values. |
| `log_level_from_string(str)` / `log_level_to_string(level)` | Convert log levels. |

### Burst Metadata and Packet Access

| Function | Purpose |
| --- | --- |
| `create_burst_params()` | Allocate generic burst metadata. |
| `create_tx_burst_params()` | Allocate TX burst metadata. |
| `set_header(burst, port, q, num, segs)` | Set burst port, queue, packet count, and segment count. |
| `set_num_packets(burst, num)` / `get_num_packets(burst)` | Set or read packet count. |
| `get_q_id(burst)` | Read queue ID. |
| `get_burst_tot_byte(burst)` | Read total byte count for a burst. |
| `get_packet_ptr(burst, idx)` | Return segment 0 packet address as an integer. |
| `get_segment_packet_ptr(burst, seg, idx)` | Return segment packet address as an integer. |
| `get_packet_length(burst, idx)` / `get_segment_packet_length(burst, seg, idx)` | Read packet or segment length. |
| `get_packet_flow_id(burst, idx)` | Read matched flow ID, or `0` when no flow matched. |
| `get_packet_rx_timestamp(burst, idx)` | Return `(Status, timestamp_ns)`. |
| `copy_buffer_to_packet(burst, idx, data, nbytes=None, src_offset=0, dst_offset=0)` | Copy a Python buffer into segment 0. |
| `copy_buffer_to_segment_packet(burst, seg, idx, data, nbytes=None, src_offset=0, dst_offset=0)` | Copy a Python buffer into a specific segment. |
| `get_packet_bytes(burst, idx, nbytes=None, src_offset=0)` | Return `(Status, bytes)` from segment 0. |
| `get_segment_packet_bytes(burst, seg, idx, nbytes=None, src_offset=0)` | Return `(Status, bytes)` from a segment. |
| `set_packet_lengths(burst, idx, lens)` | Set segment lengths for one packet. |
| `set_all_packet_lengths(burst, lens)` | Set segment lengths for every packet. |
| `set_packet_tx_time(burst, idx, time)` | Set scheduled TX time for one packet. |

### RX and Reorder

| Function | Purpose |
| --- | --- |
| `get_rx_burst(port, queue)` | Dequeue an RX burst from a specific port and queue. |
| `get_rx_burst(port)` | Dequeue from any queue on a port. |
| `get_rx_burst()` | Dequeue from any queue on any port. |
| `get_rx_burst_for_connection(conn_id, server)` | Dequeue for a socket/RDMA connection. |
| `get_connection_id(burst)` | Read the transport connection ID recorded on an RX burst. |
| `set_reorder_cuda_stream(interface_name, reorder_name, stream=0)` | Set CUDA stream for a GPU reorder plan. |
| `get_reorder_burst_info(burst)` | Return `(Status, ReorderBurstInfo)`. |
| `synchronize_burst_event(burst)` | Wait for the CUDA event attached to a burst, if any. |

### TX and Header Fill

| Function | Purpose |
| --- | --- |
| `is_tx_burst_available(burst)` | Check whether TX packet buffers are available. |
| `get_tx_packet_burst(burst)` | Populate a TX burst with packet buffers. |
| `set_connection_id(burst, conn_id)` | Attach a transport connection ID to a TX burst (socket/RDMA). |
| `send_tx_burst(burst)` | Enqueue a populated TX burst. |
| `set_eth_header(burst, idx, dst_addr)` | Fill the Ethernet header destination address. |
| `set_ipv4_header(burst, idx, ip_len, proto, src_host, dst_host)` | Fill an IPv4 header. |
| `set_udp_header(burst, idx, udp_len, src_port, dst_port)` | Fill a UDP header. |
| `set_udp_payload(burst, idx, data)` | Copy UDP payload bytes. |
| `rdma_set_header(burst, op_code, conn_id, is_server, num_pkts, wr_id, local_mr_name)` | Fill RDMA TX metadata. |
| `rdma_get_opcode(burst)` | Return the RDMA operation code for a burst. |

### Buffer Release

| Function | Purpose |
| --- | --- |
| `free_packet(burst, idx)` | Free all segments for one packet. |
| `free_packet_segment(burst, seg, idx)` | Free one segment for one packet. |
| `free_all_segment_packets(burst, seg)` | Free one segment across all packets. |
| `free_segment_packets_and_burst(burst, seg)` | Free one segment across all packets and free burst metadata. |
| `free_all_packets_and_burst_rx(burst)` | Free all RX packets and RX burst metadata. |
| `free_all_packets_and_burst_tx(burst)` | Free all TX packets and TX burst metadata. |
| `free_rx_burst(burst)` / `free_tx_burst(burst)` | Free burst metadata only. |
| `free_rx_metadata(burst)` / `free_tx_metadata(burst)` | Free only RX or TX metadata. |

### File I/O

| Function | Purpose |
| --- | --- |
| `daqiri_write_raw_to_file(burst, absolute_path, file_prefix, packet_data_offset)` | Write burst packets as raw files. |
| `daqiri_write_pcap_to_file(burst, absolute_path, file_prefix)` | Append burst packets to a pcap file. |

### Ports, Traffic, Socket, and RDMA

| Function | Purpose |
| --- | --- |
| `get_mac_addr(port)` | Return `(Status, "aa:bb:cc:dd:ee:ff")`. |
| `format_eth_addr(addr)` | Return six MAC-address bytes from a `xx:xx:xx:xx:xx:xx` MAC string; invalid input returns zero bytes. |
| `get_port_id(key)` | Resolve an interface name or PCIe address to a port ID. |
| `get_num_rx_queues(port_id)` | Return configured RX queue count for a port. |
| `drop_all_traffic(port)` | Install a high-priority drop rule on a port. |
| `allow_all_traffic(port)` | Remove a drop rule installed by `drop_all_traffic`. |
| `flush_port_queue(port, queue)` | Drain stale packets from a port queue. |
| `add_rx_flow_async(port, flow)` | Return `(Status, op_id)` after enqueueing one dynamic RX flow create. |
| `add_rx_flows_async(port, flows)` | Return `(Status, op_id)` after enqueueing a dynamic RX flow batch create. One completion returns `flow_ids` in input order. |
| `delete_flow_async(flow_id)` | Return `(Status, op_id)` after enqueueing deletion of one dynamic flow. |
| `poll_flow_op()` | Return `(Status, FlowOpResult)`, or `NOT_READY` when no dynamic flow operation has completed. |
| `socket_connect_to_server(server_addr, server_port[, src_addr])` | Return `(Status, conn_id)`. |
| `socket_get_port_queue(conn_id)` | Return `(Status, port, queue)`. |
| `socket_get_server_conn_id(server_addr, server_port)` | Return `(Status, conn_id)`. |
| `socket_setsockopt(conn_id, level, optname, value)` | Apply a Linux socket option to an existing TCP/UDP socket connection. |
| `rdma_connect_to_server(server_addr, server_port[, src_addr])` | Return `(Status, conn_id)`. |
| `rdma_get_port_queue(conn_id)` | Return `(Status, port, queue)`. |
| `rdma_get_server_conn_id(server_addr, server_port)` | Return `(Status, conn_id)`. |

Dynamic RX flows are RX-only in v1. The `action` attribute remains the single queue-action
shorthand; use ordered `actions` when a raw DPDK or raw ibverbs dynamic rule needs hardware
VLAN pop or VXLAN/GRE/NVGRE decapsulation before the final queue action. Static TX
encapsulation/push rules are configured in YAML under `tx.flows`.

## Constants

| Name | Description |
| --- | --- |
| `ADV_NETWORK_HEADER_SIZE_BYTES` | Advanced network header size constant. |
| `MAX_NUM_RX_QUEUES` | Maximum configured RX queues. |
| `MAX_NUM_TX_QUEUES` | Maximum configured TX queues. |
| `MAX_INTERFACES` | Maximum configured interfaces. |
| `MAX_NUM_SEGS` | Maximum packet segments per burst. |
| `DAQIRI_VERSION` | DAQIRI package version as `YYYY.MM.PATCH`. |
| `DAQIRI_VERSION_YEAR` / `DAQIRI_VERSION_MONTH` / `DAQIRI_VERSION_PATCH` | CalVer components. |
| `DAQIRI_ABI_VERSION` | DAQIRI shared-library ABI version. |
| `DAQIRI_BURST_FLAG_REORDERED` | Burst flag indicating a reordered aggregate. |
| `DAQIRI_BURST_FLAG_REORDER_TIMEOUT` | Burst flag indicating a reorder timeout aggregate. |
| `MEM_ACCESS_LOCAL` | Local memory access flag. |
| `MEM_ACCESS_RDMA_WRITE` | RDMA write memory access flag. |
| `MEM_ACCESS_RDMA_READ` | RDMA read memory access flag. |

## Enums

| Enum | Values |
| --- | --- |
| `Status` | `SUCCESS`, `NULL_PTR`, `NO_FREE_BURST_BUFFERS`, `NO_FREE_PACKET_BUFFERS`, `NOT_READY`, `INVALID_PARAMETER`, `NO_SPACE_AVAILABLE`, `NOT_SUPPORTED`, `GENERIC_FAILURE`, `CONNECT_FAILURE`, `INTERNAL_ERROR` |
| `RDMAOpCode` | `CONNECT`, `SEND`, `RECEIVE`, `RDMA_WRITE`, `RDMA_WRITE_IMM`, `RDMA_READ`, `RDMA_READ_IMM`, `INVALID` |
| `RDMACompletionType` | `RX`, `TX`, `INVALID` |
| `EngineType` | `UNKNOWN`, `DEFAULT`, `DPDK`, `SOCKET`, `RDMA` |
| `Direction` | `RX`, `TX`, `TX_RX` |
| `BufferLocation` | `CPU`, `GPU`, `CPU_GPU_SPLIT` |
| `MemoryKind` | `HOST`, `HOST_PINNED`, `HUGE`, `DEVICE`, `INVALID` |
| `StreamType` | `RAW`, `SOCKET`, `INVALID`, `PCIE` |
| `LoopbackType` | `DISABLED`, `LOOPBACK_TYPE_SW` |
| `RDMAMode` | `CLIENT`, `SERVER`, `INVALID` |
| `RDMATransportMode` | `RC`, `UC`, `UD`, `INVALID` |
| `SocketMode` | `CLIENT`, `SERVER`, `INVALID` |
| `FlowType` | `QUEUE`, `VLAN_PUSH`, `VLAN_POP`, `TUNNEL_ENCAP`, `TUNNEL_DECAP` |
| `TunnelType` | `NONE`, `VXLAN`, `GRE`, `NVGRE` |
| `FlowMatchType` | `IPV4_UDP`, `FLEX_ITEM` |
| `FlowOpType` | `ADD_RX`, `ADD_RX_BATCH`, `DELETE` |
| `ReorderMethod` | `INVALID`, `SEQ_BATCH_NUMBER`, `SEQ_PACKETS_PER_BATCH` |
| `ReorderDataType` | `SAME`, `INT4`, `INT8`, `INT16`, `INT32`, `FP16`, `BF16`, `FP32`, `FP64`, `INVALID` |
| `ReorderEndianness` | `HOST`, `NETWORK`, `INVALID` |
| `LogLevel` | `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL`, `OFF` |
| `ErrorGlobalStats` | `OUT_OF_RX_BUFFERS`, `RX_QUEUE_FULL`, `METADATA_BUF_DEPLETED`, `SENTINEL` |

## Data and Config Classes

All exposed classes have a default constructor and read/write attributes.
Config classes mirror the C++ configuration structs, with Python attribute
names that mostly omit the trailing underscore from the C++ member name (e.g.
`name_` → `name`). A few fields are renamed for clarity — for example `mrs_`
→ `memory_regions` and `ifs_` → `interfaces`.

| Class | Purpose |
| --- | --- |
| `BurstParams` | Opaque burst handle used by RX, TX, free, and packet-access helpers. Exposes `hdr`, `connection_id`, and `rdma_wr_id`. |
| `BurstHeader` | Wrapper for `BurstHeaderParams`. |
| `BurstHeaderParams` | Burst metadata: packet count, port, queue, segment count, byte totals, and reorder flags. |
| `ReorderBurstInfo` | Metadata for reordered aggregate bursts. |
| `NetworkConfig` | Top-level parsed DAQIRI configuration. |
| `CommonConfig` | Global stream type, optional network-engine selection, direction, loopback, and core settings. PCIe rejects an explicit engine; socket transport protocol is derived from the endpoint URI scheme. |
| `InterfaceConfig` | Per-interface address, socket/RoCE/RDMA, RX, and TX configuration. |
| `RxConfig` | RX flow isolation, timestamps, queues, flows, flex items, and reorder configs. |
| `TxConfig` | TX accurate-send flag, queues, and flows. |
| `CommonQueueConfig` | Shared queue fields: name, ID, batch size, split boundary, CPU core, memory regions, and offloads. |
| `RxQueueConfig` | RX queue wrapper with common queue fields and timeout. |
| `TxQueueConfig` | TX queue wrapper with common queue fields. |
| `MemoryRegionConfig` | Memory region kind, affinity, access flags, sizes, counts, and ownership. |
| `VlanActionConfig` | VLAN push parameters: VLAN ID, priority, DEI, and ethertype. |
| `TunnelConfig` | VXLAN, GRE, or NVGRE tunnel template fields for hardware encap/decap actions. |
| `FlowAction` | Flow action type, queue target ID, optional VLAN config, and optional tunnel config. |
| `FlowMatch` | Flow match fields for UDP, IPv4, and flex item matching. |
| `FlowConfig` | Static named flow rule combining legacy `action`, ordered `actions`, and match fields. |
| `FlowRuleConfig` | Dynamic RX flow rule combining legacy `action`, ordered `actions`, and match fields. |
| `FlowOpResult` | Dynamic flow operation completion. Batch adds return `flow_ids` in input order. |
| `FlexItemConfig` | Flexible parser item configuration. |
| `FlexItemMatch` | Flexible parser match value and mask. |
| `SocketConfig` | Socket client/server endpoint URI, legacy IP/port, and timing settings. |
| `RoCEConfig` | RoCE transport settings. |
| `RDMAConfig` | RDMA mode, transport mode, and port. |
| `ReorderConfig` | Reorder name, type, memory region, payload offset, flows, method, and data type conversion. |
| `ReorderBitFieldConfig` | Bit offset and width for extracting reorder fields. |
| `ReorderSeqBatchNumberConfig` | Sequence-number, batch-number, and packets-per-batch field config. |
| `ReorderSeqPacketsPerBatchConfig` | Sequence-number and packets-per-batch field config. |
| `ReorderDataTypesConfig` | Optional reorder input/output data type conversion settings. |
