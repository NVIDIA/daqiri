# Platform tests

This directory is reserved for correctness and qualification tests that require a
provisioned GPU, NIC, privileged container, hugepages, or specific host topology. Keep
test logic capability-oriented; dedicated CI/CD jobs select it according to each
runner's declared platform profile. See the parent [testing guide](../README.md) for
the marker, preflight, and local-reproduction policy.

## Named-endpoint raw ibverbs test

`test_named_endpoints.py` materializes the checked-in
`examples/daqiri_example_named_endpoints_tx_rx.yaml` template for one mlx5 device,
initializes the ibverbs engine, creates the runtime endpoint, and requires the example
to report at least one named-endpoint TX packet and a successful NIC completion drain. It
preserves the template's published memory and batch sizing so the test also covers the
example's device-capacity requirements. Set:

- `DAQIRI_PLATFORM_IBVERBS_BDF` to the mlx5 PCI BDF (for example `0000:01:00.0`).
- `DAQIRI_NAMED_ENDPOINTS_EXAMPLE` when the executable is not at
  `build/examples/daqiri_example_named_endpoints`.
- `DAQIRI_PLATFORM_CPU_CORES` optionally to five comma-separated allowed CPU IDs;
  otherwise the test selects the first five CPUs in its affinity mask.
- `DAQIRI_PLATFORM_REQUIRE_NAMED_ENDPOINTS=1` in the dedicated qualification job so
  missing capabilities fail preflight instead of producing a local-development skip.

Run the focused test in the privileged GPU/NIC container with:

```bash
docker run --rm --privileged --gpus all --network host \
  -v /dev/hugepages:/dev/hugepages \
  -v "$PWD:/workspace/daqiri:ro" -w /workspace/daqiri \
  -e DAQIRI_PLATFORM_IBVERBS_BDF=0000:01:00.0 \
  -e DAQIRI_PLATFORM_REQUIRE_NAMED_ENDPOINTS=1 \
  <daqiri-test-image> \
python3 -m pytest tests/platform/test_named_endpoints.py \
  -m platform -p no:cacheprovider
```

Host networking exposes the mlx5 netdev used to discover the endpoint source MAC.

## Raw ibverbs multi-region RX

`test_multi_region_rx.py` uses single-port mlx5 hardware loopback to verify separate
CPU/GPU destinations for two-, three-, and four-region RX queues. It checks segment
lengths, all application bytes, GPU residence, multiple packet slots, short packets
with unused trailing segments, direct RX polling, and rejection of five regions.
It also verifies that `DAQIRI_IBV_STRIDING=1` cannot disable physical scattering.

Set `DAQIRI_PLATFORM_IBVERBS_BDF` to the mlx5 PCI BDF. The test discovers its MAC and
chooses three allowed CPU cores; override these with `DAQIRI_PLATFORM_RX_MAC` and
`DAQIRI_PLATFORM_{MASTER,TX,RX}_CPU`. `DAQIRI_PLATFORM_GPU` and
`DAQIRI_PLATFORM_NUMA` default to `0`. Set
`DAQIRI_PLATFORM_REQUIRE_MULTI_REGION_RX=1` in qualification jobs to fail missing
capabilities rather than skip.

Compile `multi_region_rx.cpp` as the current user inside the project container,
against the SDK being tested:

```bash
c++ -std=c++17 -O2 -pthread tests/platform/multi_region_rx.cpp \
  -o /tmp/multi_region_rx $(pkg-config --cflags --libs daqiri) \
  -I/usr/local/cuda/include -L/usr/local/cuda/lib64 -lcudart
```

Then run as root in the privileged GPU/NIC container with hugepages mounted:

```bash
DAQIRI_MULTI_REGION_RX_EXECUTABLE=/tmp/multi_region_rx \
DAQIRI_PLATFORM_IBVERBS_BDF=0000:01:00.0 \
DAQIRI_PLATFORM_REQUIRE_MULTI_REGION_RX=1 \
python3 -m pytest tests/platform/test_multi_region_rx.py -m platform -p no:cacheprovider
```

With no executable override, the pytest fixture compiles a temporary executable
against the SDK selected by `PKG_CONFIG_PATH`. Keep hardware tests serialized on
the selected port.

The same reproducer also exercises one- through four-region TX queues in direct and
indirect mode, with explicit or named-endpoint network headers. Multi-region TX puts
private application headers in CPU regions and the final payload in GPU memory. Each
case sends 1100 distinct packets to exercise slot/SQ reuse and verifies every application
byte, aggregate IPv4/UDP lengths, and hardware-generated checksums. Five-region TX is
rejected. Run just these cases with the same environment and executable override:

```bash
python3 -m pytest tests/platform/test_multi_region_rx.py -m platform -k tx
```

`test_multi_region_rx.py::test_shared_tx_lifetime` exercises raw and endpoint
shared TX with one GPU payload slot and two slots per private CPU segment. It
checks delayed clones, clone-of-clone, original cancellation, full gathered
checksums, and queue/MR deletion while an unsent descriptor retains storage.

`test_multi_region_rx.py::test_null_region_rx` verifies leading, middle, trailing,
combined, and discard-only null regions in direct and indirect polling, with both fixed
`buf_size` discard spans and omitted/zero `buf_size` for final null MRs. It compares every
retained CPU/GPU byte over 1100 packets, checks null pointers and discarded lengths,
exercises short packets and slot reuse, and supplies deliberately unusable allocation-only
fields to ensure null regions ignore them. Discard-only cases also create and delete a
null region and RX queue at runtime, including the in-use deletion guard. Run with the
same fixture settings:

```bash
python3 -m pytest tests/platform/test_multi_region_rx.py -m platform -k null_region
```

`test_discard_only_large_batch` fills three 513-packet zero-timeout bursts to exercise
RQ sizing and credit reuse. `test_oversized_frame_recovers` injects one frame larger than
a fixed null span, requires the local-length-error CQE, then receives 1100 valid packets
in direct and indirect mode to prove the queue continues across multiple RQ wraps.

## RDMA application round-trip latency

`rdma_latency.cpp` runs RC SEND ping-pong between two existing, physically
connected RoCE endpoints. It uses 32 preposted receives and one outstanding ping,
preallocates all packet buffers and metadata, and echoes directly from the received
buffer. A receive buffer is reposted only after its SEND completion. Both endpoints
verify the sequence and all payload bytes; server verification occurs after SEND
completion. The timed interval starts immediately before client SEND submission
and ends when the client polls the matching RECEIVE completion. Allocation,
client packet initialization, and client payload validation are outside that
interval. Server validation can still compete with subsequent requests; this is
an application RTT test, not a NIC timestamp measurement.

Compile as the current user in the project container against the tested SDK:

```bash
c++ -std=c++17 -O3 -pthread tests/platform/rdma_latency.cpp \
  -o /tmp/rdma_latency $(pkg-config --cflags --libs daqiri) \
  -I/usr/local/cuda/include
```

Supply a separate DAQIRI YAML for each endpoint. Each config needs memory regions
named `TX` and `RX`, each with at least 64 buffers and a `buf_size` covering the
message size. Use `host_pinned` for CPU payload checks. Configure one paired RX/TX
queue, a local `roce://` address, and RC transport. The server local URI must use
port `18491`. Select `poll_mode` independently for each direction; fully direct
connections use one application thread for both directions. Leave
`DAQIRI_RDMA_SEND_SIGNAL_EVERY` unset or set it to `1`; the test rejects other
values before initialization because it waits for every SEND completion. Choose distinct
application, CM (`master_core`), and indirect worker cores on the same CPU topology
for each comparison. Run as root in the privileged GPU/NIC container:

```bash
# Arguments: config server bytes warmup samples app_cpu client_ip server_ip csv
ip netns exec <server-ns> /tmp/rdma_latency /tmp/server.yaml \
  1 64 2000 20000 10 10.253.91.1 10.253.91.2 /tmp/server.csv &
ip netns exec <client-ns> /tmp/rdma_latency /tmp/client.yaml \
  0 64 2000 20000 6 10.253.91.1 10.253.91.2 /tmp/client.csv
wait
```

Repeat with both directions indirect, both direct, and each mixed combination.
Require matching PHY counters to prove that traffic traverses the cable. The test
prints RTT percentiles, completion totals, payload verification, and allocation
counts. The CSV contains measured client RTT samples in microseconds. There are
no timed handle allocations or server payload copies. Repeat runs and alternate
mode order. `ib_send_lat` reports half RTT, so double its latency before comparison.
