# DAQIRI Concepts

Use this reference when the user asks what a DAQIRI term means. Answer the concept first, then briefly explain what it changes about setup, YAML, or benchmark choice. Do not force the user into the onboarding workflow until their concept question is answered.

Primary repo source: `docs/concepts.md`. Related setup sources: `docs/getting-started.md`, `docs/benchmarks/index.md`, `docs/benchmarks/raw_benchmarking.md`, and `docs/benchmarks/socket_benchmarking.md`.

## Explanation Pattern

For each concept answer:

1. Give a plain definition in DAQIRI terms.
2. Name the relevant YAML/API surface.
3. Say what decision it affects during onboarding.
4. Mention one common pitfall only if it helps the user's next step.

## GPUDirect

GPUDirect means the NIC can DMA packet data directly to or from NVIDIA GPU memory instead of staging through CPU memory and then copying with `cudaMemcpy`. In DAQIRI, this is why raw Ethernet and RDMA paths are valuable for GPU-bound acquisition: packets can arrive in buffers that downstream CUDA work can consume with fewer CPU copies and lower latency.

Setup implications:

- Requires a supported NVIDIA RTX or Data Center GPU; GeForce is not supported.
- Uses `memory_regions` with `kind: "device"` for GPU VRAM when the platform supports it.
- The DAQIRI container's patched DPDK path uses dma-buf support, so `nvidia-peermem` is not required inside that container for DPDK.
- On integrated GPU systems such as DGX Spark / GB10, `host_pinned` may be the right memory kind because NIC peer-DMA into discrete GPU VRAM is not the same model.
- GPU visibility and affinity matter; in a container, selected GPU UUIDs may become CUDA ordinal `0`.

Benchmark implications:

- If ConnectX and a suitable GPU are available, prioritize raw Ethernet GPUDirect or RoCE/RDMA over software socket loopback.
- Confirm packet delivery and hardware counters separately; successful GPUDirect setup is not proven by application runtime alone.

## Stream Types and Engines

`stream_type` is the family of I/O the application wants. An engine is the implementation behind that stream.

- `stream_type: "raw"`: kernel-bypass raw Ethernet. Default engine is `ibverbs`; `engine: "dpdk"` selects DPDK when built.
- `stream_type: "socket"` with `udp://` or `tcp://`: Linux sockets, always built.
- `stream_type: "socket"` with `roce://`: RDMA/RoCE via the ibverbs/RDMA path.
- `DAQIRI_ENGINE` at build time selects optional engines: `dpdk` and `ibverbs`. Linux sockets are always built.

Onboarding implication: do not ask users to choose an engine first unless they have a reason. Ask what hardware and peer protocol they have, then choose the stream/engine path.

## Raw Ethernet

Raw Ethernet bypasses the Linux network stack and drives the NIC directly from user space. It is the highest-performance DAQIRI path and the one with hardware flow steering. It requires NVIDIA ConnectX-6 Dx or later; packet pacing, precise timed transmission, and hardware reorder require newer hardware as documented in the repo.

Onboarding implication: when ConnectX is present, prefer raw cabled loopback or raw hardware loopback over socket software tests.

Pitfall: raw packets do not automatically use Linux routing or ARP. The config or application must provide/resolve the correct destination MAC.

## Socket and RoCE

Socket stream configs use endpoint URI schemes:

- `udp://`: Linux UDP sockets.
- `tcp://`: Linux TCP sockets.
- `roce://`: RDMA over Converged Ethernet, using RDMA verbs under DAQIRI's socket/RDMA configuration model.

Onboarding implication: UDP/TCP are good baselines or compatibility tests. RoCE/RDMA is useful when the peer speaks RDMA verbs or when comparing RDMA behavior, but cabled raw Ethernet is usually the first high-performance DAQIRI-owned path when both ends are under DAQIRI control.

## Packets, Bursts, and Segments

A packet is one unit of data sent or received. A burst is a batch metadata object, `BurstParams`, that describes multiple packets. A segment is a contiguous memory slice inside a packet; packets can have one segment or multiple segments backed by different memory regions.

Onboarding implication:

- Larger bursts generally improve throughput but can add latency.
- Header-data split uses multiple segments: CPU memory for headers and GPU memory for payloads.
- Users should inspect bursts through DAQIRI helper APIs rather than assuming struct layout.

## Memory Regions

A memory region is a named pool of packet buffers declared in YAML and referenced by queues.

Common kinds:

- `huge`: explicit hugetlb CPU memory; required or recommended for hot CPU buffers and DPDK/raw paths. DAQIRI fails instead of silently falling back when huge allocation fails.
- `device`: GPU VRAM; requires GPUDirect support.
- `host_pinned`: CUDA-pinned CPU memory; useful on integrated GPU systems.
- `host`: regular CPU memory; not recommended for hot paths.

Onboarding implication: raw benchmarks may fail early if hugepages are not mounted or sized. GPUDirect paths need correct GPU visibility and memory-region affinity.

## Zero-Copy Ownership

DAQIRI returns pointers to buffers that the NIC DMA'd into; it does not copy packet data into a separate application-owned object. That is the zero-copy model.

API implication: the application must free RX bursts after processing and free or send TX bursts after allocation. Holding bursts drains buffer pools and can produce `NO_FREE_BURST_BUFFERS`, `NO_FREE_PACKET_BUFFERS`, drops, or stalled TX.

Onboarding implication: if first-run logs show no-buffer errors, do not just increase buffer counts; check whether the application or benchmark is freeing bursts and whether queue sizing matches the workload.

## Flows and Queues

A queue is a NIC-side RX or TX buffer that points at one or more memory regions. A flow is a hardware match/action rule that steers packets into queues. Flow steering is available for raw Ethernet and is programmed during `daqiri_init()`.

Onboarding implication:

- Queue IDs in raw RX flow actions must match configured RX queues.
- Multi-queue RSS preserves per-flow ordering; it is not packet striping.
- Initialization failures around flows usually mean the NIC rejected a rule or the YAML references invalid queue/flex-item targets.

## Polling Modes

`poll_mode: "indirect"` uses DAQIRI worker threads and favors batching/high throughput. `poll_mode: "direct"` lets the application thread poll the raw ibverbs queue directly and favors minimum single-packet latency.

Onboarding implication: use defaults for first benchmarks unless the user is explicitly measuring latency or application-thread polling.

## RX Reorder

RX reorder assembles out-of-order packets into an aggregate burst. Software reorder can run on CPU or GPU; hardware reorder uses raw ibverbs mlx5 flow steering on supported ConnectX-7+ hardware and can place payloads directly into final output slots.

Onboarding implication: do not start with reorder unless the user's goal requires it. First prove basic TX/RX, then add reorder and validate missing-packet/timeouts separately.
