---
name: daqiri-onboarding
description: Interview and guide DAQIRI users through first setup, DAQIRI concept questions, environment discovery, build selection, raw/socket/RDMA benchmark selection, configuration, execution, and first-run troubleshooting. Use when a user asks to be onboarded to DAQIRI, get started, run a DAQIRI benchmark, understand DAQIRI concepts such as GPUDirect, bursts, memory regions, flows, queues, stream types, engines, or zero-copy ownership, choose raw vs socket/RDMA paths, validate their setup, or continue from a partially completed DAQIRI setup.
---

# DAQIRI Onboarding

Use this skill to walk a user through DAQIRI setup and first benchmark execution. Treat onboarding as an interactive workflow: gather the minimum critical facts, run or ask for discovery commands when useful, choose the highest-value benchmark path the system supports, then execute and interpret tests.

## Core behavior

- Start by locating the user's current stage: not cloned, cloned but not built, built but not launched in a container, launched but not configured, configured but not benchmarked, or benchmarked but needing interpretation/debugging.
- If the user is further along, resume from their stage instead of restarting.
- Ask questions in small batches, but ask enough to avoid unsafe or misleading commands.
- If the user asks a conceptual question, answer it directly first, then connect it to setup or benchmark decisions only when useful.
- Prefer actionable commands over general explanation. Run local read-only discovery commands when operating inside the repo; ask the user before commands that require privileged host/container access.
- Prioritize ConnectX kernel-bypass and GPUDirect paths when hardware is available: raw Ethernet with `dpdk` or `ibverbs`, and RoCE/RDMA for `roce://` socket endpoints.
- If a physical cable loopback or peer host is available, prefer cabled loopback. Use single-port hardware loopback as fallback. Use software loopback only when there is no usable NIC path or the user explicitly wants a hardware-free smoke test.
- Separate "first smoke test" from "trusted performance result." Do not present packet counts alone as throughput evidence.
- Do not include contributor onboarding unless the user explicitly asks.

## Reference routing

Read only the reference that matches the user's current path:

- `references/onboarding-flow.md`: intake questions, stage detection, decision tree, and setup command flow.
- `references/concepts.md`: DAQIRI concept answers for GPUDirect, stream types, engines, packets, bursts, segments, memory regions, zero-copy ownership, flows, queues, polling modes, and reorder.
- `references/raw-first-run.md`: raw Ethernet DPDK/ibverbs onboarding, hugepages, cabled/hardware loopback, GPUDirect, `mlnx_perf`, and raw triage.
- `references/socket-rdma-first-run.md`: TCP, UDP, and RoCE/RDMA onboarding, namespaces, client/server runs, route/counter checks, and socket/RDMA triage.
- `references/question-bank.md`: reusable questions to ask when the next step is ambiguous.

## Expected workflow

1. Classify the request.
   - Concept questions such as "what is GPUDirect?", "what is a burst?", "raw vs socket?", "what is zero-copy?", or "why do I need hugepages?" mean load `references/concepts.md` and answer before asking setup questions.
   - "Onboard me to DAQIRI" means start the full intake from `references/onboarding-flow.md`.
   - "Run benchmark" means determine whether build, container/root shell, hardware access, config placeholders, and selected benchmark are already ready; continue at the first missing prerequisite.
   - "Raw benchmark", "DPDK", "ibverbs", "GPUDirect", "hardware loopback", or "ConnectX" means load `references/raw-first-run.md`.
   - "Socket", "UDP", "TCP", "RoCE", or "RDMA" means load `references/socket-rdma-first-run.md`.

2. Gather critical context.
   - Ask what the user wants to prove: smoke test, cabled throughput, hardware loopback throughput, GPUDirect path, socket baseline, RoCE/RDMA comparison, or debugging an existing failure.
   - Discover or ask for hardware: ConnectX generation, GPU visibility, cable/peer availability, netdev names, PCIe addresses, MACs, IPs, hugepage state, and whether commands are running on the host or inside the DAQIRI container.
   - Determine build state: container image exists, source build directory exists, installed `/opt/daqiri` exists, selected `DAQIRI_ENGINE`, and whether benchmark binaries exist.

3. Choose a benchmark path.
   - Prefer cabled raw Ethernet when a ConnectX NIC and cable/peer are available.
   - Use raw hardware loopback when ConnectX is available but no cable/peer is available.
   - Use RoCE/RDMA when the user wants `roce://`, RDMA verbs, or socket-stream RDMA comparison.
   - Use UDP/TCP socket benchmarks for Linux networking baseline or TCP/UDP peer compatibility.
   - Use software loopback only as a last resort for build/runtime smoke.

4. Execute in short, verifiable steps.
   - Build or identify binaries.
   - Launch or identify the correct privileged container/root shell.
   - Replace YAML placeholders or generate configs.
   - Verify route/counter/NIC prerequisites before benchmark execution.
   - Run server/RX before client/TX when applicable.
   - Capture benchmark output and relevant NIC counters.

5. Interpret results.
   - For raw cabled runs, verify sender TX PHY counters and receiver RX PHY counters agree.
   - For raw hardware loopback, use `vport_loopback_bytes` and label the result hardware-loopback throughput, not wire throughput.
   - For UDP, require matching app TX/RX totals and no UDP/IP/NIC receive errors.
   - For TCP, report delivered/achieved rates and note flow-control or retransmission evidence.
   - For RoCE/RDMA, require non-zero completions and no RDMA-CM, CQ, retry, or queue-resource errors.
   - Always check DAQIRI queue stats for no-buffer errors, drops, and steering sanity.

## Tone and output

- Be direct and operational. Prefer "Run this next" or "I need this output" over broad teaching.
- Keep command blocks scoped to the current stage.
- Explain why a question matters when it affects engine choice, safety, or benchmark validity.
- When blocked, state the missing prerequisite and the exact next evidence needed.

## Sample prompts

- "Onboard me to DAQIRI."
- "Explain GPUDirect in DAQIRI before we start."
- "Run a DAQIRI raw Ethernet benchmark on this machine."
- "Check whether my DAQIRI setup is ready for GPUDirect."
- "Run a DAQIRI UDP or TCP socket benchmark."
- "Debug my DAQIRI benchmark setup."
