# Skill Benchmark: daqiri-onboarding

> ✅ **Overall verdict: PASS — Recommended for publication**

## Publication Recommendation

Recommended for publication based on the completed evaluation evidence in this report.

## Evaluation Metadata

- Skill: `daqiri-onboarding`
- Validation status: `passed`
- Evaluation date: 2026-11-24
- Evaluator version: `1.6.10`
- Agents: Claude Code (`aws/anthropic/bedrock-claude-opus-5-5`), Codex (`openai/openai/gpt-6.1-sol`)
- Tasks: 14 evaluation tasks (14 positive)
- Dataset digest: `sha256:76ab1dcf7caf6398b46e1cbda36f79ceab21882d9ff28af9291693db30972f9d` (skill-evaluator-dataset-snapshot/1)
- Attempts per task: 1
- Environment: `k8s-sandbox`
- Tier 2 evidence: required for publication
- Tier 3 evidence: required for publication

Each task attempt ran in its own isolated sandbox pod.

## Tier 1 LLM Provenance

SkillSpector provider, model, analyzer revision, and determinism evidence retained for this scan.

| Skill | Provider | Adapter | Analyzer | Model | Revision | Determinism |
| --- | --- | --- | --- | --- | --- | --- |
| daqiri-onboarding | nv_inference | openai | mcp_tool_poisoning | `aws/anthropic/bedrock-claude-opus-5-5` | `2.13.5` | nondeterministic / provider_defaults |
| daqiri-onboarding | nv_inference | openai | semantic_developer_intent | `aws/anthropic/bedrock-claude-opus-5-5` | `2.13.5` | nondeterministic / provider_defaults |
| daqiri-onboarding | nv_inference | openai | semantic_quality_policy | `aws/anthropic/bedrock-claude-opus-5-5` | `2.13.5` | nondeterministic / provider_defaults |
| daqiri-onboarding | nv_inference | openai | semantic_security_discovery | `aws/anthropic/bedrock-claude-opus-5-5` | `2.13.5` | nondeterministic / provider_defaults |
| daqiri-onboarding | nv_inference | openai | meta_analyzer | `aws/anthropic/bedrock-claude-opus-5-5` | `2.13.5` | nondeterministic / provider_defaults |

- Controls for `daqiri-onboarding`: temperature=not set (source=provider_default, forwarded=no); seed=not set (source=unset, forwarded=no); reasoning_effort=not set (source=provider_default, forwarded=no)

## What This Report Answers

The three-tier evaluation checks whether the skill:

- is safe to use;
- produces correct answers;
- is discovered and activated when needed;
- helps the agent complete the user's goal and expected workflow; and
- avoids wasted skill and tool usage.

## Results at a Glance

| Measure | Claude Code (Baseline → Skill Uplift) | Codex (Baseline → Skill Uplift) |
|---|---:|---:|
| Overall | 91.5% — baseline ran, but no comparable score was available; uplift unavailable | 85.7% — baseline ran, but no comparable score was available; uplift unavailable |
| Security | 100.0% → 100.0% (±0.0 points) | 96.4% → 96.4% (±0.0 points) |
| Correctness | 65.7% → 97.1% (+31.4 points) | 95.7% → 98.6% (+2.9 points) |
| Discoverability | 98.9% — baseline ran, but no comparable score was available; uplift unavailable | 90.4% — baseline ran, but no comparable score was available; uplift unavailable |
| Effectiveness | 44.1% → 69.1% (+25.0 points) | 65.5% → 72.6% (+7.1 points) |
| Efficiency | 92.3% — baseline ran, but no comparable score was available; uplift unavailable | 70.7% — baseline ran, but no comparable score was available; uplift unavailable |

**How to read this table:** baseline is the same task attempted without the target skill. Scores are rounded to one decimal; threshold-adjacent values use additional precision so their displayed band matches the verdict. Uplift is derived from those displayed scores and shown in percentage points.

Example: `47.0% → 92.0% (+45.0 points)` means the skill-assisted run scored 92.0%, 45.0 percentage points above its 47.0% no-skill baseline.

### Comparison details

Shared-source comparisons use the same signals and weights on both conditions. They do not replace the full dimension scores above or change the publication verdict.

| Agent | Dimension | Basis | Baseline → With skill (uplift) | Explanation |
| --- | --- | --- | --- | --- |
| claude-code | Discoverability | Dimension comparison | N/A | Discoverability is not applicable to the no-skill baseline; there is no installed skill to discover. |
| claude-code | Efficiency | Token-only comparison | 96.9% → 95.4% (-1.5 points) | Only Token Efficiency is comparable; the full dimension scores use different evaluator sources. |
| codex | Discoverability | Dimension comparison | N/A | Discoverability is not applicable to the no-skill baseline; there is no installed skill to discover. |
| codex | Efficiency | Token-only comparison | 81.1% → 84.8% (+3.7 points) | Only Token Efficiency is comparable; the full dimension scores use different evaluator sources. |

## Token Usage

Actual Tier 3 execution usage is reported for every observed agent/case pair and both conditions.

| Agent | Dataset case | With skill | Without skill | Delta | Change | Coverage |
|---|---|---:|---:|---:|---:|---|
| claude-code | All cases | 2,432,192 | 1,240,357 | +1,191,835 | +96.09% | skill 14/14; base 14/14 |
| claude-code | absent-hardware-capability | 111,509 | 109,075 | +2,434 | +2.23% | skill 1/1; base 1/1 |
| claude-code | cyclic-hardware-reorder | 320,545 | 147,321 | +173,224 | +117.58% | skill 1/1; base 1/1 |
| claude-code | direct-polling | 186,916 | 112,328 | +74,588 | +66.40% | skill 1/1; base 1/1 |
| claude-code | explicit-template-rendering | 111,454 | 36,575 | +74,879 | +204.73% | skill 1/1; base 1/1 |
| claude-code | finite-tail | 111,953 | 71,899 | +40,054 | +55.71% | skill 1/1; base 1/1 |
| claude-code | incomplete-hardware-aggregates | 112,132 | 37,095 | +75,037 | +202.28% | skill 1/1; base 1/1 |
| claude-code | monotonic-hardware-reorder | 110,985 | 109,280 | +1,705 | +1.56% | skill 1/1; base 1/1 |
| claude-code | new-profile-generation | 228,915 | 146,147 | +82,768 | +56.63% | skill 1/1; base 1/1 |
| claude-code | pending-firmware-activation | 188,646 | 37,303 | +151,343 | +405.71% | skill 1/1; base 1/1 |
| claude-code | reorder-consumer-stalls | 270,550 | 144,834 | +125,716 | +86.80% | skill 1/1; base 1/1 |
| claude-code | rx-buffer-ownership | 149,122 | 72,265 | +76,857 | +106.35% | skill 1/1; base 1/1 |
| claude-code | simple-port-edit | 230,819 | 108,346 | +122,473 | +113.04% | skill 1/1; base 1/1 |
| claude-code | throughput-evidence | 150,094 | 36,629 | +113,465 | +309.77% | skill 1/1; base 1/1 |
| claude-code | udp-flow-mismatch | 148,552 | 71,260 | +77,292 | +108.46% | skill 1/1; base 1/1 |
| codex | All cases | 1,248,413 | 1,200,251 | +48,162 | +4.01% | skill 14/14; base 14/14 |
| codex | absent-hardware-capability | 76,769 | 78,753 | -1,984 | -2.52% | skill 1/1; base 1/1 |
| codex | cyclic-hardware-reorder | 189,593 | 63,858 | +125,735 | +196.90% | skill 1/1; base 1/1 |
| codex | direct-polling | 106,163 | 74,498 | +31,665 | +42.50% | skill 1/1; base 1/1 |
| codex | explicit-template-rendering | 42,465 | 13,621 | +28,844 | +211.76% | skill 1/1; base 1/1 |
| codex | finite-tail | 48,814 | 45,684 | +3,130 | +6.85% | skill 1/1; base 1/1 |
| codex | incomplete-hardware-aggregates | 68,848 | 30,567 | +38,281 | +125.24% | skill 1/1; base 1/1 |
| codex | monotonic-hardware-reorder | 81,047 | 104,645 | -23,598 | -22.55% | skill 1/1; base 1/1 |
| codex | new-profile-generation | 109,097 | 95,060 | +14,037 | +14.77% | skill 1/1; base 1/1 |
| codex | pending-firmware-activation | 66,851 | 38,571 | +28,280 | +73.32% | skill 1/1; base 1/1 |
| codex | reorder-consumer-stalls | 175,720 | 496,043 | -320,323 | -64.58% | skill 1/1; base 1/1 |
| codex | rx-buffer-ownership | 84,648 | 57,476 | +27,172 | +47.28% | skill 1/1; base 1/1 |
| codex | simple-port-edit | 72,362 | 57,466 | +14,896 | +25.92% | skill 1/1; base 1/1 |
| codex | throughput-evidence | 51,985 | 22,225 | +29,760 | +133.90% | skill 1/1; base 1/1 |
| codex | udp-flow-mismatch | 74,051 | 21,784 | +52,267 | +239.93% | skill 1/1; base 1/1 |
| ALL AGENTS | Dataset aggregate | 3,680,605 | 2,440,608 | +1,239,997 | +50.81% | skill 28/28; base 28/28 |

Prompt tokens include cached reads, so total tokens are `prompt + completion` (cached is not added twice). The Efficiency score uses `(prompt - cached) + completion`. Change is `100 * (with - without) / without`; negative means fewer tokens. N/A comparisons include their reason in the coverage column; coverage is never estimated.

## Tier Status

| Tier | Purpose | Status | Evidence |
|---|---|---|---|
| Tier 1 | Static validation | **PASSED WITH OBSERVATIONS** | 11 validator(s); 22 finding(s) |
| Tier 2 | Semantic deduplication | **PASSED** | 2 validator(s); 0 finding(s) |
| Tier 3 | Live agent evaluation | **PASS** | 2 agent(s); 14 task(s) |

## Findings and Observations

<details>
<summary>Show detailed findings and successful checks</summary>

- **MEDIUM** QUALITY/quality_correctness: No documented scripts in table format (`skills/daqiri-onboarding/SKILL.md`)
- **MEDIUM** QUALITY/quality_correctness: Instructions don't mention 'run_script' (`skills/daqiri-onboarding/SKILL.md`)
- **MEDIUM** QUALITY/quality_correctness: SKILL_SPEC recommended field missing: 'metadata.author' (`skills/daqiri-onboarding/SKILL.md`)
- **MEDIUM** QUALITY/quality_correctness: SKILL_SPEC recommended field missing: 'metadata.tags' (`skills/daqiri-onboarding/SKILL.md`)
- **MEDIUM** QUALITY/quality_efficiency: Deeply nested references in configuration.md (`skills/daqiri-onboarding/SKILL.md`)
- 17 additional finding(s) are available in the full evaluation artifacts.

</details>

## Scoring Methodology

<details>
<summary>Show dimension definitions, source signals, and thresholds</summary>

| Dimension | Question | Scored signals |
|---|---|---|
| Security | Is it safe to use? | `security` (100%) |
| Correctness | Is the answer correct? | `accuracy` (100%) |
| Discoverability | Was the right skill loaded when needed? | `skill_execution` (100%) |
| Effectiveness | Did the skill help complete the task? | `goal_accuracy` (50%) + `behavior_check` (50%) |
| Efficiency | Did it avoid wasted tool calls and token usage? | `skill_efficiency` (50%) + `token_efficiency` (50%) |

- Dimension bands: PASS at 50% or above; NEUTRAL from 40% to below 50%; FAIL below 40%.
- Overall Tier 3 lift: PASS at +5 points or more; FAIL at -10 points or less; values between those bands are NEUTRAL.
- Overall verdict: PASS only when every configured dimension passes for at least one supported agent. Lift is reported as diagnostic evidence and does not override this gate.
- The 50% attempt pass threshold is a separate per-task gate; it is not the dimension pass threshold.
- Effectiveness is the equal-weight mean of goal completion (`goal_accuracy`) and expected workflow adherence (`behavior_check`).
- Efficiency is 50% tool-call productivity (the backward-compatible `skill_efficiency` wire id) and 50% `token_efficiency`. Positive-case skill routing is scored under Discoverability, not Efficiency; a negative case without a routing target is N/A. N/A sources are omitted, remaining weights are renormalized, and the dimension is marked partial.

Signals present in this run:

- `security` (Security): unsafe operations, secret leakage, and unauthorized access.
- `skill_execution` (Skill Execution): whether the expected skill was selected, decoys were avoided, and the workflow executed.
- `skill_efficiency` (Tool Productivity): tool-call productivity (legacy wire id; routing is scored under Discoverability).
- `accuracy` (Accuracy): final-answer correctness against the reference answer.
- `goal_accuracy` (Goal Accuracy): whether the user's goal was achieved.
- `behavior_check` (Behavior Check): whether the expected workflow behavior was followed.
- `token_efficiency` (Token Efficiency): actual uncached prompt plus completion usage (50% of Efficiency).

</details>

## Freshness

Regenerate this benchmark when the skill, evaluation dataset, target agent/model, evaluator version, environment, or scoring policy changes.
