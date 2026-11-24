# DAQIRI perftest

Use `daqiri_perftest` to compare bandwidth and latency with a consistent command line across raw Ethernet, RoCE, TCP, and UDP. Build with `DAQIRI_BUILD_EXAMPLES=ON` to get the launcher in the build root or the installed `bin` directory. The launcher uses Python 3 and PyYAML and works with `DAQIRI_BUILD_PYTHON=OFF`.

## Select the path

| Topology | Arguments and path |
| --- | --- |
| `hw-loopback` | Raw ibverbs with `--interface <netdev>`. The NIC returns packets internally on the selected port. |
| `sw-loopback` | Raw DPDK, one queue pair and no interface argument. DAQIRI returns packets in software. |
| `same-host --role local` | For raw Ethernet, provide two cabled ports with `--client-interface` and `--server-interface`. For IP transports, provide both IPv4 addresses and optionally the two netdevs to verify address assignment. |
| `same-host` or `two-host` with paired roles | Run once per peer. For raw Ethernet, specify each local `--interface`, the other side’s `--peer-mac`, and the server’s TCP `--control IP:PORT`. For IP transports, specify both IPv4 addresses on each side. Optionally specify the local `--interface` to check address assignment. |

Raw interface arguments accept a netdev or PCI BDF. Use a netdev name when several netdevs map to the same BDF. Paired IP tests use the server address and TCP port 18500 for control by default. Start the server first, then the client with matching test settings. Run each command in its existing network namespace when applicable. Linux can route two local IPs through its local path. Use the directional physical-port counters to confirm a cabled path.

This command measures application latency over local TCP for two queue pairs and two message sizes. Append `--dry-run` to print the resolved YAML and validate it without traffic.

```bash
daqiri_perftest lat --transport tcp --topology same-host \
  --client-address 127.0.0.1 --server-address 127.0.0.1 \
  --queues 2 --sizes 64,4096 --bidirectional \
  --json tcp-latency.json --samples-csv tcp-probes.csv
```

On a host with a usable mlx5 NIC, select its netdev for an internal hardware-loopback bandwidth test.

```bash
sudo daqiri_perftest bw --transport raw --engine ibverbs --topology hw-loopback \
  --interface eth1 --queues 2 --sizes 64,1024,4096 --json ibverbs-bw.json
```

## Control the workload

- `--packet-size` and comma-separated `--packet-sizes` select raw Ethernet L2 frame bytes excluding FCS. `--message-size` and `--message-sizes` select application message bytes. Use one size selector per run. The original `--size` and `--sizes` keep their transport-dependent units.
- `--queues` assigns one steering flow per raw queue, one RC connection per RoCE queue pair, or one endpoint per TCP/UDP queue pair. Socket ports increment for each queue and each sweep size.
- Repeat `--tx-mr` and `--rx-mr` as `kind:buffer_bytes:buffer_count[:affinity]` to define ordered raw packet segments or a single socket/RoCE region. Each queue has its own region configuration. For TCP/UDP, the region sizes limit payload capacity and RX backlog. Default storage is huge for DPDK, host-pinned for ibverbs/RoCE, and host for TCP/UDP.
- Raw ibverbs and RoCE latency default to direct TX and RX polling, which submits one packet or operation at a time. Raw bandwidth, DPDK, TCP, and UDP default to indirect polling. Override either direction with `--tx-poll` or `--rx-poll`. Use `--batch-size` for indirect bursts and `--tx-cores` or `--rx-cores` for indirect pollers. Set `--master-core` and `--app-cores` for application placement. A local run lists server queues before client queues. A single CPU value applies to all queues. For bidirectional TCP bandwidth, `--rx-app-cores` controls the application reader threads.
- Automatic placement uses allowed isolated CPUs for application and polling threads first and prefers an ordinary CPU for the master. Perftest warns if it finds too few isolated CPUs for the requested layout or none at all. Explicit CPU options take precedence. Paired processes on the same host warn if their benchmark CPU assignments overlap.
- In latency mode, `--realtime-priority 80` runs the application threads with `SCHED_FIFO` and verifies the requested priority before starting probes. It requires `CAP_SYS_NICE` or a sufficient `RLIMIT_RTPRIO`. With direct polling these threads also poll the hardware queues.
- Bandwidth defaults to 2 seconds of warmup and 10 seconds of measurement. Use `--warmup-seconds` and `--seconds` to change them. Latency defaults to 1,000 warmup probes and 10,000 measured probes per queue and direction. Use `--warmup-samples` and `--samples` to change them.
- RoCE bandwidth defaults to up to 1,024 posted receives per connection. The RX region's buffer count and the burst metadata shared by local queues cap the target. `--rx-depth` overrides the default within those limits. Paired receive-only RoCE bandwidth servers default to direct polling and replenish released RX buffers in batches of up to 32. RoCE latency defaults to one posted receive per connection and also accepts `--rx-depth`.
- Indirect RoCE bandwidth submits each RC SEND burst in one verbs call. Providers with atomic posting generate one successful completion per burst. For a receive-only peer, `--rx-poll direct --tx-poll direct` runs both sides of the receiver's queue pair on its application thread. The sender can still use indirect polling to submit batches.
- `--bidirectional` starts simultaneous bandwidth streams. Latency runs one probe at a time, with a separate phase for each initiator and queue.

For an example-to-CLI RoCE bandwidth comparison, use `daqiri_bench_rdma
--workload none`. Set `batch_size` in the sender's `rdma_bench_client` or
`rdma_bench_server` section and use an indirect TX queue. A receive-only role
can use direct RX and TX queues for bulk receive recycling.

`daqiri_bench_rdma_latency CONFIG server|client WARMUP SAMPLES CSV` uses
the same preallocated direct RC ping-pong loop as the CLI's unidirectional
software RTT mode. Give each role a separate YAML with direct RX and TX and set
`message_size`, `cpu_core`, and `rx_depth` in its `rdma_bench_<role>`
section. Use the bandwidth example's `DATA_TX_GPU_<ROLE>` and
`DATA_RX_GPU_<ROLE>` memory region names. Both roles must use the same
warmup and sample counts. Set `opaque_payload: true` on both roles when using device regions and
`zero_init: true` on their device TX regions. The client writes the CSV.

In raw message mode each message is one UDP payload. The application bandwidth
report counts payload bytes, and JSON includes the frame size before Ethernet
padding. For ibverbs, perftest announces the named sender and places only the
payload in TX regions. RX regions and DPDK TX regions hold the payload plus the
42-byte Ethernet/IPv4/UDP header and any Ethernet padding. Use a message size
that fits the path MTU.

DAQIRI zeroes device TX regions during setup. For raw ibverbs packet-size
bandwidth runs with indirect TX and a batch larger than one, a single device
region sized for the largest complete frame is initialized with its Ethernet,
IPv4, and UDP header before traffic. On supported mlx5 devices, these bursts
use enhanced multi-packet send (eMPW), the same TX path as the raw bandwidth
sample. Perftest prints the selected frame layout. The named sender supplies
the header for device layouts that only have room for the payload, and for
device-header layouts in direct, single-packet, or latency runs. Raw DPDK
requires the first 42 TX bytes to be CPU-accessible.

```bash
sudo daqiri_perftest lat --transport raw --engine ibverbs --topology hw-loopback \
  --interface eth1 --message-size 1024
```

## Interpret the result

Before traffic, the launcher prints warning and error findings from the read-only `tune_system.py` checks on both peers. Tuning findings are advisory. Use `--json` to retain resolved settings and results, and `--samples-csv` to retain individual latency probes.

The console shows the run and queue configuration above the results tables. Latency
uses microseconds and labels software and hardware intervals. Bandwidth reports
application rates and counts in one table and available NIC counter rates in another.
The JSON and probe CSV retain their original units and precision.

Bandwidth displays submission and reception per direction and queue, with receive drain traffic reported separately. On supported NICs, the launcher samples `mlnx_perf` once per second and keeps complete intervals inside the measurement window. Hardware loopback reports the aggregate `vport_loopback_bytes` rate once. Cabled runs check the sender TX and receiver RX physical counters.

Same-host software latency starts immediately before TX submission and ends when the application receives the matching packet or complete TCP message. Where available, hardware latency on the same probe is RX hardware timestamp minus TX CQE timestamp. A TX completion can follow RX, so the difference is signed. Both distributions include min, median, p95, p99, and max. Two-host measurements use full application ping-pong RTT. Paired same-host runs also use RTT for the software interval if the CPU clock domains cannot be verified.

For a raw ibverbs echo with two processes, pass `--hardware-rtt` on both roles with `--topology same-host` or `--topology two-host`. The initiator reports `reply_rx_hw_minus_request_tx_cqe` alongside `application_rtt` for the same probes. The hardware interval uses the initiator's NIC clock for both timestamps and includes the responder's application turnaround. With direct polling, the responder prepares each reply before signaling readiness. Host-only runs validate the payload token after posting the reply. Device-memory runs use the dedicated receive queue, frame or message length, and raw flow ID where available to match one probe at a time. A timeout invalidates an opaque-payload case so a late reply cannot be counted toward a later probe. With `--bidirectional`, each role initiates its own phase.

Check the active `REAL_TIME_CLOCK_ENABLE` values and PTP clock alignment for hardware comparison. Raw ibverbs and RoCE hardware latency use the same packed wall-clock CQE format for TX and RX. Perftest submits the TX packets immediately. For distinct PHCs, observed clock skew plus sampling uncertainty must be at most 1 µs before and after the case. Perftest prefers Linux kernel cross-timestamps and records the sampling method in the JSON preflight. If the settings, clock comparison, or timestamp APIs prevent hardware measurement, the launcher reports the reason and uses CPU timing. The CPU result includes application TX and RX processing. See the [firmware and PTP setup guide](../tutorials/system_configuration.md#enable-accurate-tx-scheduling-packet-pacing-and-hardware-rx-timestamps).

Agent run instructions are in [examples/perftest/AGENTS.md](https://github.com/nvidia/daqiri/blob/main/examples/perftest/AGENTS.md). Potential comparative tests include rate caps, CQ completion moderation, RDMA READ/WRITE, and latency while running an independent bandwidth load.
