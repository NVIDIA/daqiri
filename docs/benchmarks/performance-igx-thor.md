---
hide:
  - navigation
---

# Performance: IGX Thor

This report covers single-host cabled-loopback receive performance on the
NVIDIA IGX Thor Developer Kit. Rates are application throughput averaged across
three independent 30-second runs.

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
drops. Small raw packets use hardware pacing at the highest tested loss-free
rate.

## Results summary

| Stream / engine | Message size | Receive setup | App <span class="unit">Gbps</span> |
| --------------- | -----------: | ------------- | -------------------------------: |
| Raw Ethernet / ibverbs | 8000 B | 1 queue | **198.404 ±0.002** |
| Raw Ethernet / DPDK | 4096 B | 1 queue | **194.351 ±0.010** |
| Socket / RoCE | 1 MiB | 1 queue | **194.016 ±0.086** |
| Socket / TCP | 8000 B | 3 pairs | **47.212 ±2.114** |
| Socket / TCP | 1 MiB | 3 pairs | **46.268 ±0.555** |

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

### Small-packet pacing

| Engine | Payload | Pacer target | App <span class="unit">Gbps</span> |
| ------ | ------: | -----------: | -------------------------------: |
| DPDK | 256 B | 50 Gbps | 49.246 ±0.002 |
| ibverbs | 256 B | 125 Gbps | **98.989 ±3.116** |
| DPDK | 64 B | 20 Gbps | 19.703 ±0.001 |
| ibverbs | 64 B | 65 Gbps | **39.085 ±0.166** |

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

TCP remains below the DGX Spark results. The two leading platform-level
hypotheses are Thor's lower maximum CPU frequency and additional networking
scheduler and interrupt overhead from the supported PREEMPT_RT kernel. The
kernel contribution has not been isolated with an RT versus non-RT comparison.

## UDP status

The current UDP sweep is not accepted for publication because every tested
configuration reports receive drops.

| Message size | Pairs | Delivered App <span class="unit">Gbps</span> | Kernel drops |
| -----------: | ----: | -----------------------------------------: | -----------: |
| 8000 B | 1 | 5.125 ±0.170 | 5,509,474 |
| 8000 B | 2 | 10.492 ±0.052 | 2,903,770 |
| 8000 B | 3 | **15.213 ±0.074** | 1,250,849 |
| 1000 B | 1 | 1.835 ±0.118 | 869,061 |
| 1000 B | 2 | 2.393 ±0.108 | 1,386,214 |
| 1000 B | 3 | **2.523 ±0.158** | 1,081,302 |

These rows will be replaced by paced, loss-free measurements before the UDP
results are treated as final.
