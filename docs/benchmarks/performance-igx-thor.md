---
hide:
  - navigation
---

# Performance: IGX Thor

This report covers single-host cabled-loopback receive performance on the
NVIDIA IGX Thor Developer Kit. Rates are application throughput, reported as
mean ± sample standard deviation across three independent 30-second runs.

## Test system

| Component | Detail |
| --------- | ------ |
| Platform | NVIDIA IGX Thor Developer Kit, T7000 |
| CPU | 14-core Arm CPU, maximum 2.601 GHz |
| GPU | NVIDIA RTX PRO 6000 Blackwell Max-Q |
| NIC | NVIDIA ConnectX-7, firmware 28.46.1006 |
| Network | One 200 GbE cable connecting physical ports p0 and p1; MTU 9000 |
| Host software | Ubuntu 24.04.2; Linux 6.8.0-1019-nvidia-tegra-rt with PREEMPT_RT |
| GPU software | NVIDIA driver 580.00; CUDA 13.0 |
| Power mode | 120W; CPU and GPU performance settings maximized |
| CPU placement | Cores 8-13 isolated; cores 0-7 reserved for the OS and interrupts |
| DAQIRI build | Release; DPDK and ibverbs engines enabled |

Raw DPDK and raw ibverbs use GPUDirect into the discrete GPU. RoCE, TCP,
and UDP use separate client and server network namespaces to force traffic
across the cable. Only application rate is reported.

Raw and RoCE results have no reported queue, completion, or NIC priority-buffer
drops. The UDP results have zero application packet loss, kernel receive
errors, and NIC receive discards. Small raw packets use hardware pacing at the
highest tested loss-free rate.

## Results summary

| Stream / engine | Message size | Receive setup | App <span class="unit">Gbps</span> |
| --------------- | -----------: | ------------- | -------------------------------: |
| Raw Ethernet / ibverbs | 8000 B | 1 queue | **198.404 ±0.002** |
| Raw Ethernet / DPDK | 4096 B | 1 queue | **194.351 ±0.010** |
| Socket / RoCE | 1 MiB | 1 queue | **194.016 ±0.086** |
| Socket / TCP | 8000 B | 3 pairs | **47.212 ±2.114** |
| Socket / TCP | 1 MiB | 3 pairs | **46.268 ±0.555** |
| Socket / UDP | 8000 B | 3 pairs | **15.838 ±0.041** |

## Raw Ethernet throughput

| Payload | DPDK App <span class="unit">Gbps</span> | Raw ibverbs App <span class="unit">Gbps</span> |
| ------: | ---------------------------------------: | ----------------------------------------------: |
| 8000 B | 193.961 ±0.010 | **198.404 ±0.002** |
| 4096 B | 194.351 ±0.010 | **197.858 ±0.006** |
| 1024 B | 191.441 ±0.013 | **194.634 ±0.023** |
| 256 B | 49.246 ±0.002 | **98.989 ±3.116** |
| 64 B | 19.703 ±0.001 | **39.085 ±0.166** |

Raw ibverbs equals or exceeds DPDK at every tested payload. Both engines
approach the 200 GbE link ceiling at payloads of 1024 bytes and larger. Raw
ibverbs provides approximately twice the DPDK application throughput at 64-
and 256-byte payloads.

### Small-packet test settings

The 64- and 256-byte results above use TX pacing to avoid receive drops.
The configured caps are test settings; compare engines using the measured
application receive rates in the throughput table above.

| Payload | DPDK TX cap | Raw ibverbs TX cap |
| ------: | ----------: | -----------------: |
| 256 B | 50 Gbps | 125 Gbps |
| 64 B | 20 Gbps | 65 Gbps |

An unpaced run measures receive throughput under saturation, but a run with
drops does not establish a sustainable loss-free rate. Pacing is unnecessary
when an unpaced run is already loss-free. In the small-packet rate sweeps,
higher offered rates introduced drops and sometimes reduced delivered
throughput. The 1024-byte and larger results above are unpaced.

## Receive throughput with GPU workloads

Each workload runs on the actual received data after DAQIRI assembles the raw
packets into a contiguous GPU buffer. DPDK uses a batch size of 10,240 packets;
raw ibverbs uses 4,096 packets.

- **FFT** -- batched FP32, length-1024 complex-to-complex forward transforms.
- **GEMM** -- FP32 1024x1024 matrix multiply.

| Workload | DPDK App <span class="unit">Gbps</span> | Raw ibverbs App <span class="unit">Gbps</span> |
| -------- | ---------------------------------------: | ----------------------------------------------: |
| none | 193.961 ±0.010 | **198.404 ±0.002** |
| FFT | 193.604 ±0.542 | **197.658 ±1.300** |
| GEMM (FP32) | 194.078 ±0.547 | **198.138 ±1.302** |

FFT and GEMM produce no meaningful throughput reduction. Every workload run
completed without reported queue, completion, or NIC priority-buffer drops.

## RoCE RC SEND throughput

| Message size | App <span class="unit">Gbps</span> |
| -----------: | -------------------------------: |
| 8 MB | 193.916 ±0.069 |
| 1 MiB | **194.016 ±0.086** |
| 64 KiB | 192.312 ±0.214 |
| 8 KiB | 32.735 ±0.053 |
| 4 KiB | 16.308 ±0.082 |

RoCE reaches approximately 194 Gbps for messages of 64 KiB and larger.

## TCP throughput

| Message size | 1 pair | 2 pairs | 3 pairs |
| -----------: | -----: | ------: | ------: |
| 1 MiB | 28.263 ±0.293 | 40.379 ±2.070 | **46.268 ±0.555** |
| 8000 B | 25.199 ±0.640 | 40.484 ±0.597 | **47.212 ±2.114** |
| 1000 B | 6.483 ±0.028 | 12.783 ±0.004 | **18.420 ±0.050** |

Each pair uses one pinned client core and one pinned server core. Three pairs
consume all six isolated CPUs. Native `iperf3` reached approximately 25.9 Gbps
with one stream and 43.5 Gbps with eight streams, so DAQIRI matches or exceeds
the native Linux TCP baseline on this system.

TCP results on the IGX Thor remain below the DGX Spark results. A single-core
[CoreMark](https://github.com/eembc/coremark) comparison measured **26,243 ±24**
iterations/s on IGX Thor (CPU 8) and **42,463 ±16** on DGX Spark (CPU 19). Both
systems ran the same GCC 13.3 `-O2` binary pinned to one core; scores are the mean
and sample standard deviation of three validated performance runs. Spark's
**1.62×** higher score suggests that lower per-core CPU performance explains
most of the roughly **2× TCP throughput gap**.

## UDP throughput

The corrected UDP sweep uses an **8 MiB kernel receive buffer per socket**.
The RX batch size is 32 datagrams. Each pair uses one client CPU and one server
CPU, with the server's receive-I/O thread and application worker sharing its CPU.

Every accepted repetition has matching sent and received packet counts, zero
kernel UDP receive errors, and zero NIC receive-buffer or priority-buffer
discards. Stable one-second `mlnx_perf` samples and directional PHY counter
deltas confirm traffic crosses the cable.

| Message size | Pairs | TX cap per pair | Delivered App <span class="unit">Gbps</span> |
| -----------: | ----: | --------------: | -----------------------------------------: |
| 8000 B | 1 | Unpaced | 9.031 ±0.039 |
| 8000 B | 2 | Unpaced | 11.936 ±0.393 |
| 8000 B | 3 | Unpaced | **15.838 ±0.041** |
| 1000 B | 1 | 2.25 Gbps | 1.918 ±0.005 |
| 1000 B | 2 | 1.75 Gbps | 2.821 ±0.007 |
| 1000 B | 3 | 0.50 Gbps | 1.500 (σ <0.001) |

Receive rates are aggregate; TX caps apply per pair and may exceed the achieved
send rate. All 1000-byte cases use the same paced sweep: 0.50–2.25 Gbps per pair
in 0.25 Gbps steps. Rows show the highest caps that passed three 30-second runs
without loss. Higher caps failed confirmation for two and three pairs with NIC
receive-buffer drops, sometimes intermittently. These are validated operating
points on the tested grid, not proven maximum rates.
