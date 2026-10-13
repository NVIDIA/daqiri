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


## RC RDMA reads and writes

`test_rdma_read_write.py` creates a client and server in one process on real
RDMA-capable local IPv4 addresses. It uses the Python bindings and CPU `host`
memory. Both addresses may belong to one mlx5 port for same-host NIC loopback;
this is a correctness test, not a cable-throughput benchmark.

The test reserves peer target buffers, exports their registered descriptors,
and checks exact bytes and guard ranges for 1,000 READs and 1,000 WRITEs across
both connection directions, single-packet requests, and four-packet batches
with lengths 1, 64, 257, and 4096 bytes. It checks preserved opcodes, caller
WR IDs, pointers, and one completion per batch, then verifies SEND/RECEIVE
coexistence. The final invalid-rkey test requires a `GENERIC_FAILURE` completion;
that deliberate remote-access violation puts the client QP into error.

Set `DAQIRI_PLATFORM_RDMA_CLIENT_IP` and `DAQIRI_PLATFORM_RDMA_SERVER_IP` to
unused IPv4 addresses already configured on a local RDMA interface.
`DAQIRI_PLATFORM_RDMA_PORT` defaults to 19485 and
`DAQIRI_PLATFORM_RDMA_ITERATIONS` defaults to 100 per case.
Set `DAQIRI_PLATFORM_REQUIRE_RDMA_READ_WRITE=1` in qualification jobs so missing
addresses, devices, bindings, or CPU cores fail instead of skipping.
The test does not modify networking. If adding temporary addresses, remove
only those addresses after the test, including on failure.

Run against the freshly built bindings in a project container:

```bash
docker run --rm --network host --ulimit memlock=-1 \
  --device=/dev/infiniband/uverbs0 --device=/dev/infiniband/uverbs1 \
  --device=/dev/infiniband/rdma_cm \
  -v "$PWD:/workspace/daqiri:ro" -w /workspace/daqiri \
  -e PYTHONPATH=/workspace/daqiri/build/python \
  -e DAQIRI_PLATFORM_RDMA_CLIENT_IP=<client-ip> \
  -e DAQIRI_PLATFORM_RDMA_SERVER_IP=<server-ip> \
  -e DAQIRI_PLATFORM_REQUIRE_RDMA_READ_WRITE=1 \
  <daqiri-test-image> \
  python3 -m pytest tests/platform/test_rdma_read_write.py -m platform -p no:cacheprovider
```

Expose the CUDA driver library using the container's GPU runtime or a read-only
host-library mount; the test uses CPU buffers but the shared SDK links CUDA.
Select the actual `uverbs` device paths present on the runner. For evidence of
hardware execution, record before/after `rx_read_requests`, `rx_write_requests`,
and `req_remote_access_errors` under
`/sys/class/infiniband/<device>/ports/1/hw_counters`.

Verified on mlx5_0 (vendor part ID 4129, firmware 28.50.1002): all 10 cases passed,
with counter deltas of 1,001 read requests (including the rejected invalid-key
request), 1,000 write requests, and one expected requester remote-access error.
