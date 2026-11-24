## Description: <br>
Guide DAQIRI setup and configuration generation or repair, diagnose initialization failures, packet drops, RX-worker/application deficits, buffer errors and saved benchmark/counter evidence, explain DAQIRI memory paths, select polling and RX reorder modes, and handle explicitly requested customer demos. <br>

This skill is ready for commercial/non-commercial use. <br>

## Owner
NVIDIA <br>

### License/Terms of Use: <br>
Apache 2.0 <br>
## Use Case: <br>
Developers and engineers use this skill to set up DAQIRI, generate or repair its YAML configurations, run first benchmarks, and diagnose initialization, packet-drop, buffer and saved-evidence issues when streaming Ethernet data into CPU and GPU memory with NVIDIA NICs. <br>

### Deployment Geography for Use: <br>
Global <br>

## Requirements / Dependencies: <br>
**Requires API Key or External Credential:** [Not Specified] <br>
**Credential Type(s):** [None identified] <br>

Do not include secrets in prompts/logs/output; use least-privilege credentials; rotate keys as appropriate. <br>

## Known Risks and Mitigations: <br>
Risk: Review before execution as proposals could introduce incorrect or misleading guidance into skills. <br>
Mitigation: Review and scan skill before deployment. <br>

## Reference(s): <br>
- [Configuration Generation and Repair](references/configuration.md) <br>
- [Optional Customer Demo](references/customer-demo.md) <br>
- [Documentation and Helper Map](references/documentation-map.md) <br>
- [Evidence and Delivery Claims](references/evidence.md) <br>
- [First Benchmark](references/first-run.md) <br>
- [Raw ibverbs initialization limits](references/ibverbs-init-limits.md) <br>
- [Raw ibverbs / DevX RX Gaps](references/ibverbs-rx-gaps.md) <br>
- [Polling Mode Selection](references/polling-modes.md) <br>
- [Reorder Selection and Capability Diagnosis](references/reorder-selection.md) <br>
- [Setup and Runtime Selection](references/setup.md) <br>
- [Acquisition Failure Diagnosis](references/troubleshooting.md) <br>
- [DAQIRI Documentation](https://nvidia.github.io/daqiri/) <br>
- [DAQIRI Getting Started](https://nvidia.github.io/daqiri/getting-started/) <br>
- [DAQIRI Benchmarking](https://nvidia.github.io/daqiri/benchmarks/) <br>
- [DAQIRI Concepts](https://nvidia.github.io/daqiri/concepts/) <br>


## Skill Output: <br>
**Output Type(s):** [Analysis, Configuration instructions, Shell commands, Files] <br>
**Output Format:** [Markdown with inline bash code blocks and YAML configuration files] <br>
**Output Parameters:** [1D] <br>
**Other Properties Related to Output:** [None] <br>

## Evaluation Agents Used: <br>
- Claude Code (`aws/anthropic/bedrock-claude-opus-5-5`) <br>
- Codex (`openai/openai/gpt-6.1-sol`) <br>



## Evaluation Tasks: <br>
14 evaluation tasks (14 positive), 1 attempt per task, each task attempt run in its own isolated sandbox pod (`k8s-sandbox`); evaluation date 2026-11-24, evaluator version `1.6.10`. Baseline is the same task attempted without the target skill. <br>

## Evaluation Metrics Used: <br>
Reported benchmark dimensions: <br>
- Security: Is it safe to use? Scored from `security` (100%). <br>
- Correctness: Is the answer correct? Scored from `accuracy` (100%). <br>
- Discoverability: Was the right skill loaded when needed? Scored from `skill_execution` (100%). <br>
- Effectiveness: Did the skill help complete the task? Scored from `goal_accuracy` (50%) + `behavior_check` (50%). <br>
- Efficiency: Did it avoid wasted tool calls and token usage? Scored from `skill_efficiency` (50%) + `token_efficiency` (50%). <br>

Underlying evaluation signals used in this run: <br>
- `security`: Security: unsafe operations, secret leakage, and unauthorized access. <br>
- `skill_execution`: Skill Execution: whether the expected skill was selected, decoys were avoided, and the workflow executed. <br>
- `skill_efficiency`: Tool Productivity: tool-call productivity (legacy wire id; routing is scored under Discoverability). <br>
- `accuracy`: Accuracy: final-answer correctness against the reference answer. <br>
- `goal_accuracy`: Goal Accuracy: whether the user's goal was achieved. <br>
- `behavior_check`: Behavior Check: whether the expected workflow behavior was followed. <br>
- `token_efficiency`: Token Efficiency: actual uncached prompt plus completion usage (50% of Efficiency). <br>



## Evaluation Results: <br>
> ✅ **Overall verdict: PASS — Recommended for publication**

### Publication Recommendation

Recommended for publication based on the completed evaluation evidence in this report.

### Results at a Glance

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

### Tier Status

| Tier | Purpose | Status | Evidence |
|---|---|---|---|
| Tier 1 | Static validation | **PASSED WITH OBSERVATIONS** | 11 validator(s); 22 finding(s) |
| Tier 2 | Semantic deduplication | **PASSED** | 2 validator(s); 0 finding(s) |
| Tier 3 | Live agent evaluation | **PASS** | 2 agent(s); 14 task(s) |

## Skill Version(s): <br>
f750024 (source: git SHA, committed 2026-11-24) <br>

## Ethical Considerations: <br>
NVIDIA believes Trustworthy AI is a shared responsibility and we have established policies and practices to enable development for a wide array of AI applications. When downloaded or used in accordance with our terms of service, developers should work with their internal team to ensure this skill meets requirements for the relevant industry and use case and addresses unforeseen product misuse. <br>

(For Release on NVIDIA Platforms Only) <br>
Please report quality, risk, security vulnerabilities or NVIDIA AI Concerns [here](https://app.intigriti.com/programs/nvidia/nvidiavdp/detail). <br>
