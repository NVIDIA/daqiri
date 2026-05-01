---
hide:
  - navigation
---
# Understanding the Configuration File

This section walks through the YAML configuration used by the benchmark applications. The annotated example below is based on `daqiri_bench_raw_tx_rx.yaml`. Click on the :material-plus-circle: icons to expand explanations for each annotated line.

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
6. :material-package-variant: The type of memory region. Best options are `device` (GPU) or `huge` (hugepages - CPU). Also supported: `host_pinned` (CPU, pinned) and `host` (CPU, unpinned). Choose based on whether your application processes packets on the GPU or CPU.
7. :material-wrench: The GPU ID for `device` memory regions. The NUMA node ID for CPU memory regions.
8. :material-package-variant: The number of buffers in the memory region. A higher value means more time to process the data, but takes additional space on the GPU BAR1. Too low increases the risk of dropping packets from the NIC having nowhere to write (Rx) or higher latency from buffering (Tx). A good starting point is 5x the `batch_size` below.
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
