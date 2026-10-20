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
