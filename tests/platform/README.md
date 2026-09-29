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
to report at least one successful named-endpoint TX packet. Set:

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
