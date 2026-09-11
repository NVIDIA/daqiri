# DAQIRI benchmark harness

This package implements the shared orchestration and evidence foundation used by
`scripts/run_benchmark_harness.py`. It resolves portable suite intent against a
site-local profile, supervises exact local or SSH process groups, captures
provenance and phase-specific counters, and emits one canonical verdict per
repetition.

The first version deliberately supports one vertical path: unidirectional
physical cross-host UDP with the Linux socket engine. It accepts only externally
prepared topology and does not change addresses, routes, IRQ placement, MTUs, or
link state.

## Dependencies

- Python 3.10 or newer with PyYAML
- Linux `/proc` and `/sys` on benchmark hosts
- `git`, `ip`, `ethtool`, and `mlnx_perf` on benchmark hosts
- `ssh` for hosts whose profile transport is `ssh`
- a matching executable `daqiri_bench_socket` build on every benchmark host,
  either on the host or in a prebuilt Docker image
- Docker on hosts whose profile selects `container.runtime: docker`

The remote worker uses only the Python standard library. Dynamic commands and
paths travel as JSON over standard input; they are not interpolated into an SSH
shell command.

## Package map

- `manifest.py`: strict semantic validation and deterministic plan expansion
- `executor.py`: local/SSH JSON-RPC transport
- `remote_worker.py`: process/container supervision, exact cleanup, snapshots, and preflight
- `lifecycle.py`: repetition state machine and rollback verification
- `udp.py`: socket output parsing, counter derivation, and UDP verdict policy
- `results.py`: append-only JSONL and deterministic CSV/Markdown views
- `schemas/`: machine-readable suite and site-profile schemas
- `sample-configs/`: portable suite and non-runnable documentation profile

See the [reproducible benchmark harness guide](../../docs/benchmarks/reproducible-harness.md)
for usage, artifacts, evidence semantics, and current limitations.

## Tests

The tests use fake local/SSH executors for deterministic failure injection and a
real local worker for process-group cleanup:

```bash
python3 -m pytest -q tests/benchmark_harness
```

They require no NIC, GPU, root privilege, or remote host. The repository
container includes `pytest` and PyYAML.

## Known limitations

- Version 1 does not create or restore network topology because it never mutates it.
- Prebuilt Docker role execution is supported, but the harness never builds or
  pulls images. GPU utilization collection, loopback, TCP, raw Ethernet, RDMA,
  GPU workloads, and ResNet adapters are not implemented.
- `resume` never reruns a repetition whose artifact directory already exists;
  it performs bounded cleanup and records that repetition as `failed`.
- The checked-in site profile uses documentation addresses and must not be run as-is.
