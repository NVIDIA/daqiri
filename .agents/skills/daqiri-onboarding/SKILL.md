---
name: daqiri-onboarding
description: Guide DAQIRI setup, configuration generation, first benchmarks, concept questions, and first-run troubleshooting. Use for getting started, resuming setup, choosing raw Ethernet versus UDP/TCP or RoCE, validating a configuration, or explicitly running an available customer demo.
---

# DAQIRI Onboarding

Help the user reach a first verified result appropriate to their hardware and goal. Resume from the first missing prerequisite rather than restarting setup.

## Route the request

- **Concept question:** use [the documentation map](references/documentation-map.md) to read the relevant concept section. Answer directly before asking setup questions.
- **Setup, configuration, benchmark, or troubleshooting:** read [the onboarding flow](references/onboarding-flow.md), then only the documentation needed for the current stage.
- **Explicit live customer/onboarding demo:** use the optional demo routing below. Ordinary first-run onboarding does not require a demo package.

## Working rules

- Establish what the user wants to prove, their current stage, host/container context, and available hardware. Discover local facts read-only; ask only for missing information that affects the next decision.
- Use the matching checkout's documentation, generator and validator as the technical sources of truth. Resolve paths using the documentation map; do not assume a username, checkout path, image tag, CPU layout or NIC identity.
- Honor the requested transport and test scope. When the user wants a hardware data path and has no preference, favor cabled raw Ethernet with ConnectX, then supported hardware loopback. Use software loopback for a hardware-free smoke test, not as proof of a NIC path.
- Explain proposed privileged container launches, host/network changes and traffic before execution, and obtain authorization if the session has not already supplied it. Retain authorization for agreed actions. Readiness/pacing answers are separate from permission.
- Compile inside the project container as the current user; run hardware benchmarks with the required root/hardware access. Read the current container/build instructions instead of copying a fixed launch recipe.
- Show the resolved config-generation command and generated YAML. Validate with the native hardware-free validator before traffic; validation alone does not prove hardware support or delivery.
- Capture fresh commands, statuses, application output and relevant counters. Label saved results as historical. Distinguish smoke-test success, config validation and measured hardware performance.
- Keep explanations concise and scoped to the current stage. When blocked, name the missing prerequisite and the next evidence needed. Contributor onboarding is outside scope unless requested.

## Optional live customer demo

Only on an explicit demo request, locate a prompt supplied by the user or an available demo package in the checkout. The current local convention is `customer-demo-*/onboarding-demo-prompt.txt`; it is optional and not shipped with this skill. Resolve candidates from the repository root, not the skill installation directory. If multiple candidates exist, ask which to use. If none exists, explain that the customer demo is unavailable and offer ordinary onboarding; do not invent its sequence or start traffic.

When a prompt exists, read it and the documents it requests before beginning. Follow its latest presentation, authorization, preparation and readiness instructions over older demo notes. Saving, editing or explaining a prompt does not launch the demo. A demo request does not itself authorize privileged actions beyond the session's explicit scope.

## Example requests

- “Onboard me to DAQIRI.”
- “Explain GPUDirect before we start.”
- “Generate and validate a raw Ethernet config for this machine.”
- “Continue from my existing container and run a TCP benchmark.”
- “Debug these no-free-buffer errors.”
- “Use the available package to conduct my live DAQIRI customer demo.”
