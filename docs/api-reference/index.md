---
hide:
  - navigation
---

# API Guide

DAQIRI is a library that moves bursts of packets between NICs and CPU or
GPU memory with zero copies, configured from a YAML file or an equivalent
C++ `NetworkConfig` struct.

This page is the orientation for the API: it covers DAQIRI's
configuration-first model and the lifecycle every application follows.
For the terminology and conceptual background it relies on
(*kernel bypass*, *GPUDirect*, *burst*, *segment*, *flow*, *queue*,
*memory region*, *zero-copy ownership*, *RX reorder*), keep the
[Concepts](../concepts.md) page open in a second tab.

## Configuration-First Model

A DAQIRI application starts from a YAML configuration file (or an
equivalent `NetworkConfig` struct built in code). The configuration
defines the active stream type, optional engine, endpoint URIs, NIC interfaces, RX and TX
queues, memory regions, flow steering rules, flow isolation,
header-data split, and optional reorder plans. After initialization,
the language API operates on those configured ports, queues, buffers,
and flows.

The language APIs do **not** discover queues, memory, or flow steering
rules on their own. They are runtime handles over the topology declared
in the configuration (YAML file or `NetworkConfig` struct). The
configuration is the source of truth for queue IDs, memory placement,
stream-type / engine / endpoint selection, and flow routing.

The configuration schema lives in the
[Configuration YAML Reference](configuration.md). For an annotated
end-to-end example, see the
[configuration walkthrough tutorial](../tutorials/configuration-walkthrough.md).

## Application Model

The typical DAQIRI application lifecycle has six steps:

1. **Write or select a YAML configuration** for the target system.
2. **Initialize DAQIRI** from that configuration (`daqiri_init`).
3. **Receive or transmit packet bursts** through configured queues
   (`get_rx_burst` / `get_tx_packet_burst` + `send_tx_burst`).
4. **Access packet data** through `BurstParams` helper functions
   (`get_packet_ptr`, `get_segment_packet_ptr`, ...).
5. **Explicitly release packet and burst buffers** when the
   application is done with them (the `free_*` family).
6. **Shut down DAQIRI** before process exit (`shutdown`).

Each step maps directly to a section of the
[C++ API Usage](cpp.md) and [Python API Usage](python.md) pages. The
buffer-release step in particular is load-bearing — see
[Zero-Copy Ownership](../concepts.md#zero-copy-ownership) on the
Concepts page for why missed frees cause queue drops.

## See also

- [Concepts](../concepts.md) — terminology and background for everything
  this page references.
- [Configuration YAML Reference](configuration.md) — every YAML key, its
  type, and its valid values.
- [C++ API Usage](cpp.md) — initialization, RX/TX workflows, buffer
  lifecycle, file writing, utilities, and the full C++ function reference.
- [Python API Usage](python.md) — the same workflow through the pybind11
  bindings, including GIL behavior, tuple return shapes, and the full
  Python function reference.
- [Configuration walkthrough tutorial](../tutorials/configuration-walkthrough.md)
  — annotated YAML walkthrough with a use-case decision tree.
