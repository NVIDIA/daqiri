# DAQIRI onboarding skill benchmark

Self-assessed development comparison completed 2026-11-24. Publication validation
and signing are separate from these historical measurements.

14 development scenarios x two arms x three attempts x two providers = 168 fresh
sessions, 42 per provider/arm. Both arms shared source/docs and prepared native
tools, with isolated sessions, counterbalanced order and natural skill discovery.
Participant sessions had no GPU/NIC mappings or live traffic. Source revision:
`2c315f1e6374d7cb9df8889d63bcb1a498fa2378`, plus the shared walkthrough correction.

| Requested model (`xhigh`) | Material success: no skill -> skill | Tokens | Session time | Non-cache-read input + output |
| --- | --- | ---: | ---: | ---: |
| Codex `gpt-6.1-sol` | 42/42 -> 42/42 | -16.9% | -8.8% | -11.5% |
| Claude `claude-opus-5-5` | 37/42 -> 41/42 | -43.8% | -27.4% | -26.6% |

Resource change is `100 * (sum(skill) / sum(no_skill) - 1)` over every attempt,
including task failures and failed inner probes. Tokens are native final
cache-inclusive input plus output, not monetary cost. Time includes CLI startup,
provider waits and tools; preparation/grading/cleanup are excluded. Effort was
not independently reported; Claude reported its requested model, Codex did not.

Claude improves throughput/evidence interpretation (0/3 -> 2/3) and direct-polling
ownership (1/3 -> 3/3); one skill-assisted answer still overclaims losslessness.
Codex uses fewer tokens in 12/14 cases; routine editing uses 12.2% more and pending
activation 16.9% more. Claude uses fewer tokens and less time in all 14 observed
cells. All 24 generation/rendering sessions pass with required operations verified.

Grades were frozen before joining resources to conditions: 168 primary AI reviews,
110 secondaries and 28 adjudications. A supplemental diagnostic census and
materiality/common-versus-engine-validator clarifications preceded unblinding;
these were not preregistered. Primary-only Claude success is 39/42 -> 42/42;
final grading is 37/42 -> 41/42. Both-successful matched Claude pairs still save
42.9% tokens and 27.6% time. No attempts were rerun/replaced; no infrastructure,
authentication or quota failures occurred. AI judges can share errors and style
can reveal treatment. Three repeats do not establish significance or all-prompt
benefits; individual helper/fast-path contributions were not isolated.

The full development dataset, fixtures and detailed reports are retained outside
the distributed skill. `evals/evals.json` contains compact self-contained checks
of these workflows; those rewritten checks have not been run in this benchmark.
The numbers above describe the archived development campaign, not measurements
of the compact publication dataset. No personal/local identifiers, credentials,
operational inventories or raw execution traces are distributed.
