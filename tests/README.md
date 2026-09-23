# Testing DAQIRI

DAQIRI organizes tests by their execution requirements:

- `tests/portable/` contains tests that need only the source checkout. Pytest collects
  only this directory by default.
- `tests/cpp/` contains build-backed C++ tests registered with CTest.
- `tests/bindings/` contains tests that import the compiled `daqiri` module.
- `tests/platform/` contains qualification tests selected by CI/CD jobs on provisioned
  GPU/NIC platforms.

Keeping the lanes in separate directories prevents a default run from importing a
binding or platform test module before pytest can apply marker selection.

## Portable Python tests

These tests run directly from a source checkout and must not import the compiled
`daqiri` Python module. On the host, use a virtual environment so the command also
works on distributions that protect the system Python environment:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install --requirement tests/requirements.txt
.venv/bin/python -m pytest
```

The project container already includes the current test packages. To synchronize it
with a changed dependency manifest, install the manifest explicitly in the disposable
container environment:

```bash
python3 -m pip install --break-system-packages \
  --requirement tests/requirements.txt
python3 -m pytest
```

`scripts/check_pr.sh` runs `tests/portable/` in its local virtual environment.
Feature-specific Python dependencies belong in `tests/requirements.txt` and should be
added by the feature that needs them.

## C++ tests

Build C++ tests explicitly and run them with CTest:

```bash
cmake -S . -B build -DDAQIRI_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The GVSP test is dependency-free and exercises complete synthetic
Ethernet/IPv4/UDP/GVSP frames plus frame assembly edge cases. Future C++ tests
should also be registered with CTest and use a native C++ test framework when
that dependency is justified.

## Python-binding tests

Put tests that import `daqiri` under `tests/bindings/` and mark them with
`@pytest.mark.bindings`. The bindings are not part of the default DAQIRI build, so
build a test image explicitly and expose its installed module:

```bash
IMAGE_TAG=daqiri:python-tests \
DAQIRI_BUILD_PYTHON=ON \
scripts/build-container.sh

docker run --rm --gpus all \
  -v "$PWD:/workspace/daqiri:ro" \
  -w /workspace/daqiri \
  -e PYTHONPATH=/opt/daqiri/lib/python \
  daqiri:python-tests \
  bash -lc 'python3 -m pip install --break-system-packages \
    --requirement tests/requirements.txt && \
    python3 -m pytest tests/bindings \
    -m "bindings and not platform" -p no:cacheprovider'
```

This explicit build prevents a binding test from accidentally passing against an
unrelated module installed on the host. Targeting `tests/bindings/` avoids importing
platform test modules, while the marker expression also rejects a misplaced test that
is marked for both lanes.

## Platform tests

Put pytest tests requiring a NIC, GPU, hugepages, or privileged access under
`tests/platform/` and mark them with `@pytest.mark.platform`. If one also imports
`daqiri`, mark it `bindings` as well. Keep the tests capability-oriented: CI/CD chooses
the subset for a runner based on its GPU, NIC, engine, privileges, and host topology.
The job owns container launch, device assignment, platform configuration, and resource
serialization.

These tests are excluded from the standard local PR check. A developer can
reproduce a selected platform test in the Python-enabled image built above on a system
with matching capabilities:

```bash
docker run --rm --privileged --gpus all \
  -v /dev/hugepages:/dev/hugepages \
  -v "$PWD:/workspace/daqiri:ro" \
  -w /workspace/daqiri \
  -e PYTHONPATH=/opt/daqiri/lib/python \
  daqiri:python-tests \
  bash -lc 'python3 -m pip install --break-system-packages \
    --requirement tests/requirements.txt && \
    python3 -m pytest tests/platform \
    -m platform -p no:cacheprovider'
```

Local capability checks may skip tests to make reproduction convenient. A dedicated
platform job must instead fail its preflight when a capability declared by that runner
is unavailable; silently skipping the platform's required coverage must not produce a
green qualification job.

The benchmark executables under `examples/` remain DAQIRI's current integration and
performance tools. Platform correctness tests may drive those executables, but stable
performance measurements and thresholds belong in dedicated benchmark pipelines rather
than the ordinary pass/fail suite.
