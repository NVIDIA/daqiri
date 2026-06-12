---
hide:
  - navigation
---

# Understanding the Configuration File

## Choosing an example config

### Choosing the appropriate DAQIRI stream type for your setup

DAQIRI exposes a single API on top of multiple packet I/O stacks, selected at runtime with `stream_type` and endpoint URI schemes such as `udp://`, `tcp://`, and `roce://`. Pick the row that matches your hardware and the role of the other endpoint:

- **Raw Ethernet** — `stream_type: "raw"`. Kernel-bypass with GPUDirect zero-copy. Highest performance. Requires an [NVIDIA ConnectX-class NIC](https://www.nvidia.com/en-us/networking/ethernet-adapters/); `tx_port` and `rx_port` can share one physical NIC for a single-host closed-loop bench, or be split across two hosts.
- **Socket — UDP / TCP** — `stream_type: "socket"` with `udp://` or `tcp://` endpoints. Plain Linux kernel sockets. No NIC, no privileges, no special CMake flags. Useful as a comparison baseline and as a path to first results on a system without an NVIDIA NIC.
- **Socket — RoCE (RDMA)** — `stream_type: "socket"` and `roce://` endpoints. RDMA verbs over Ethernet, with a server/client connection model and a NIC-level reliable transport. Primarily intended for setups where **one** endpoint is a third-party RoCE implementation (FPGA, instrument, customer black box). When both peers run DAQIRI, prefer an upper-layer library such as MPI / NCCL / UCX instead.

If you don't have any NIC at all, the `*_sw_loopback*` variants of the Raw Ethernet configs need no hardware — useful for first-time build verification.

(`DAQIRI_ENGINE` at the CMake layer selects which optional engine implementations to compile in — `dpdk` enables `stream_type: "raw"`, and `ibverbs` enables `roce://` endpoints. Linux UDP/TCP sockets are always built in. The default build is `dpdk ibverbs`.)

For a shorter selection guide, start with the [Benchmarking overview](../benchmarks/benchmarks.md). With a stream type in mind, read down the questions below and stop at the first one that matches what you're trying to do. Each section names the YAML, the binary that consumes it, and any platform-specific notes.

??? question "1. I want to measure baseline throughput"
    Pick the stream type that matches your stack (see the [overview](#choosing-the-appropriate-daqiri-stream-type-for-your-setup) above), then the hardware or transport variant.

    **Raw Ethernet** (`stream_type: "raw"`) — runs on `daqiri_bench_raw_gpudirect`.

    - **Generic discrete GPU** (template — replace `<placeholders>`) — [`daqiri_bench_raw_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx.yaml). This is the file annotated line-by-line in the [walkthrough below](#annotated-walkthrough).
    - **Four queue closed-loop TX+RX** (template — replace `<placeholders>`) — [`daqiri_bench_raw_tx_rx_4q.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_4q.yaml). Uses one application worker per TX/RX queue, with each `bench_tx` entry sending a different UDP flow.
    - **DGX Spark / GB10** (prefilled) — [`daqiri_bench_raw_tx_rx_spark.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_spark.yaml). `kind: host_pinned` for the integrated GPU; cores, PCIe addresses, and IPs are prefilled. See the [Spark profile callout](../benchmarks/raw_benchmarking.md#update-the-loopback-configuration) for run details.
    - **DGX Spark multi-queue core-scaling matrix** (prefilled) — one base config [`daqiri_bench_raw_tx_rx_spark_mq.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_spark_mq.yaml) (the balanced TX=2/RX=2 superset; cores TX → 16,17, RX → 18,19) from which `examples/run_spark_mq_bench.sh` (via `scripts/gen_spark_mq_config.py`) derives the four `(TX, RX)` cells — (1,1), (1,2) (RX scaling), (2,1) (TX scaling), (2,2) (balanced) — by pruning queues/flows. All run on `daqiri_bench_raw_gpudirect` at the native 8 KB shape.
    - **DGX Spark cross-host** (prefilled, runs on two Sparks) — [`daqiri_bench_raw_tx_spark_xhost.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_spark_xhost.yaml) on the TX host and [`daqiri_bench_raw_rx_spark_xhost.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_rx_spark_xhost.yaml) on the RX host. Each host runs `daqiri_bench_raw_gpudirect` against its own half; cables connect p0↔p0 between the two boxes. See the [Cross-host two-DGX-Spark loopback](../benchmarks/raw_benchmarking.md#cross-host-two-dgx-spark-loopback) section for run details.
    - **No physical NIC available** — [`daqiri_bench_raw_sw_loopback.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_sw_loopback.yaml). `loopback: "sw"`, no NIC required. Useful for first-time build verification, not representative of production performance.

    To watch the same raw loopback benchmark with live Prometheus and Grafana
    counters, use the Grafana compose stack described in
    [Watch live OpenTelemetry metrics in Grafana](../benchmarks/raw_benchmarking.md#watch-live-opentelemetry-metrics-in-grafana).

    **Socket — RoCE (RDMA)** (`stream_type: "socket"`, `roce://` endpoints) — runs on `daqiri_bench_rdma` (use `--mode {tx,rx,both}`). Configs use `kind: host_pinned` regardless of platform.

    - **Generic** (template — replace IPs) — [`daqiri_bench_rdma_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_rdma_tx_rx.yaml).
    - **DGX Spark** (prefilled) — [`daqiri_bench_rdma_tx_rx_spark.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_rdma_tx_rx_spark.yaml). See [Socket and RDMA Benchmarking](../benchmarks/socket_benchmarking.md#run-the-rdma-roce-benchmark) for namespace and wire-counter run details.
    - **DGX Spark netns wire loopback** (prefilled, combined base) — [`daqiri_bench_rdma_tx_rx_spark_netns.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_rdma_tx_rx_spark_netns.yaml). Carries both roles; `examples/run_spark_bench.sh` (via `scripts/gen_spark_netns_config.py`) splits it per role and runs each in its own network namespace (`--mode server` / `--mode client`) so RDMA-CM resolves over the wire; see [Socket and RDMA Benchmarking](../benchmarks/socket_benchmarking.md#run-the-rdma-roce-benchmark).
    - **DGX Spark cross-host** (prefilled, runs on two Sparks) — [`daqiri_bench_rdma_tx_rx_spark_xhost.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_rdma_tx_rx_spark_xhost.yaml). Run with `--mode server` on the RX host and `--mode client` on the TX host. See the [Cross-host two-DGX-Spark loopback](../benchmarks/raw_benchmarking.md#cross-host-two-dgx-spark-loopback) section for run details.

    **Socket — UDP / TCP** (`stream_type: "socket"` with `udp://` or `tcp://` endpoints) — runs on `daqiri_bench_socket`. The shipped smoke-test configs bind to `127.0.0.1`; see [Socket and RDMA Benchmarking](../benchmarks/socket_benchmarking.md) for namespace-based wire tests.

    - **UDP** — [`daqiri_bench_socket_udp_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_socket_udp_tx_rx.yaml).
    - **UDP — DGX Spark netns wire loopback** (combined base) — [`daqiri_bench_socket_udp_tx_rx_spark_netns.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_socket_udp_tx_rx_spark_netns.yaml). Carries both roles; `examples/run_spark_bench.sh` (via `scripts/gen_spark_netns_config.py`) splits it per role and runs each in its own network namespace (`--mode server` / `--mode client`) so same-host IPs cross the wire instead of looping through `lo`; see [Socket and RDMA Benchmarking](../benchmarks/socket_benchmarking.md#run-the-linux-socket-benchmark).
    - **TCP** — [`daqiri_bench_socket_tcp_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_socket_tcp_tx_rx.yaml).
    - **TCP — DGX Spark netns wire loopback** (combined base) — [`daqiri_bench_socket_tcp_tx_rx_spark_netns.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_socket_tcp_tx_rx_spark_netns.yaml). Carries both roles; `examples/run_spark_bench.sh` (via `scripts/gen_spark_netns_config.py`) splits it per role and runs each in its own network namespace (`--mode server` / `--mode client`); see [Socket and RDMA Benchmarking](../benchmarks/socket_benchmarking.md#run-the-linux-socket-benchmark).

??? question "2. I have out-of-order UDP packets that need to be reordered on the GPU"
    DAQIRI's flagship pipeline: a CUDA kernel reads a sequence number from each packet's header and places packets at the correct offset in a GPU buffer, so a downstream consumer sees a fully ordered stream without a CPU touch. Configs run on `daqiri_bench_raw_reorder_seq` unless 2.4 applies. Sub-questions:

    **2.1 Which algorithm matches how your packets encode batches?**

    - *"My wire format sends a fixed N packets per logical batch; the seqno identifies position within the batch"* — `seq_packets_per_batch`.
    - *"My wire format identifies the batch index in the seqno; packets-per-batch is fixed for the stream"* — `seq_batch_number`.

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

    *Requires: Raw Ethernet build (`DAQIRI_ENGINE` includes `dpdk`) + NVIDIA ConnectX-class NIC (or the SW-loopback variant for first-time validation).*

    A [diff-style walkthrough](#packet-reordering-on-the-gpu) of `daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml` appears below.

??? question "3. I need to parse small per-packet metadata on the CPU while keeping payload on the GPU"
    - [`daqiri_bench_raw_tx_rx_hds.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_hds.yaml) (runs on `daqiri_bench_raw_hds`).

    Header-data split: segment 0 (CPU) holds the header, segment 1 (GPU) holds the payload via GPUDirect zero-copy. Pick this when the CPU needs to read small per-packet fields without ever touching the payload.

    *Requires: Raw Ethernet build (`DAQIRI_ENGINE` includes `dpdk`) + NVIDIA ConnectX-class NIC.*

    A [diff-style walkthrough](#header-data-split-hds) of this config appears below.

??? question "4. I need flow-based load balancing across multiple RX queues"
    - **Closed-loop TX+RX with four queues** — [`daqiri_bench_raw_tx_rx_4q.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_4q.yaml) (runs on `daqiri_bench_raw_gpudirect`).
    - [`daqiri_bench_raw_rx_multi_q.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_rx_multi_q.yaml) (runs on `daqiri_bench_raw_gpudirect`).

    The four-queue TX+RX config is self-contained and maps each `bench_tx`/`bench_rx` list entry to the matching DAQIRI queue. The RX-only config is for an external traffic source. Both demonstrate flow-rule-based routing across multiple RX queues, with explicit CPU cores for both DAQIRI queue workers and benchmark application workers.

    *Requires: Raw Ethernet build (`DAQIRI_ENGINE` includes `dpdk`) + NVIDIA ConnectX-class NIC. The RX-only config also requires a separate TX traffic source.*

??? question "5. I need to record packet data to disk"
    Sub-question: **which output format?**

    **5.1 Wireshark- / tcpdump-compatible PCAP** — runs on `daqiri_example_pcap_writer`. Default; works on any filesystem. Run shape: `daqiri_example_pcap_writer <yaml> <output.pcap> [--tx]` (omit `--tx` for an RX-only tcpdump-style capture).

    - **Hardware loopback** — [`daqiri_example_pcap_writer_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_pcap_writer_tx_rx.yaml).
    - **No physical NIC available** — [`daqiri_example_pcap_writer_sw_loopback.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_pcap_writer_sw_loopback.yaml).

    *Requires: Raw Ethernet build (`DAQIRI_ENGINE` includes `dpdk`). No special CMake flag.*

    **5.2 Zero-copy GPU → NVMe writes** (advanced) — runs on `daqiri_example_gds_write`. Pick this *only* if the GPU-to-disk zero-copy path is the specific subject of investigation; otherwise pick PCAP (5.1).

    - **Hardware loopback** — [`daqiri_example_gds_write_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_gds_write_tx_rx.yaml).
    - **No physical NIC available** — [`daqiri_example_gds_write_sw_loopback.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_example_gds_write_sw_loopback.yaml).

    *Requires: built with `-DDAQIRI_ENABLE_GDS=ON`, NVMe-backed storage, working cuFile / `nvidia_fs` stack, `gdscheck.py -p` reports `NVMe : Supported`.*

## Annotated walkthrough

This section walks through three YAML configurations: the base TX+RX template, followed by diff-style snippets for header-data split (HDS) and GPU packet reordering. Click on the :material-plus-circle: icons to expand explanations for each annotated line.

Annotations are prefixed with a category icon when applicable:

- :material-wrench:{ title="System-specific" } **System-specific** — must be changed for your hardware
- :material-package-variant:{ title="Payload-dependent" } **Payload-dependent** — adjust based on your application's packet format and throughput needs

In each code block, the lines you're most likely to tune are highlighted: system-specific addresses, cores, and MAC/IPs in the base walkthrough; feature-defining values (split boundaries, batch sizes, sequence-number positions) in the HDS and reorder diff snippets below.

### Base TX+RX config

The annotated example below is [`daqiri_bench_raw_tx_rx.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx.yaml).

```yaml hl_lines="5 24 30 36 42 58 62 66 67 68"
daqiri: # (1)!
  cfg:
    version: 1
    stream_type: "raw" # (2)!
    master_core: 3 # (3)!
    debug: false
    log_level: "info"
    loopback: "" # (4)!

    memory_regions: # (5)!
    - name: "Data_TX_GPU"
      kind: "device" # (6)!
      affinity: 0 # (7)!
      num_bufs: 51200 # (8)!
      buf_size: 8064 # (9)!
    - name: "Data_RX_GPU"
      kind: "device"
      affinity: 0
      num_bufs: 51200
      buf_size: 8064

    interfaces: # (10)!
    - name: "tx_port"
      address: <0000:00:00.0> # (11)!
      tx: # (12)!
        queues: # (13)!
        - name: "tx_q_0"
          id: 0
          batch_size: 10240 # (14)!
          cpu_core: 11 # (15)!
          memory_regions: # (16)!
            - "Data_TX_GPU"
          offloads: # (17)!
            - "tx_eth_src"
    - name: "rx_port"
      address: <0000:00:00.0> # (18)!
      rx:
        flow_isolation: true # (19)!
        queues:
        - name: "rq_q_0"
          id: 0
          cpu_core: 9
          batch_size: 10240
          memory_regions:
            - "Data_RX_GPU"
        flows: # (20)!
        - name: "flow_0"
          id: 0 # (21)!
          action: # (22)!
            type: queue
            id: 0
          match: # (23)!
            udp_src: 4096
            udp_dst: 4096

bench_rx: # (24)!
- interface_name: "rx_port"
  cpu_core: 8

bench_tx: # (25)!
- interface_name: "tx_port"
  cpu_core: 10
  batch_size: 10240
  payload_size: 8000
  header_size: 64
  eth_dst_addr: <00:00:00:00:00:00>
  ip_src_addr: <1.2.3.4>
  ip_dst_addr: <5.6.7.8>
  udp_src_port: 4096
  udp_dst_port: 4096
```

1. The `daqiri` section configures the DAQIRI library, which is responsible for setting up the NIC. It is passed to `daqiri_init(...)` during application startup. Within this section, `name:` fields on interfaces, queues, flows, and memory regions are used only for logging — pick any descriptive string.
2. **`stream_type`** · `string` · *required* — High-level transport family selected for this config. **Supported:** `"raw"` (Raw Ethernet via kernel bypass, used here), `"socket"` (kernel sockets and RDMA). Use endpoint URI schemes such as `tcp://`, `udp://`, and `roce://` for socket-style transports.
3. :material-wrench: **`master_core`** · `integer (CPU core ID)` · *required* — Core used for DAQIRI setup. Does not need to be isolated; recommended to differ from the `cpu_core` fields below that poll the NIC.
4. **`loopback`** · `string` · *default: `""`* — Loopback mode. **Supported:** `""` (no loopback; use the physical NIC), `"sw"` (software loopback — no NIC required, used by the `*_sw_loopback*` configs for first-time build verification).
5. The `memory_regions` section lists where the NIC will write/read data from/to when bypassing the OS kernel. Tip: when using GPU buffer regions, keeping the sum of their buffer sizes below 80% of your BAR1 size is generally a good rule of thumb.
6. :material-package-variant: **`kind`** · `string` · *required* — Type of memory backing the region. **Supported:** `device` (GPU VRAM via GPUDirect — preferred on discrete GPUs), `host_pinned` (CPU pinned memory — required on integrated GPUs like NVIDIA GB10/DGX Spark where peer-DMA isn't available), `huge` (hugepages, CPU), `host` (CPU unpinned). See the [memory regions reference](../api-reference/configuration.md#memory-regions). Choose based on whether packets are processed on the GPU or CPU and on the GPU class.
7. :material-wrench: **`affinity`** · `integer (GPU ID / NUMA node)` · *required* — GPU device ID when `kind: device` or `kind: host_pinned`; NUMA node ID for CPU memory regions (`huge`, `host`).
8. :material-package-variant: **`num_bufs`** · `integer` · *required* — Number of buffers in the region. Higher gives more time to process packets but uses more BAR1 space; too low risks NIC drops (RX) or buffering latency (TX). A good starting point is 3×–5× the queue `batch_size`. For Raw Ethernet (`stream_type: "raw"`), `num_bufs` below 1.5× the NIC ring size deadlocks the worker; `daqiri_init` auto-bumps such regions to 3× the ring (24576 with the default 8192) and logs a `WARN`.
9. :material-package-variant: **`buf_size`** · `integer (bytes)` · *required* — Size of each buffer in the region. Should equal your maximum packet size, or smaller when chaining regions per packet (e.g. header-data split — see the [HDS walkthrough](#header-data-split-hds) below).
10. The `interfaces` section lists the NIC interfaces that will be configured for the application.
11. :material-wrench: **`address`** · `string (PCIe BDF)` · *required* — PCIe bus address of this interface. **Must be changed for your system.** Both `tx_port` and `rx_port` may point to the same physical NIC for single-port closed-loop benches.
12. Each interface declares a `tx` (transmitting) and/or `rx` (receiving) section. Include only the side you're using on that port, or both if a single port carries traffic in both directions.
13. The `queues` section lists per-direction queues. Queues are a core NIC concept: they handle the actual reception or transmission of packets. RX queues buffer incoming packets until the application processes them; TX queues hold outgoing packets waiting to be sent. The simplest setup uses one RX and one TX queue; using more queues allows parallel streams (each queue can be pinned to its own CPU core and memory region).
14. :material-package-variant: **`batch_size`** · `integer (packets)` · *required* — Packets per burst. The RX path delivers packets to the application in batches of this size; the TX path should not send more packets than this per call.
15. :material-wrench: **`cpu_core`** · `integer (CPU core ID)` · *required* — Core that this queue uses to poll the NIC. Ideally one [isolated core](system_configuration.md#step-5-isolate-cpu-cores) per queue. **Must match your system's available cores.**
16. The list of memory regions where this queue will write/read packets. **Order matters:** the first region is used until one buffer fills (`buf_size`), then the next region is used, and so on until the packet is fully written/read. The [HDS walkthrough](#header-data-split-hds) below shows a chained example.
17. **`offloads`** · `list of string` · *TX queues only* — Optional tasks offloaded to the NIC. **Supported:** `tx_eth_src` (the NIC inserts the Ethernet source MAC into outgoing headers). `daqiri_init()` fails if the NIC cannot install the offload flow rule. Note: IP, UDP, and Ethernet checksums/CRC are always done by the NIC and are not optional.
18. :material-wrench: **`address`** · `string (PCIe BDF)` · *required* — PCIe bus address of the RX interface. May share the BDF with `tx_port` for single-port closed-loop benches, or be a different NIC for two-port setups. **Must be changed for your system.**
19. **`flow_isolation`** · `boolean` · *default: `false`* — When `true`, packets that don't match this interface's MAC or any rule under `flows` are delegated back to the Linux kernel (no kernel bypass). Useful for letting the interface still handle ARP, ICMP, etc. while DAQIRI takes the application packets. When `false`, every packet hitting the interface must be processed (or dropped) by your application.
20. The list of flows. Flows route packets to a queue based on packet fields. If `flows` is missing, all packets are routed to the first queue. For Raw Ethernet, each rule is programmed into the NIC during `daqiri_init()`; initialization fails if any rule or the send-to-kernel fallback (when `flow_isolation: true`) cannot be installed. Per interface, use only standard UDP/IP flows or only flex-item flows — not both.
21. **`id`** · `integer` · *required* — Tag attached to packets that match this flow. Useful when multiple flows route to a single queue and the application needs to distinguish which rule matched.
22. What to do with packets that match this flow. The only currently supported action is `type: queue` (send the packet to the queue with the given `id`). That `id` must match an `rx.queues` entry on this interface; `daqiri_init()` rejects unknown queue IDs during config validation.
23. :material-package-variant: List of rules to match packets against. **All** rules must hold for a packet to match the flow. Currently supported keys: `udp_src` / `udp_dst` (UDP source/destination port numbers, integer), `ipv4_len` (full IPv4 packet length in bytes, integer). **Adjust to match your incoming traffic.**
24. The `bench_rx` section is specific to the benchmark application. It is a list of application worker configs; list entries map to DAQIRI RX queues on the named interface, either by explicit `queue_id` or by queue-list order when `queue_id` is omitted. `cpu_core` pins the benchmark application's RX worker thread; it is separate from the DAQIRI queue `cpu_core` above, and can use the same core only when you intentionally want to share. In this base config there is one RX queue, so there is one entry. Other DAQIRI binaries (e.g. the reorder-quantize bench) may add fields here; see those configs for details.
25. :material-package-variant: The `bench_tx` section configures the TX side of the benchmark: the benchmark application's TX worker core, packet sizes, and the Ethernet/IP/UDP header fields embedded in outgoing packets. It is a list for the same reason as `bench_rx`: each entry maps to a DAQIRI TX queue on the named interface, either by explicit `queue_id` or by queue-list order when `queue_id` is omitted. The `cpu_core` field pins the application TX thread. The `eth_dst_addr` is required when `flow_isolation` is enabled on the RX interface. The `ip_src_addr` / `ip_dst_addr` are only needed when traffic is routed across subnets — for a direct cable loopback, any value works. The `payload_size`, `header_size`, and UDP ports should match your application's packet format.

### Header-data split (HDS)

For applications that parse small per-packet fields on the CPU while keeping the payload on the GPU, DAQIRI supports **header-data split (HDS)**: the NIC writes the header bytes to a CPU buffer (segment 0) and the payload to a GPU buffer (segment 1) using GPUDirect zero-copy. The packet is split at a fixed byte boundary defined by the `buf_size` of the first region in the queue's `memory_regions:` list.

The canonical HDS config is [`daqiri_bench_raw_tx_rx_hds.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_hds.yaml). It builds on the base TX+RX config above; only the deltas are shown here.

**New CPU memory regions, one per direction.** Headers land here.

```yaml hl_lines="6 11 16 21"
memory_regions:
- name: "Data_TX_CPU"   # (1)!
  kind: "huge"
  affinity: 0
  num_bufs: 51200
  buf_size: 64          # (2)!
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
  buf_size: 1000        # (3)!
```

1. New region for headers. `kind: "huge"` puts buffers in CPU hugepages so the CPU can read them without touching GPU memory. Pair `Data_TX_CPU` and `Data_RX_CPU` with their GPU siblings to chain regions per packet.
2. :material-package-variant: **`buf_size`** · `integer (bytes)` · *required* — In HDS, the **first region's `buf_size` is the split boundary**: bytes 0 to `buf_size − 1` go to the CPU region, the remainder spills into the next region. Size this to match your header length exactly (64 bytes for a typical Eth+IPv4+UDP header).
3. :material-package-variant: **`buf_size`** · `integer (bytes)` · *required* — Size of each GPU buffer in HDS mode: payload-only (no longer the full packet size). The packet first fills 64 bytes in `Data_RX_CPU`, then the remaining 1000 bytes spill into `Data_RX_GPU`.

**Chained `memory_regions:` per queue.** Order matters: header region first, payload region second. The NIC walks the list in order, filling each region's `buf_size` before moving on.

```yaml
tx:
  queues:
  - memory_regions:    # (1)!
      - "Data_TX_CPU"
      - "Data_TX_GPU"
rx:
  queues:
  - memory_regions:
      - "Data_RX_CPU"
      - "Data_RX_GPU"
```

1. Header region listed first. For each RX packet, 64 bytes land in `Data_RX_CPU`, then the next 1000 bytes land in `Data_RX_GPU`.

**Pin the packet length in the flow rule.** HDS triggers cleanly only when the full packet length is known up front, so the flow match adds `ipv4_len`:

```yaml hl_lines="5"
flows:
- match:
    udp_src: 4096
    udp_dst: 4096
    ipv4_len: 1050     # (1)!
```

1. :material-package-variant: **`ipv4_len`** · `integer (bytes)` · *optional in the base config; recommended for HDS* — Match on the full IPv4 packet length. Required for HDS so the NIC can split deterministically. Set to the fixed packet length you expect.

**Match `bench_tx.payload_size` to the GPU region.** The TX side generates 1000-byte payloads to match `Data_RX_GPU.buf_size`:

```yaml hl_lines="2 3"
bench_tx:
  payload_size: 1000
  header_size: 64
```

The HDS bench runs on `daqiri_bench_raw_hds`:

```bash
./build/examples/daqiri_bench_raw_hds ./build/examples/daqiri_bench_raw_tx_rx_hds.yaml --seconds 10
```

### Packet reordering on the GPU

For UDP workloads where packets arrive out-of-order and need to be placed at their correct offset in a GPU buffer before the application sees them, DAQIRI provides a **GPU reorder kernel**: the kernel reads a sequence number from each packet and writes the packet into a dedicated landing region at the correct slot. The downstream consumer reads a fully ordered batch with no CPU touch.

The canonical reorder config is [`daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml) (`seq_packets_per_batch` algorithm, GPU kernel, closed-loop TX+RX). It builds on the base TX+RX config above; only the deltas are shown here.

**New `Reorder_RX_GPU` memory region.** A large, dedicated GPU region that holds one fully reordered batch.

```yaml hl_lines="5 9 16"
memory_regions:
- name: "Data_TX_CPU"
  kind: "huge"
  num_bufs: 16384
  buf_size: 2048
- name: "Data_RX_GPU"
  kind: "device"
  num_bufs: 16384      # (1)!
  buf_size: 2048
- name: "Reorder_RX_GPU"  # (2)!
  kind: "device"
  affinity: 0
  num_bufs: 128
  # (source buf_size - payload_byte_offset) * packets_per_batch
  # = (2048 - 64) * 1024 = 2,031,616 bytes per reordered batch.
  buf_size: 2031616    # (3)!
```

1. :material-package-variant: **`num_bufs`** · `integer` · *required* — Shrunk from the base config's 51200 to 16384. Reorder works at smaller batches with smaller per-packet buffers (`buf_size: 2048` here vs. 8064 in the base), so the buffer pool is correspondingly smaller.
2. **New region for the kernel output.** Each buffer holds one fully reordered batch of `packets_per_batch` packets, payload-only (the header is stripped via `payload_byte_offset`).
3. :material-package-variant: **`buf_size`** · `integer (bytes)` · *required* — Sized as `(source buf_size − payload_byte_offset) × packets_per_batch`. For this config: `(2048 − 64) × 1024 = 2,031,616` bytes. Re-derive when you change any of the three inputs.

**Match queue `batch_size` to `packets_per_batch`.** The reorder kernel processes exactly one batch per invocation; the queue must hand it that many packets at once.

```yaml hl_lines="3 4"
rx:
  queues:
  - batch_size: 1024     # (1)!
    timeout_us: 2000     # (2)!
    memory_regions:
      - "Data_RX_GPU"
```

1. :material-package-variant: **`batch_size`** · `integer (packets)` · *required* — Must equal `packets_per_batch` in the reorder config below.
2. :material-package-variant: **`timeout_us`** · `integer (microseconds)` · *default: none (waits forever)* — Maximum time the queue waits for a partial batch to fill before flushing. Without it, a stalled flow can stall the reorder kernel indefinitely.

**Flow `id` tags packets for the reorder config.** The reorder block selects packets to reorder by flow ID.

```yaml hl_lines="3"
flows:
- name: "flow_0"
  id: 201               # (1)!
  action:
    type: queue
    id: 0
  match:
    udp_src: 5000
    udp_dst: 5000
```

1. **`id`** · `integer` · *required* — Flow tag attached to matching packets. Set to a non-zero value here so the `reorder_configs:` block below can reference it via `flow_ids:` to select which packets to reorder.

**The `reorder_configs:` block.** The core of the feature — sits inside the `rx:` section alongside `queues` and `flows`.

```yaml hl_lines="5 11 12 13"
reorder_configs:
- name: "rx_reorder_seq_1024"
  reorder_type: "gpu"           # (1)!
  memory_region: "Reorder_RX_GPU"  # (2)!
  payload_byte_offset: 64       # (3)!
  flow_ids:
    - 201                       # (4)!
  method:
    seq_packets_per_batch:      # (5)!
      sequence_number:
        bit_offset: 512         # (6)!
        bit_width: 32
      packets_per_batch: 1024   # (7)!
```

1. **`reorder_type`** · `string` · *required* — Where the kernel runs. **Supported:** `"gpu"` (CUDA kernel, recommended), `"cpu"` (throughput-bounded; comparison baseline — see `daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml`).
2. **`memory_region`** · `string` · *required* — Name of the landing region for reordered output. Must match a region defined in the top-level `memory_regions:` (here, `Reorder_RX_GPU`).
3. :material-package-variant: **`payload_byte_offset`** · `integer (bytes)` · *required* — Number of leading bytes (typically the header) to skip when copying packets into the reorder region. The kernel copies from this offset to the end of the source buffer.
4. List of flow IDs whose packets feed this reorder config. Must match the `id` field of one or more `flows:` entries above.
5. **`method`** — Algorithm choice. `seq_packets_per_batch` (used here) groups a fixed number of packets per batch, identified by a sequence number within the batch. The alternative `seq_batch_number` encodes the batch index directly in the seqno — see [`daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml).
6. :material-package-variant: **`bit_offset`** / **`bit_width`** · `integer (bits)` · *required* — Location and size of the sequence number within the packet. Here, the seqno starts at byte 64 (`bit_offset: 512` = 64 × 8) and is 32 bits wide — matching a `uint32` at the start of the UDP payload.
7. :material-package-variant: **`packets_per_batch`** · `integer (packets)` · *required* — Number of packets the kernel groups per reordered batch. Must equal the queue `batch_size` above.

**TX-side seqno injection.** The benchmark TX path writes a monotonic big-endian `uint32` into the configured payload offset.

```yaml hl_lines="3 4 7"
bench_tx:
  batch_size: 1024
  payload_size: 1000
  header_size: 64
  udp_src_port: 5000
  udp_dst_port: 5000
  sequence_number_offset: 0   # (1)!
  sequence_number_start: 0
```

1. :material-package-variant: **`sequence_number_offset`** · `integer (bytes)` — Byte offset into the UDP payload where the TX path writes the monotonic seqno. Must align with `sequence_number.bit_offset` in the RX reorder config (after subtracting the header size). Here the seqno is at the very start of the payload.

The reorder bench runs on `daqiri_bench_raw_reorder_seq`:

```bash
./build/examples/daqiri_bench_raw_reorder_seq ./build/examples/daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml --seconds 10
```

Other reorder variants are listed under [question 2 of the decision tree above](#choosing-an-example-config): the CPU-kernel variant, the RX-only variants, and the `seq_batch_number` algorithm with in-kernel int4 → fp32 type conversion (runs on `daqiri_bench_raw_reorder_quantize`).

---
**Previous:** [Raw Ethernet Benchmarking](../benchmarks/raw_benchmarking.md)
