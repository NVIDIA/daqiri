---
hide:
  - navigation
---
# Understanding the Configuration File

## Choosing an example config

Read down the questions below and stop at the first one that matches what you're trying to do. Each section names the YAML, the binary that consumes it, and the build flags or hardware it requires. **Backend selection is a build-time choice via `DAQIRI_MGR`** — the default build enables all three backends (DPDK raw, kernel sockets, and RDMA).

??? question "1. I want to measure baseline throughput"
    Pick the backend that matches your stack, then the hardware or protocol variant.

    **DPDK raw** — runs on `daqiri_bench_raw_gpudirect`. Highest performance, kernel bypass; requires a Mellanox-class NIC.

    - **Generic discrete GPU** (template — replace `<placeholders>`) — [`daqiri_bench_raw_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx.yaml). This is the file annotated line-by-line in the [walkthrough below](#annotated-walkthrough).
    - **DGX Spark / GB10** (prefilled) — [`daqiri_bench_raw_tx_rx_spark.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_spark.yaml). `kind: host_pinned` for the integrated GPU; cores, PCIe addresses, and IPs are prefilled. See the [Spark profile callout](benchmarking_examples.md#update-the-loopback-configuration) for run details.
    - **No physical NIC available** — [`daqiri_bench_raw_sw_loopback.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_sw_loopback.yaml). `loopback: "sw"`, no NIC required. Useful for first-time build verification, not representative of production performance.

    **RDMA / RoCE** — runs on `daqiri_bench_rdma` (use `--mode {tx,rx,both}`). Low-latency interconnect; available in the default build (set `-DDAQIRI_MGR="dpdk socket rdma"` explicitly for clarity). Requires an RDMA-capable fabric. Configs use `kind: host_pinned` regardless of platform.

    - **Generic** (template — replace IPs) — [`daqiri_bench_rdma_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_rdma_tx_rx.yaml).
    - **DGX Spark** (prefilled) — [`daqiri_bench_rdma_tx_rx_spark.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_rdma_tx_rx_spark.yaml). See the [Spark profile callout](benchmarking_examples.md#update-the-loopback-configuration) for run details.

    **Kernel TCP/UDP sockets** — runs on `daqiri_bench_socket`. No NIC, no privileges, no special CMake flags. Useful as a comparison baseline against DPDK and RDMA. Both bind to `127.0.0.1`.

    - **UDP** — [`daqiri_bench_socket_udp_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_socket_udp_tx_rx.yaml).
    - **TCP** — [`daqiri_bench_socket_tcp_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_socket_tcp_tx_rx.yaml).

??? question "2. I have out-of-order UDP packets that need to be reordered on the GPU"
    DAQIRI's flagship pipeline: a CUDA kernel reads a sequence number from each packet's header and places packets at the correct offset in a GPU buffer, so a downstream consumer sees a fully ordered stream without a CPU touch. Configs run on `daqiri_bench_raw_reorder_seq` unless 2.4 applies. Sub-questions:

    **2.1 Which algorithm matches how your packets encode batches?**

    - *"My protocol sends a fixed N packets per logical batch; the seqno identifies position within the batch"* — `seq_packets_per_batch`.
    - *"My protocol identifies the batch index in the seqno; packets-per-batch is fixed at the protocol level"* — `seq_batch_number`.

    **2.2 Where should the reorder run?**

    - GPU kernel (default, recommended) — `reorder_type: "gpu"`.
    - CPU (throughput-bounded; comparison/baseline path) — `reorder_type: "cpu"`.

    **2.3 Self-contained, or do you have a TX peer?**

    - TX+RX — closed-loop in one process.
    - RX-only — you'll generate traffic separately. **A standalone run of any `raw_rx_*` config exits cleanly with `0` packets if no traffic arrives — that is not a bug; you need a TX peer.**

    **2.4 Do you also need an in-kernel payload type conversion?**

    - No — pick a leaf from the table below.
    - Yes — [`daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml) (runs on `daqiri_bench_raw_reorder_quantize`, not `daqiri_bench_raw_reorder_seq`). Combines `seq_batch_number` reorder with an in-kernel payload type conversion; the `data_types` block sets the input and output types (the example uses int4 → fp32). Pick this when wire format and compute format differ.

    Concrete leaves (without conversion):

    | YAML | Algorithm | Kernel | Direction |
    |---|---|---|---|
    | [`daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml) | `seq_packets_per_batch` (1024) | GPU | TX+RX |
    | [`daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml) | `seq_packets_per_batch` (1024) | CPU | TX+RX |
    | [`daqiri_bench_raw_rx_reorder_seq_ppb.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_rx_reorder_seq_ppb.yaml) | `seq_packets_per_batch` (128) | GPU | RX-only |
    | [`daqiri_bench_raw_rx_reorder_seq_batch.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_rx_reorder_seq_batch.yaml) | `seq_batch_number` | GPU | RX-only |
    | [`daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml) | `seq_packets_per_batch` (1024) | CPU | TX+RX, no NIC |

    *Requires: DPDK build + Mellanox-class NIC (or the SW-loopback variant for first-time validation).*

??? question "3. I need to parse small per-packet metadata on the CPU while keeping payload on the GPU"
    - [`daqiri_bench_raw_tx_rx_hds.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_hds.yaml) (runs on `daqiri_bench_raw_hds`).

    Header-data split: segment 0 (CPU) holds the header, segment 1 (GPU) holds the payload via GPUDirect zero-copy. Pick this when the CPU needs to read small per-packet fields without ever touching the payload.

    *Requires: DPDK build + Mellanox-class NIC.*

??? question "4. I need flow-based load balancing across multiple RX queues"
    - [`daqiri_bench_raw_rx_multi_q.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_rx_multi_q.yaml) (runs on `daqiri_bench_raw_gpudirect`).

    RX-only by design — drive traffic from a separate peer. Demonstrates flow-rule-based routing across multiple RX queues, each pinned to its own CPU core.

    *Requires: DPDK build + Mellanox-class NIC + a separate TX traffic source.*

??? question "5. I need to record packet data to disk"
    Sub-question: **which output format?**

    **5.1 Wireshark- / tcpdump-compatible PCAP** — runs on `daqiri_example_pcap_writer`. Default; works on any filesystem. Run shape: `daqiri_example_pcap_writer <yaml> <output.pcap> [--tx]` (omit `--tx` for an RX-only tcpdump-style capture).

    - **Hardware loopback** — [`daqiri_example_pcap_writer_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_pcap_writer_tx_rx.yaml).
    - **No physical NIC available** — [`daqiri_example_pcap_writer_sw_loopback.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_pcap_writer_sw_loopback.yaml).

    *Requires: DPDK build. No special CMake flag.*

    **5.2 Zero-copy GPU → NVMe writes** (advanced) — runs on `daqiri_example_gds_write`. Pick this *only* if the GPU-to-disk zero-copy path is the specific subject of investigation; otherwise pick PCAP (5.1).

    - **Hardware loopback** — [`daqiri_example_gds_write_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_gds_write_tx_rx.yaml).
    - **No physical NIC available** — [`daqiri_example_gds_write_sw_loopback.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_gds_write_sw_loopback.yaml).

    *Requires: built with `-DDAQIRI_ENABLE_GDS=ON`, NVMe-backed storage, working cuFile / `nvidia_fs` stack, `gdscheck.py -p` reports `NVMe : Supported`.*

## Annotated walkthrough

This section walks through the YAML configuration used by the benchmark applications. The annotated example below is based on [`daqiri_bench_raw_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx.yaml). Click on the :material-plus-circle: icons to expand explanations for each annotated line.

Annotations are prefixed with a category:

- :material-wrench:{ title="System-specific" } **System-specific** — must be changed for your hardware (these lines are highlighted in the code block below)
- :material-package-variant:{ title="Payload-dependent" } **Payload-dependent** — adjust based on your application's packet format and throughput needs

```yaml hl_lines="5 28 34 40 46 77 78 79"
daqiri: # (1)!
  cfg:
    version: 1
    manager: "dpdk" # (2)!
    master_core: 3 # (3)!
    debug: false
    log_level: "info"

    memory_regions: # (4)!
    - name: "Data_TX_GPU" # (5)!
      kind: "device" # (6)!
      affinity: 0 # (7)!
      num_bufs: 51200 # (8)!
      buf_size: 1064 # (9)!
    - name: "Data_RX_GPU"
      kind: "device"
      affinity: 0
      num_bufs: 51200
      buf_size: 1000
    - name: "Data_RX_CPU"
      kind: "huge"
      affinity: 0
      num_bufs: 51200
      buf_size: 64

    interfaces: # (10)!
    - name: "tx_port" # (11)!
      address: <0000:00:00.0> # (12)! # The BUS address of the interface doing Tx
      tx: # (13)!
        queues: # (14)!
        - name: "tx_q_0" # (15)!
          id: 0 # (16)!
          batch_size: 10240 # (17)!
          cpu_core: 11 # (18)!
          memory_regions: # (19)!
            - "Data_TX_GPU"
          offloads: # (20)!
            - "tx_eth_src"
    - name: "rx_port"
      address: <0000:00:00.0> # (21)! # The BUS address of the interface doing Rx
      rx:
        flow_isolation: true # (22)!
        queues:
        - name: "rx_q_0"
          id: 0
          cpu_core: 9
          batch_size: 10240
          memory_regions: # (23)!
            - "Data_RX_CPU"
            - "Data_RX_GPU"
        flows: # (24)!
        - name: "flow_0" # (25)!
          id: 0 # (26)!
          action: # (27)!
            type: queue
            id: 0
          match: # (28)!
            udp_src: 4096
            udp_dst: 4096
            ipv4_len: 1050

bench_rx: # (29)!
  interface_name: "rx_port" # Name of the RX port from the daqiri config
  gpu_direct: true          # Set to true if using a GPU region for the Rx queues.
  split_boundary: true      # Whether header and data are split for Rx (Header to CPU)
  batch_size: 10240
  max_packet_size: 1064
  header_size: 64

bench_tx: # (30)!
  interface_name: "tx_port" # Name of the TX port from the daqiri config
  gpu_direct: true          # Set to true if using a GPU region for the Tx queues.
  split_boundary: 0         # Byte boundary where header and data are split for Tx, 0 if no split
  batch_size: 10240
  payload_size: 1000
  header_size: 64
  eth_dst_addr: <00:00:00:00:00:00> # Destination MAC address - required when Rx flow_isolation=true
  ip_src_addr: <1.2.3.4>    # Source IP address - only required on layer 3 network
  ip_dst_addr: <5.6.7.8>    # Destination IP address - only required on layer 3 network
  udp_src_port: 4096        # UDP source port
  udp_dst_port: 4096        # UDP destination port
```

1. The `daqiri` section configures the DAQIRI library, which is responsible for setting up the NIC. It is passed to `daqiri_init(...)` during application startup.
2. `manager` is the backend networking library. Supported values: `dpdk`, `rdma`.
3. :material-wrench: `master_core` is the ID of the CPU core used for setup. It does not need to be isolated, and is recommended to differ from the `cpu_core` fields below used for polling the NIC.
4. The `memory_regions` section lists where the NIC will write/read data from/to when bypassing the OS kernel. Tip: when using GPU buffer regions, keeping the sum of their buffer sizes lower than 80% of your BAR1 size is generally a good rule of thumb.
5. A descriptive name for that memory region to refer to later in the `interfaces` section.
6. :material-package-variant: The type of memory region. On discrete GPUs, the best options are `device` (GPU VRAM, GPUDirect) or `huge` (hugepages, CPU). On integrated GPUs (e.g. NVIDIA GB10 / DGX Spark) where `device` cannot be peer-DMA'd by the NIC, use `host_pinned` instead. Also supported: `host` (CPU, unpinned). See the full [memory regions reference](../configuration.md#memory-regions). Choose based on whether your application processes packets on the GPU or CPU and on the GPU class.
7. :material-wrench: The GPU ID for `device` memory regions. The NUMA node ID for CPU memory regions.
8. :material-package-variant: The number of buffers in the memory region. A higher value means more time to process the data, but takes additional space on the GPU BAR1. Too low increases the risk of dropping packets from the NIC having nowhere to write (Rx) or higher latency from buffering (Tx). A good starting point is 3x-5x the `batch_size` below. For the DPDK backend, `num_bufs` below 1.5x the NIC ring size deadlocks the worker; `daqiri_init` auto-bumps such MRs to 3x the ring (24576 with the default 8192) and logs a `WARN`.
9. :material-package-variant: The size of each buffer in the memory region. These should be equal to your maximum packet size, or less if breaking down packets (e.g. header-data split, see the `rx` queue below).
10. The `interfaces` section lists the NIC interfaces that will be configured for the application.
11. A descriptive name for that interface, currently only used for logging.
12. :material-wrench: The PCIe/bus address of that interface, as identified in previous sections. **Must be changed for your system.**
13. Each interface can have a `tx` (transmitting) or `rx` (receiving) section, or both if you'd like to configure both Tx and Rx on the same interface.
14. The `queues` section lists the queues for that interface. Queues are a core concept of NICs: they handle the actual receiving or transmitting of network packets. Rx queues buffer incoming packets until they can be processed by the application, while Tx queues hold outgoing packets waiting to be sent on the network. The simplest setup uses only one receive and one transmit queue. Using more queues allows multiple streams of network traffic to be processed in parallel, as each queue can be assigned to a specific CPU core with its own memory regions.
15. A descriptive name for that queue, currently only used for logging.
16. The ID of that queue, which can be referred to later in the `flows` section.
17. :material-package-variant: The number of packets per batch (or burst). The Rx path delivers packets to the application in batches of this size. The Tx path should not send more packets than this value per call.
18. :material-wrench: The ID of the CPU core that this queue will use to poll the NIC. Ideally one [isolated core](system_configuration.md#step-5-isolate-cpu-cores) per queue. **Must match your system's available cores.**
19. The list of memory regions where this queue will write/read packets from/to. The order matters: the first memory region will be used first to read/write from until it fills up one buffer (`buf_size`), after which it will move to the next region in the list and so on until the packet is fully written/read. See the `memory_regions` for the `rx` queue below for an example.
20. The `offloads` section (Tx queues only) lists optional tasks that can be offloaded to the NIC. The only value currently supported is `tx_eth_src`, which lets the NIC insert the ethernet source MAC address in the packet headers. Note: IP, UDP, and Ethernet checksums/CRC are always done by the NIC and are not optional.
21. :material-wrench: Same as for `tx_port`. Each interface in this list should have a unique MAC address. **Must be changed for your system.**
22. Whether to isolate the Rx flow. If true, any incoming packets that do not match the MAC address of this interface — or are not directed to a queue via the `flows` section below — will be delegated back to Linux for processing (no kernel bypass). This is useful to let this interface handle ARP, ICMP, etc. Otherwise, any packets sent to this interface (e.g. ping) will need to be processed (or dropped) by your application.
23. :material-package-variant: This scenario is called HDS (Header-Data Split): the packet will first be written to a buffer in the `Data_RX_CPU` memory region, filling its `buf_size` of 64 bytes — which is consistent with the size of our header — then the rest of the packet will be written to the `Data_RX_GPU` memory region. Its `buf_size` of 1000 bytes is just what we need for the payload. Adjust the region list and sizes based on your packet structure.
24. The list of flows. Flows are responsible for routing packets to the correct queue based on various properties. If this field is missing, all packets will be routed to the first queue.
25. The flow name, currently only used for logging.
26. The flow `id` is used to tag the packets with what flow it arrived on. This is useful when sending multiple flows to a single queue, as the application can differentiate which flow (i.e. rules) matched the packet based on this ID.
27. What to do with packets that match this flow. The only supported action currently is `type: queue` to send the packet to a queue given its `id`.
28. :material-package-variant: List of rules to match packets against. All rules must be met for a packet to match the flow. Currently supported rules include `udp_src` and `udp_dst` (port numbers) and `ipv4_len` (IP packet length). **Adjust to match your incoming traffic.**
29. :material-package-variant: The `bench_rx` section is specific to the benchmark application (`default_bench.cpp`). It configures the Rx side: which interface to receive on, whether GPUDirect and header-data split are enabled, and the expected packet geometry. These parameters should align with how `memory_regions` and `queues` were configured for the `rx` interface above.
30. :material-package-variant: The `bench_tx` section is specific to the benchmark application (`default_bench.cpp`). It configures the Tx side: which interface to transmit on, packet sizes, and the Ethernet/IP/UDP header fields to embed in outgoing packets. The `eth_dst_addr` is required when `flow_isolation` is enabled on the Rx interface. The `ip_src_addr`/`ip_dst_addr` are only needed when traffic is routed across subnets — for a direct cable loopback, any value works. The `payload_size`, `header_size`, and UDP ports should match your application's packet format.

---
**Previous:** [Benchmarking Examples](benchmarking_examples.md)
