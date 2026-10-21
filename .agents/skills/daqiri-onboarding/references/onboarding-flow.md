# Onboarding Flow

Use the [documentation map](documentation-map.md) for technical instructions and helper locations. This reference defines the interview and verification decisions rather than duplicating those instructions.

## Locate the current stage

Use information already supplied, then ask a small batch of unresolved questions:

- What should the first result prove: working install, GPU-accessible acquisition, cabled throughput, hardware loopback, UDP/TCP baseline, RoCE, or diagnosis of a failure?
- Is this the host, a container, or a remote system? Which checkout/image/install and benchmark binaries are available?
- Which GPU and NIC are available? Is there a confirmed cable/peer, and which ports belong to the intended test?

For configuration inputs, distinguish discovered values from user estimates or examples. Mark uncertain port pairing, peer addresses and CPU choices explicitly; verify them before dependent actions. Hardware enumeration alone does not identify cable endpoints.

| Existing state | Next step |
|---|---|
| No checkout | Use Getting Started to obtain source/submodules; do not assume SSH credentials |
| Source but no compatible image/build | Read the build guide, select required optional engines, and compile in the project container |
| Build/image but no usable runtime | Inspect existing containers and propose the required launch; preserve unrelated work |
| Runtime but no concrete config | Discover inputs and use the canonical configuration generator |
| Concrete config | Run native semantic validation, then verify hardware/path prerequisites |
| Prior benchmark output | Interpret or diagnose that run before proposing another |

Do not build or launch another container when an adequate existing runtime already satisfies the request.

## Choose the path

Honor the user's stated transport and goal. Otherwise:

- ConnectX and a confirmed cable/peer: raw Ethernet using a compiled engine.
- ConnectX without a cable/peer: supported raw hardware loopback.
- Linux TCP/UDP compatibility or baseline: always-built socket engine.
- RoCE/RDMA goal: `roce://` endpoints and the optional ibverbs/RDMA engine.
- No usable NIC path or explicit hardware-free goal: software loopback smoke test.

Confirm compiled engine availability using the native validator's `--list-engines` when supported. Do not invent a `socket` build-option value. Read the benchmark overview for current engine choices.

On GB10/Spark, use the documented `host_pinned` shared-memory path; do not mistake it for discrete-GPU device-memory registration or require peermem. On hybrid iGPU/dGPU systems, follow the platform guide for GPU UUID selection and container ordinals. Verify actual CPU affinity in the context that will execute the workload.

## Discover, generate, validate

Use the config-generation guide's netdev-to-BDF/MAC discovery and the system-check helper as needed. Confirm GPU affinity, usable CPU cores, peer MAC/IP/header values, memory kind, frame/buffer sizing, queue counts and hugepage requirements. Read the generator's current `--help`; do not reuse another host's values.

Generate with `raw-pair` or `socket-pair` where applicable. For an existing/custom configuration, preserve application-owned sections and consult the generator's generic rendering support. Write new configs to a fresh user-appropriate directory rather than overwrite originals.

Discover the actual validator in the build/install/runtime, show its command, output and exit status, and resolve errors before traffic. If unavailable, report generation as unvalidated and identify the missing build/runtime prerequisite. Do not substitute YAML parsing or stub validation for native semantic validation.

## Execute the first test

Read only the selected raw or socket/RDMA benchmark guide. Show concrete commands with host/container roles, configured inputs and a bounded duration. Start RX/server before TX/client where the roles are separate. For UDP/TCP/RoCE wire tests, verify the route and namespace path rather than assume same-host endpoints cross the cable.

Use the existing namespace/cross-host setup scripts only for a matching topology after reading their help and current behavior. Explain changes before applying them and plan cleanup for resources created by this run. A pre-existing namespace or container is not owned merely because its name matches an example. Do not delete namespaces containing moved interfaces without following the documented restoration procedure.

Keep first-smoke success separate from optional performance tuning. Do not change firmware, boot settings, MTU, pause policy, clocks or routes solely because a tutorial lists an optimization. Resolve applicability, authorization and restoration before changes.

## Interpret the evidence

- **Cabled raw:** compare sender TX PHY with receiver RX PHY. For throughput, use `mlnx_perf` stable one-second intervals from a run of at least ten seconds; discard startup/shutdown. Count aliases once and use RX as confirmation, not an additional rate.
- **Hardware loopback:** use `vport_loopback_bytes`; PHY counters should stay flat. Label it hardware-loopback throughput, not wire throughput.
- **UDP:** inspect application TX/RX counts and kernel UDP/IP/NIC errors; expose loss or partial shutdown rather than assume equality.
- **TCP:** report delivered/achieved rates and available retransmission/backpressure evidence.
- **RoCE:** require nonzero completions and inspect RDMA-CM, CQ, retry and resource errors.
- **All paths:** inspect queue steering, drops and no-buffer errors. Explain zero-copy burst ownership before recommending larger pools. Report shutdown-tail work and unmeasured timing scopes explicitly.

Onboarding ends with a concrete config, its validation state, the selected runtime/benchmark, and either a verified smoke result or a diagnosed missing prerequisite. Include only the next useful command or measurement; do not label a diagnosed failure a successful benchmark.
