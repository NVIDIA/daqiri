---
hide:
  - navigation
---

# Reproducible benchmark harness

The benchmark harness turns a benchmark process into a result only after it has
validated the process, application, kernel, NIC, and physical-path evidence. A
zero exit code is necessary but never sufficient for a `valid` result.

Version 1 supports one end-to-end adapter: unidirectional UDP through DAQIRI's
Linux socket engine on two physical hosts. This narrow first adapter establishes
the common manifest, lifecycle, provenance, cleanup, result, and report
contracts. It does not claim support for TCP, raw Ethernet, RDMA, GPU workloads,
ResNet, namespace loopback, or hardware loopback.

## Suite intent and site bindings

The harness keeps portable experiment intent separate from machine-specific
bindings:

- A **suite manifest** selects the topology and adapter, duration, repetitions,
  drain and timeout periods, matrix dimensions, requested rate and its scope,
  buffer geometry, collectors, requirements, and acceptance policy.
- A **site profile** binds roles to hosts, worktrees, host or immutable Docker
  binary identities, links, IP and MAC addresses, PCI devices, ports, queues,
  NUMA nodes, and the master, socket I/O, application worker, IRQ, and NAPI CPUs.
- A **resolved manifest** expands every cell and repetition into exact role
  configs, commands, environments, collectors, placements, and unique process
  ownership IDs before any host is contacted.

The schemas are
[`suite-v1.schema.json`](https://github.com/NVIDIA/daqiri/blob/main/scripts/benchmark_harness/schemas/suite-v1.schema.json)
and
[`site-v1.schema.json`](https://github.com/NVIDIA/daqiri/blob/main/scripts/benchmark_harness/schemas/site-v1.schema.json).
The Python loader also performs cross-field safety checks that JSON Schema alone
cannot express.

The checked-in
[`physical-udp-suite.yaml`](https://github.com/NVIDIA/daqiri/blob/main/scripts/benchmark_harness/sample-configs/physical-udp-suite.yaml)
uses the publication default of three independent 30-second repetitions. Copy
[`physical-udp-site.example.yaml`](https://github.com/NVIDIA/daqiri/blob/main/scripts/benchmark_harness/sample-configs/physical-udp-site.example.yaml)
outside the repository and replace every documentation value with a verified
site binding. Do not check machine-specific IP addresses or CPU IDs into the
portable suite.

### Rate scope

`rate_scope` is mandatory:

| Value | Meaning |
| --- | --- |
| `per_pair` | Each TX/RX pair requests the matrix rate. |
| `per_link` | All pairs sharing one physical link divide that link's matrix rate. |
| `aggregate` | Every selected pair divides one suite-wide matrix rate. |

The resolved plan records both the declared rate and each transmitter's actual
`--target-gbps` argument. Results keep requested and achieved rates separate.

### Placement and capacity checks

Before launch, resolution rejects:

- a buffer smaller than the UDP message or an MTU smaller than the IPv4/UDP datagram;
- an unknown host/link, mismatched local and remote ports, duplicate host port,
  duplicate per-link queue ID, or insufficient declared queue capacity;
- a nonexistent pair count or an invalid UDP payload size;
- implicit or unknown rate scope;
- CPU overlap among master, receive-I/O, application worker, IRQ, and NAPI roles,
  unless the suite explicitly sets `allow_cpu_overlap: true`.

The host preflight then verifies the declared CPUs, interface, address, route,
MAC, MTU, link state, speed, PCI identity, binary hash, linked-library
resolution in the declared execution environment, commit, dirty-state policy,
privilege path, hugepage/GPU requirements, `mlnx_perf`, unused UDP ports, and
the absence of an already running copy of the exact benchmark binary. Optional
site-defined contamination checks are read-only commands whose success and
output are archived.

Set `container.runtime: none` for a host executable. For a prebuilt Docker
image, set `runtime: docker`, record both its readable image tag and immutable
`sha256:...` image ID, give the absolute in-image binary and work directory,
and declare whether `--gpus all` is required. The resolved command uses the
immutable ID with `--pull never`; preflight verifies the tag still resolves to
that ID and hashes the binary inside it. The harness does not build or pull
images.

## Plan before running

Always inspect the fully expanded plan first:

```bash
python3 scripts/run_benchmark_harness.py plan \
  --suite scripts/benchmark_harness/sample-configs/physical-udp-suite.yaml \
  --site /path/to/site.yaml \
  --run-id udp-smoke-001 > /tmp/udp-smoke-plan.json
```

`plan` and `run --dry-run` create no artifacts and contact no hosts. The same
inputs and run ID produce byte-equivalent plan data and the same plan SHA-256.

Run only after checking the commands, interfaces, routes, ports, CPU placement,
rate allocation, expected commits, and binary paths:

```bash
python3 scripts/run_benchmark_harness.py run \
  --suite scripts/benchmark_harness/sample-configs/physical-udp-suite.yaml \
  --site /path/to/site.yaml \
  --output /path/to/new-run-directory
```

The output directory must not exist. Exit status is `0` when every repetition is
`valid`, `2` when no repetition failed but at least one is `invalid`, `1` for an
orchestration/setup/process/cleanup failure, and `130` after an interrupt and
bounded cleanup.

## Explicit lifecycle

Each repetition follows these phases:

```text
preflight -> original-state snapshot -> prepare immutable runtime files
          -> startup snapshot -> launch receivers -> receiver readiness
          -> launch collectors -> collector readiness -> active_start snapshot
          -> launch transmitters -> active_end snapshot -> stop collectors
          -> drain -> drain_end snapshot -> stop receivers
          -> shutdown_tail snapshot -> validation -> rollback -> cleanup verification
```

The first version requires `topology_ownership: external`. Preparation writes
only unique, read-only runtime files; it never changes live networking. The
original addresses, routes, device metadata, and counters are still archived so
the no-mutation claim is auditable. A later topology adapter must define exact
rollback before it can claim harness ownership.

Every launched role and collector has a unique ownership ID. The remote worker
starts a new process group and persists its PID, process-group ID, Linux start
tick, command, and run ID. A Docker role additionally gets a unique container
name and ownership label; the worker persists the immutable container ID.
Cleanup verifies the process identity and the container ID plus label before
sending a signal. It sends `SIGINT` to normal receivers so they print their
final summary, or `SIGTERM` on failures, waits a bounded interval, escalates
through `SIGTERM` and `SIGKILL` when needed, then proves no owned process or
container remains. It never uses `pkill -f` or another broad match.

Readiness, collector, snapshot, process-exit, parser, or cleanup-verification
failure fails the repetition. An interrupt is recorded before it is returned to
the shell.

## Artifact bundle

One run produces a single bundle:

```text
run/
  resolved-manifest.yaml
  plan.json
  plan.sha256
  provenance/
    controller.json
    tx-host.json
    rx-host.json
    *-original-state.json
    preflight.json
    prepare.json
  cells/<cell-id>/repetition-<n>/
    lifecycle.jsonl
    processes/
    roles/
    collectors/
    snapshots/
  results.jsonl
  summary.csv
  report.md
```

Resolved configs, commands, environments, role logs, collector logs, process
identity records, and phase snapshots are written without overwrite. JSONL files
are append-only. `results.jsonl` is authoritative; `summary.csv` and `report.md`
are deterministic derived views and can be regenerated:

```bash
python3 scripts/run_benchmark_harness.py report /path/to/run
```

Provenance contains controller and host commits and dirty state, exact arguments,
Python and OS identity, executable hashes, linked libraries, driver and firmware
information, Docker image inspection when selected, full address/route state,
placement, interrupt/softirq state, and GPU identity when available.

## Result states and UDP acceptance

Every planned repetition ends in exactly one state:

- `valid`: every required artifact exists and satisfies the UDP acceptance policy.
- `invalid`: processes ran, but evidence is missing, inconsistent, contaminated,
  or outside the publication criteria.
- `failed`: preflight, setup, readiness, process execution, collection, cleanup,
  or cleanup verification failed.

A physical UDP repetition is `valid` only when all of these hold:

- one authoritative client and server completion summary exists;
- application TX/RX packet and byte totals are nonzero and agree;
- DAQIRI socket-engine packet totals exist and agree with the application totals;
- active-window UDP `InErrors` and `RcvbufErrors`, IP `ReasmFails`, and selected
  NIC receive-discard counters stay within policy and never decrease;
- directional PHY TX/RX packet and byte deltas advance and agree within policy;
- both directions contain enough stable `mlnx_perf` samples after the startup and
  shutdown samples are removed;
- achieved aggregate RX rate and controller-measured active duration reach their
  declared policy thresholds.

Startup, active, drain, and shutdown-tail snapshots remain separate. Application
delivery is taken from final process summaries, while achieved rate comes from
stable active-window hardware samples. Neither process runtime nor packet count
alone is treated as a throughput measurement.

`report.md` labels a cell publication-ready only when the suite requested at
least three repetitions of at least 30 seconds, every repetition exists, and all
are `valid`.

## Resume

Resume is tied to the stored plan hash:

```bash
python3 scripts/run_benchmark_harness.py resume /path/to/run
```

The command refuses a modified resolved manifest or mismatched run identity. It
skips completed repetitions. If a repetition directory exists without a result,
resume performs exact bounded cleanup and records that repetition as `failed`;
it never silently reruns and mixes evidence in an existing directory. Remaining
unstarted repetitions use new immutable paths.

## Unsupported paths and expected failures

Version 1 intentionally rejects or does not implement:

- topology setup/teardown hooks or network mutation;
- namespace/software loopback and mlx5 hardware loopback evidence;
- TCP retransmission, raw Ethernet, RDMA CQ/completion, multi-queue-specific,
  GPU workload, and ResNet acceptance policies;
- container image building or pulling, and `docker exec` into existing containers;
- GPU active-window utilization collection;
- reuse or overwrite of a prior run ID, role config, or artifact directory.

Common failure signatures are explicit: `preflight failed` for identity/topology
drift, `receiver exited before readiness`, `result parser rejected role output`,
`too few stable mlnx_perf ... samples`, and `cleanup verification failed`. These
are failed or invalid evidence states, not successful benchmark points.

Later PRs can add small adapters behind the same lifecycle in this order:
transactional loopback/topology ownership, raw Ethernet and TCP policies, RDMA
capacity and completion evidence after the queue-index work, then GPU workloads
and ResNet. Results from different adapters remain comparable because the
resolved-plan, process-ownership, artifact, and verdict contracts do not change.
