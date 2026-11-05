---
hide:
  - navigation
---

# Live Customer Demo on DGX Spark

This demo discovers a Spark's hardware, generates and validates raw ibverbs
configurations, measures a cabled raw run, and optionally runs finite TensorRT
inference. Each stage runs live. Recorded image packets are input data; inference
and measurements are computed during the run. No saved results are replayed.

The first implementation supports one Spark with an external cable between its
two physical ports, one or two TX/RX function pairs, `ibverbs`, and `host_pinned`
shared memory. CPU placement and hardware identities come from a local profile.
Other platforms need their own acceptance run before support is claimed.

**Verification status:** lifecycle and failure paths have portable automated tests.
The new packaged workflow has not yet passed a hardware acceptance run. Historical
Spark rehearsal results do not establish acceptance of these new helpers. No
throughput or inference timing is promised.

## Prepare outside the presentation

Use [Getting Started](../getting-started.md) for the existing project image and
[System Configuration](system_configuration.md) for platform prerequisites.
Build only in the project container as the current user; hardware execution uses
a root container with GPU/NIC access. Keep preparation separate from speaking time.

Copy `demos/onboarding/profile.example.yaml` into
`demos/onboarding/local/profile.yaml`. Discover and fill the image ID, GPU UUID,
netdev/BDF/MAC mappings, physical aliases and available CPU cores. CPU numbers in
the template are examples, not a discovered topology. All participating functions,
including aliases used only for counter confirmation, belong in `ports`; each
entry's `physical_port` groups aliases. `paths` selects one or two distinct
function pairs in the same physical p0-to-p1 direction. Confirm the cable endpoints
with the user; enumeration alone cannot prove them.

Read-only preparation must inspect existing containers, image, GPU, hugepages,
CPU affinity/isolation, PCI/netdev/RDMA mappings, carrier, MTU/addressing, MRRS/MPS
and pause policy. Use the existing system-check helper and retain output. For a
previous run, inspect its `run.json`, journal, actual container labels/ID and active
applications. Restore only a verified owned run's baseline under its authorization;
do not adopt a container by name or capture leftover tuning as a new baseline.

The package includes no image, model, engine, PCAP, local profile or historical
logs. Required host utilities include Docker, NVIDIA tools, `ip`, `ethtool`,
`setpci`, `lspci`, `ibdev2netdev`, and `mlnx_perf`. Python requires PyYAML. Run module commands from the checkout using that Python
environment. Tuning writes and restoration require a root **host** shell (or sudo
with the same Python environment); Docker execution needs daemon access. The
run record preserves its original owner's access when root updates it. Do not
run the lifecycle CLI inside a container with a different host/machine identity.
A missing utility is a diagnosed prerequisite, not an invitation to install during
the live presentation.

For optional inference, follow [model/dataset preparation](daqiri-resnet-inference.md#prepare-model-dataset)
using the existing application tools. Prepare a normalized FP16-input/FLOAT-output
ResNet-50 cache and a 256-image PCAP/labels pair (32 images/batch). Use this system's
wire addresses and the canonical 16-bit sequence/1176-byte payload geometry.
Preparation may download data/weights or build a cache only when separately
authorized. The live sequence never does so.

Copy `demos/onboarding/artifacts.example.json` into the local directory; fill
absolute paths and SHA256 hashes, including any ONNX external-weight files as
additional entries in `files`. Record the actual preparation image and GPU UUID,
confirm the declared tensor/normalization contract, and set the profile's manifest
path and `inference.enabled: true`. Artifacts must be under separate directories
beneath the checkout; the container mounts those directories read-only. SHA256
checks prove identity, not model accuracy or TensorRT compatibility. Compatibility
is additionally checked by successful live application/cache loading. A malformed
or incompatible cache must not be silently rebuilt during the demo.

Create a new durable run record (paths below are relative to the checkout):

```bash
python3 -m scripts.onboarding.run_stage init \
  --profile demos/onboarding/local/profile.yaml --run-dir .demo-runs/customer-001
```

An existing directory is never overwritten. `run.json` holds the original profile,
boot/host identity, container ID, tuning journal, stage state and evidence locations.
Keep it and its logs private as appropriate: they contain machine identities.

## Presentation and authorization

The agent-neutral starting prompt is `demos/onboarding/prompt.txt`. A user can ask
an agent to read it or invoke the optional Bartender onboarding skill. This page is
the authoritative playbook; the starting prompt does not duplicate the workflow.

Explain actions as “discover hardware,” “generate a config,” or “run inference.”
Label H (host), C (container), and H2 (host monitoring). Explain briefly, while
showing complete commands, generated YAML and actual output at their stage. Log
links supplement the displayed evidence. Explain the underlying command when a
capture helper executes it; do not make wrapper names the customer-facing action.

Before privileged launch, tuning or traffic, show the exact proposed scope,
restoration/retention plan and cleanup, obtain explicit authorization, and wait.
Retain that authorization for agreed actions; a new scope or reboot needs its own
approval. An `--execute` flag is a mechanism for running an already approved action,
not proof of consent. Never infer permission from a profile or historical approval.

After discovery, container launch, tuning, generation/validation, raw benchmark,
and inference, ask **“Are you ready to continue?”** and wait for an explicit answer.
Feedback, questions, silence and elapsed time do not advance the checkpoint.
Each helper invocation executes at most one stage; no helper advances these gates.

## Seven-stage sequence

### 1. Discover hardware — H

Run fresh preparation checks and show a concise annotated selection explaining
config inputs. For example, substitute the actual selected netdev:

```bash
# PCI BDF selects the NIC function in YAML; MAC supplies Ethernet header input.
readlink -f /sys/class/net/NETDEV/device
cat /sys/class/net/NETDEV/address
# GPU UUID selects the intended GPU; Spark uses host_pinned shared memory.
nvidia-smi --query-gpu=name,uuid --format=csv
# These are host-process CPUs, not yet container CPU availability.
python3 -c 'import os; print(sorted(os.sched_getaffinity(0)))'
```

Use `python3 -m scripts.onboarding.run_stage discover --run-dir .demo-runs/customer-001`
to capture the complete read-only checks. It verifies profile identities and saves
fresh counters/topology; it does not confirm the cable through enumeration.
Stop at the first readiness checkpoint. Planned speaking time: about five minutes.

### 2. Launch our new container — H/C

Preview the complete proposed launch command:

```bash
python3 -m scripts.onboarding.run_stage launch --run-dir .demo-runs/customer-001
```

Show the emitted argv as a reproducible shell command and explain every flag:
`--pull never` prevents downloads; `--rm` removes only this owned container after shell exit; `-d -i` retains its main
shell; a unique name and run label establish ownership; root/privileged/NVIDIA flags
provide hardware access; host networking means NIC changes affect the host;
hugepages and the checkout are bind-mounted; prepared inference directories are
read-only; workdir and entrypoint select the existing source context and shell.
Repeat the exact command before execution. Do not rebuild the image.

After authorization and the discovery readiness answer:

```bash
python3 -m scripts.onboarding.run_stage launch \
  --run-dir .demo-runs/customer-001 --execute
```

Record the actual container ID, image/mount identity, root access, GPU visibility,
compiled engines and container CPU affinity. Preserve unrelated containers.
Stop for readiness before tuning.

### 3. Check and apply system tuning — H/C

Show complete `python/tune_system.py --check all` output and explain applicable
warnings/skips using [System Configuration](system_configuration.md). Tuning is
optional. A disabled optimization does not prevent a valid functional test if its
actual prerequisites are met. Jumbo traffic still requires sufficient effective MTU.

List only authorized settings in the profile before run creation. Supported runtime
changes are per-port MTU, MRRS (masked bits, preserving MPS), explicit pause policy,
and optional declared GPU clock policy. The default empty `tuning` changes nothing.
GPU policy bounds cannot be inferred from idle samples; omit GPU tuning unless its
prior policy is known. Requested clocks and sampled achieved clocks are distinct.

```bash
# Preview the exact selected targets first.
python3 -m scripts.onboarding.runtime_tuning apply --run-dir .demo-runs/customer-001
# Execute after authorization; all original values are journaled before any write.
sudo python3 -m scripts.onboarding.runtime_tuning apply \
  --run-dir .demo-runs/customer-001 --execute
```

Show commands/read-backs and repeat the system check. Explain remaining warnings.
If optional tuning is intentionally omitted, explicitly record the decision:

```bash
python3 -m scripts.onboarding.run_stage skip --run-dir .demo-runs/customer-001 \
  --skip-stage tune --reason 'Use existing effective settings'
```

A partial write or mismatched read-back stops dependent execution. Use the exit
workflow rather than capture a second baseline. Stop for readiness.

### 4. Generate and validate a fresh config — H/C

Use [Configuration Generation](../config-generation.md) for the production
`raw-pair --help` and its resolved arguments. Explain BDFs versus packet-header
MAC/IP values, logical names, queues, bursts and ownership using
[Concepts](../concepts.md). Show proposed command/YAML as a preview before writing;
a preview is unvalidated. No traffic runs during this discussion.

```bash
# Display generator inputs and unvalidated YAML without writing config files.
python3 -m scripts.onboarding.run_stage generate --run-dir .demo-runs/customer-001
# Then generate fresh files and run native validation.
python3 -m scripts.onboarding.run_stage generate --run-dir .demo-runs/customer-001 --execute
```

The helper composes the production generator's `RawPairSpec` objects using the
sealed run profile, emits a single-pair teaching config and the selected one/two-pair
raw config into a fresh directory, displays both full YAML files, and runs native
validation. Show concrete generator inputs and actual validator command/output/exits.
The teaching config is **config-validated, hardware untested**. Identify the separate
raw config selected for the next stage; the helper checks its hash before use.
Never label a failed/unavailable validation successful. Stop for readiness.

### 5. Run the cabled raw benchmark — H/H2

```bash
python3 -m scripts.onboarding.run_stage raw --run-dir .demo-runs/customer-001 --execute
```

Show full config, native validation, installed benchmark and simultaneous monitor
commands before execution. The helper runs the selected raw config for 15 seconds
by default, with a 120-second application deadline and bounded monitor processes.
It checks actual identity/link/MTU, captures all configured aliases before/after,
and monitors one representative per physical connector. Confirm all physical
aliases during preparation; omitted aliases cannot be certified by the report.

Show complete application initialization, warnings/CRITICAL messages, queue and
shutdown output. Put the fresh summary below that output: exits, selected stable
one-second PHY intervals, medians/ranges, alias agreement, TX/RX delivery,
drops/discards/pause, threshold indicators and outstanding shutdown work.
Count aliases once; RX confirms the same transmitted traffic. Exclude startup and
shutdown; packet counts divided by application duration do not establish wire rate.
Missing evidence fails the stage; inspect it before retrying. Stop for readiness.

### 6. Introduce and run finite inference — H/C

Introduce the [application pipeline](daqiri-resnet-inference.md#the-pipeline): image
replay → external cable → NIC receive → GPU reorder and INT8-to-FP16 conversion →
queue handoff → TensorRT ResNet-50 → feature embeddings. On Spark, NIC and GPU share
pinned memory. The finite input is 256 images/eight batches of 32 using the prepared
normalized FP16 cache. Printed labels are dataset ground truth, not predictions.

```bash
python3 -m scripts.onboarding.run_stage inference \
  --run-dir .demo-runs/customer-001 --execute
```

The helper verifies artifact hashes and read-only mounts, adapts the canonical
wire-loopback template with a 100-ms RX timeout, validates it, and invokes finite
replay. Show the config, actual cache-load output, 32768 frame delivery,
image/batch counts, partial/drop indicators, TX drain, application/worker timing,
GPU samples and PHY confirmation. Put its summary below actual output. Preserve
warnings and threshold/tail evidence. Short replay bins are not sustained inference
throughput; independent reorder/normalization/transfer/sink times are unmeasured.

If prerequisites are absent, offer raw-only completion instead of building or
downloading. Record the user's reduced scope with `skip --skip-stage inference
--reason '...'`; skipped is never passed. Stop for readiness before restoration.

### 7. Restore and finish — H/C

First preview this run's journal and selected retention:

```bash
python3 -m scripts.onboarding.runtime_tuning restore --run-dir .demo-runs/customer-001
sudo python3 -m scripts.onboarding.run_stage finish --run-dir .demo-runs/customer-001 --execute
```

Finishing stops only recorded owned monitors, refuses restoration while the owned
container contains active work, restores journaled changes, verifies NIC read-backs,
then gracefully exits only the recorded/labeled container's main shell. It confirms
automatic removal; no blanket kill/prune, device reset or unrelated-container removal
is permitted. Logs/configs remain. `finished` describes verified lifecycle cleanup;
the separate outcome is `full-demo`, `raw-only`, or `early-exit`, so an early stop
is not presented as a successful full demonstration. Verify the pinned image, prepared artifacts and
unrelated containers remain; a failure means recovery required, not complete.

## Early exit, retention and pause

At cancellation or failure, stop dependent stages and offer:

| Choice | Result |
|---|---|
| Finish and restore (default) | Restore changed settings, close owned container, keep evidence |
| Keep selected runtime tuning | Retain exact reviewed journal keys; restore the remainder; close container |
| Pause | Restore by default and retain the idle owned container/checkpoint for inspection/resume |
| Recovery required | Preserve baseline/evidence and container; identify unresolved settings/work |

For example, append `--keep NETDEV/mtu` to **finish** to retain only that selected
MTU. Multiple `--keep` arguments are allowed. `finish --pause --execute` pauses;
add explicit retention keys only when requested. “Keep” is runtime retention, not
permission to create persistent boot/network configuration. `runtime_tuning restore
--keep ...` can perform recovery independently when the container is unavailable.
A kept setting retains its original baseline and is not silently undone on retry.
Settings already at target before the run remain at their baseline values.

Restoration attempts independent NIC fields even if another fails. If current
state differs from both baseline and the demo target, it records a conflict rather
than overwrite an external change. GPU policy is command-verified only: restoration
requires `--confirm-gpu-policy` after the user confirms no intervening policy change
on the same boot; after reboot resolve GPU policy manually. Never claim exact GPU
lock read-back. After manual GPU policy review/restoration, record the administrator's
explicit declaration with `runtime_tuning resolve-gpu --run-dir ... --policy default
--reason 'original policy restored manually; evidence path ...' --execute` (replace
`default` with the original declared policy). This records a manual declaration,
not hardware read-back; use `--keep gpu/policy` only when explicitly retaining the
target policy instead. The baseline is not overwritten and partial writes remain
recoverable.

Required unsupported steps block dependent stages. Optional unsupported steps may
be skipped with an explicit explanation and agreed scope change. Configuration-only
or a different transport is a different scoped outcome; never present it as success
of the cabled/inference demo. Early termination still follows the exit workflow.

## Reboot and resume

This version never edits boot configuration, firmware or drivers and never reboots
automatically. Reboot-dependent preparation is manual and separately authorized.
Offer defer/continue, skip the dependent stage, or pause for preparation. Continue
without reboot only if the **effective running state** satisfies that stage;
pending settings do not count. Optional optimizations may be deferred with limits
reported. Finish or pause, resolve runtime changes and save state before reboot.

```bash
python3 -m scripts.onboarding.run_stage status --run-dir .demo-runs/customer-001
python3 -m scripts.onboarding.run_stage resume --run-dir .demo-runs/customer-001 --execute
```

Resume requires renewed inspection, not restarting the talk. It checks host/boot,
NIC/GPU identity and owned container availability, archives previous stage evidence,
and invalidates generated/traffic stage approvals. Recheck CPU/engine access,
configuration and artifact prerequisites before dependent actions. An absent owned
container is recorded as historical and may be relaunched through stage 2; an
existing mismatched container blocks recovery. Old counters/timings remain historical.
Outstanding tuning after a reboot must be explicitly reviewed/restored first.
A restored run can continue using current settings and explicitly skip optional
tuning; reapplying a new tuning transaction requires a new run/baseline linked to
the retained previous run. Resume never recaptures an unresolved tuned baseline.

## Stress tests and acceptance

Portable tests in `tests/portable/test_onboarding_demo.py` cover selective retention,
pre-existing tuning, partial writes/restores, external drift, journal-write failure,
unknown retention keys, GPU policy uncertainty, invalid profiles, two-pair generation,
missing prerequisites/artifacts, container collisions/ownership, reboot invalidation,
concurrent operations, PID reuse, nonzero/timeout capture, missing evidence, fresh
rate computation and active-work/failed-restore cleanup guards.

Before declaring hardware acceptance, run the full seven-stage workflow on a
prepared Spark, then explicitly authorize tests for cancellation at each checkpoint,
missing optional inference, cable/link failure, unsupported tuning, restore/read-back
failure, selected MTU retention, application/monitor timeout, session interruption,
and manual reboot/resume. Verify no unrelated work changes and all resources have
an explicit final disposition. Authorization denial and unanswered readiness gates
also need an agent-driven presentation test; portable helper tests cannot prove
model instruction-following. Retain hardware evidence separately from the package.
