---
hide:
  - navigation
---

# Performance: IGX Thor

This report covers single-host cabled-loopback receive performance on the
NVIDIA IGX Thor Developer Kit. Rates are application throughput, reported as
the mean across three independent 30-second runs. Sample standard deviations
are shown as ± when at least 0.1 Gbps.

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

Raw Ethernet uses GPUDirect into the discrete GPU. RoCE, TCP, and UDP use
separate client and server network namespaces to force traffic across the cable.

The results in this report passed the following checks. Raw and RoCE
throughput runs were drop-free, with no reported queue, completion, or NIC
receive-buffer drops. UDP socket runs had matching sent and received
packet counts, no kernel receive errors, and no NIC receive discards. Latency
runs completed without timeouts, completion errors, queue handoff drops, or
NIC receive discards. For all throughput results, stable one-second
`mlnx_perf` samples confirmed the rates. Physical-port counters confirmed
that traffic crossed the cable for every reported run, including sockets
and latency tests.

## Results summary

| Stream / engine | Message size | Receive setup | App <span class="unit">Gbps</span> |
| --------------- | -----------: | ------------- | -------------------------------: |
| Raw Ethernet / ibverbs | 8000 B | 1 queue | **198.4** |
| Raw Ethernet / DPDK | 4096 B | 1 queue | **194.4** |
| Socket / RoCE | 1 MiB | 1 queue | **194.0** |
| Socket / TCP | 1 MiB | 3 RX workers | **49.1 ±2.7** |
| Socket / UDP (paced) | 8000 B | 3 RX workers; 6 Gbps cap/worker | **16.0** |

## Raw Ethernet / GPUDirect throughput by payload size

| Payload | Raw ibverbs App <span class="unit">Gbps</span> |
| ------: | -------------------------------------------: |
| 8000 B | **198.4** |
| 4096 B | 197.9 |
| 1024 B | 194.6 |
| 256 B | 99.0 ±3.1 |
| 64 B | 39.1 ±0.2 |

The 64- and 256-byte payloads use the highest tested loss-free TX caps below;
larger payloads are unpaced.

| Payload | Raw ibverbs TX cap |
| ------: | -----------------: |
| 256 B | 125 Gbps |
| 64 B | 65 Gbps |

## Raw ibverbs latency

Application send→receive latency includes TX submission, NIC/cable transit,
and RX delivery. The test sends one packet and waits to receive it before
sending the next. In indirect mode, the TX and RX workers process packets
individually. See the
[raw benchmarking guide](raw_benchmarking.md#direct-polling-latency-sweep) for
timing boundaries and polling settings.

Values are the mean per-run p50 / p99 across three runs of 10,000 measured
packets after 1,000 warmup packets.

### Application send→receive

| Packet size | Host-pinned direct p50 / p99 (<span class="unit">μs</span>) | Host-pinned indirect p50 / p99 (<span class="unit">μs</span>) | GPU direct p50 / p99 (<span class="unit">μs</span>) | GPU indirect p50 / p99 (<span class="unit">μs</span>) |
| -------: | -------------------------------: | ---------------------------------: | -----------------------: | -------------------------: |
| 64 B | 2.43 / 3.03 | 5.11 / 5.77 | 2.70 / 3.12 | 4.65 / 4.95 |
| 1024 B | 3.40 / 4.08 | 5.28 / 5.90 | 2.84 / 3.18 | 4.78 / 5.02 |
| 8192 B | 4.25 / 4.85 | 6.06 / 6.68 | 3.43 / 3.76 | 5.31 / 5.56 |

## Receive throughput with GPU workloads

Each workload runs on received data assembled into a contiguous GPU buffer,
using 8000-byte payloads and raw ibverbs batches of 4,096 packets.

- **FFT** — batched FP32, length-1024 complex-to-complex forward transforms.
- **GEMM** — FP32 1024×1024 matrix multiply.

| Workload | Raw ibverbs App <span class="unit">Gbps</span> |
| -------- | -------------------------------------------: |
| none | 198.4 |
| FFT | 197.7 ±1.3 |
| GEMM (FP32) | 198.1 ±1.3 |

## RoCE RC SEND throughput by message size

| Message size | App <span class="unit">Gbps</span> |
| -----------: | -------------------------------: |
| 8 MB | 193.9 |
| 1 MiB | **194.0** |
| 64 KiB | 192.3 ±0.2 |
| 8 KiB | 32.7 |
| 4 KiB | 16.3 |

## TCP receive throughput

### Single-core throughput by message size

| Message size | App <span class="unit">Gbps</span> |
| ------------ | --------------------------------: |
| 1 MiB | **26.1 ±0.3** |
| 8000 B | 25.2 ±0.6 |
| 1000 B | 6.5 |

### Multi-core throughput by worker count at 1 MiB

| RX workers | App <span class="unit">Gbps</span> |
| ---------: | --------------------------------: |
| 1 | 26.1 ±0.3 |
| 2 | 41.1 ±0.6 |
| 3 | **49.1 ±2.7** |

Each pair uses two isolated CPUs: one for sending and one shared by the
receive-I/O thread and application worker.

## UDP receive throughput

UDP uses an 8 MiB kernel receive buffer per socket and batches of 32 datagrams.
Each RX worker shares its core with its receive-I/O thread and has a
corresponding sender on a separate core. Results use the highest tested pacing
targets that were loss-free across all three repetitions.

### Single-core throughput by message size

| Message size | Pacer target | App <span class="unit">Gbps</span> |
| ------------ | -----------: | --------------------------------: |
| 8000 B | 16 Gbps | **9.0 ±0.2** |
| 1000 B | 2.25 Gbps | 1.9 |

### Multi-core throughput by worker count at 8000 B

| RX workers | Pacer target / worker | App <span class="unit">Gbps</span> |
| ---------: | --------------------: | --------------------------------: |
| 1 | 16 Gbps | 9.0 ±0.2 |
| 2 | 8 Gbps | 12.3 |
| 3 | 6 Gbps | **16.0** |

## Reproduce these Results

Use the [project container](../getting-started.md#run-a-benchmark), the test-system
settings above, and a 200 GbE p0↔p1 cable. Configure the host using the individual
steps in [System Configuration](../tutorials/system_configuration.md).

Run these commands from the repository root as root inside the privileged
container with all GPUs and host hugepages passed through; set `DAQIRI_BUILD_DIR`
to your Release build. The runners save configs, logs, counters, and CSVs under
`bench-results/`.

```bash
export DAQIRI_BUILD_DIR="$PWD/build-thor-perf" RUN_SECONDS=30 REPEATS=3
export LD_LIBRARY_PATH="$DAQIRI_BUILD_DIR/src:${LD_LIBRARY_PATH:-}"
bench=examples/run_cabled_loopback_bench.sh
netns=scripts/setup_cabled_loopback_netns.sh

# Raw throughput and GPU workloads.
"$netns" --platform igx-thor down
"$bench" --platform igx-thor ibverbs sweep
for workload in fft gemm; do
  WORKLOAD="$workload" FFT_LEN=1024 GEMM_DIM=1024 \
    "$bench" --platform igx-thor --payload-size 8000 ibverbs sweep
done
# DPDK headline result.
"$bench" --platform igx-thor --payload-size 4096 dpdk sweep
"$bench" --platform igx-thor ibverbs latency

# RoCE, TCP, and UDP (separate client/server namespaces).
"$netns" --platform igx-thor up
for engine in rdma socket-tcp; do
  "$bench" --platform igx-thor "$engine" sweep
done
# UDP: use an 8 MiB receive buffer per socket.
sysctl -w net.core.rmem_max=8388608 net.core.rmem_default=8388608
for cell in '8000 1 16' '8000 2 8' '8000 3 6' '1000 1 2.25'; do
  read -r payload pairs cap <<< "$cell"
  DROP_CURVE_TARGETS_OVERRIDE="$cap" "$bench" --platform igx-thor \
    --payload-size "$payload" --pairs "$pairs" socket-udp drop-curve-matrix
done
"$netns" --platform igx-thor down
```
