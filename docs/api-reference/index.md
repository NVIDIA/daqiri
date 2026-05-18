# API Guide

DAQIRI applications start with a configuration file and then use a language API to
operate on the resources that configuration creates. The configuration defines the
network backend, interfaces, RX and TX queues, memory regions, flow steering rules,
flow isolation, header-data split, and optional reorder plans. After initialization,
the C++ API works with those configured ports, queues, buffers, and flows.

For the full configuration schema, see the
[Configuration YAML Reference](configuration.md). For an annotated end-to-end
example, see the
[configuration walkthrough tutorial](../tutorials/configuration-walkthrough.md).

## Application Model

The typical DAQIRI application lifecycle is:

1. Write or select a YAML configuration for the target system.
2. Initialize DAQIRI from that configuration.
3. Receive or transmit packet bursts through configured queues.
4. Access packet data through `BurstParams` helper functions.
5. Explicitly release packet and burst buffers when the application is done with them.
6. Shut down DAQIRI before process exit.

The language APIs do not discover queues, memory, or flow steering rules on their own.
They are runtime handles over the topology declared in YAML. Keep the configuration as
the source of truth for queue IDs, memory placement, protocol/backend selection, and
flow routing.

## Key Concepts

### Configuration First

The YAML file describes the data path that DAQIRI builds. It controls which backend is
active, which NICs or endpoints are used, how RX/TX queues are assigned to CPU cores,
where packet buffers live, which flows are accepted, and whether features such as
header-data split, hardware timestamps, accurate send, RDMA, socket transport, or RX
reorder aggregation are enabled.

The C++ API initializes from this configuration, then exposes operations on the
configured data path.

### BurstParams

All packet data flows through `BurstParams`. A burst is a batch of packets grouped
together for efficient transfer between DAQIRI internals and the application.

`BurstParams` carries packet-buffer pointers, packet count, port and queue IDs, segment
count, byte totals, per-packet lengths, flow IDs, and optional RX hardware timestamps.
Treat it as an opaque handle and use the language API helper functions to inspect or
modify it.

### Zero-Copy Ownership

DAQIRI is designed around zero-copy packet delivery. When a receive API returns packet
data, the application is reading the buffers that DAQIRI received from the backend; the
API passes pointers and metadata instead of copying packet contents into new
application-owned buffers.

That ownership model makes buffer release part of the API contract. Applications must
free RX bursts after processing and free or send TX bursts after allocation. Holding
bursts indefinitely drains DAQIRI's buffer pools and can lead to
`NO_FREE_BURST_BUFFERS`, `NO_FREE_PACKET_BUFFERS`, queue drops, or stalled TX.

### Segments and Header-Data Split

A segment is one contiguous memory region that contains part of a packet. A packet can
have one segment or multiple segments. Header-data split (HDS) commonly uses segment 0
for packet headers in CPU memory and segment 1 for payload data in GPU memory, allowing
the payload path to remain zero-copy for GPU workloads.

Single-segment configurations are used for CPU- or GPU-only paths that do not split
headers from payloads.

### RX GPU Packet Aggregation and Reorder

DAQIRI offers options for packet aggregation or reorder:

- GPU packet aggregation helpers, such as the C++ `simple_packet_reorder` kernel, copy
  scattered packet buffers into a contiguous GPU output buffer after a burst has been
  received.
- RX reorder aggregation is configured in YAML with `rx.reorder_configs`. It groups
  packets into reordered aggregate bursts before the application consumes them. See the
  [Configuration YAML Reference](configuration.md#rx-reorder-configs-dpdk-v1) for the
  constraints and the language API pages for consuming reordered bursts.

## C++ API

Use the [C++ API Usage](cpp.md) page for C++ initialization, RX/TX workflows, buffer
lifecycle calls, file writing and GDS APIs, utility helpers, and status codes.
