## Description: <br>
Guide DAQIRI setup, concepts, configuration generation, validation and repair, and first or resumed benchmarks. <br>

This skill is ready for commercial/non-commercial use. <br>

## Owner
NVIDIA <br>

### License/Terms of Use: <br>
Apache 2.0 <br>
## Use Case: <br>
Developers and engineers use this skill to set up DAQIRI, understand its concepts, choose between raw Ethernet, UDP/TCP, or RoCE, generate and validate configurations, diagnose saved logs or no-free-buffer errors, and run bounded first or resumed benchmarks and smoke tests, including explicitly requested customer demos when a demo package is available. <br>

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
- [Documentation and Helper Map](references/documentation-map.md) <br>
- [Onboarding Flow](references/onboarding-flow.md) <br>
- [DAQIRI Documentation](https://nvidia.github.io/daqiri/) <br>
- [DAQIRI Getting Started](https://nvidia.github.io/daqiri/getting-started/) <br>
- [DAQIRI Benchmarking](https://nvidia.github.io/daqiri/benchmarks/) <br>
- [DAQIRI Concepts](https://nvidia.github.io/daqiri/concepts/) <br>


## Skill Output: <br>
**Output Type(s):** [Analysis, Shell commands, Configuration instructions, Files] <br>
**Output Format:** [Markdown with inline bash and YAML code blocks] <br>
**Output Parameters:** [1D] <br>
**Other Properties Related to Output:** [None] <br>

## Evaluation Agents Used: <br>
- Codex (`gpt-6.1-sol`) <br>
- Claude (`claude-opus-5-5`) <br>



## Evaluation Tasks: <br>
8 evaluation tasks (7 positive, 1 negative), 1 attempt per task, each run with the skill and as a no-skill baseline. Dataset digest and environment were not recorded (legacy or non-live result). <br>

## Evaluation Metrics Used: <br>
Reported benchmark dimensions: <br>
- Overall: Aggregate score; producer-reported dimension; the historical scoring formula used by evaluator `daqiri-paired-cli-byoe-v4-external-minimal` is not recorded. <br>
- Security: Is it safe to use? Producer-reported dimension; the historical scoring formula used by evaluator `daqiri-paired-cli-byoe-v4-external-minimal` is not recorded. <br>
- Correctness: Is the answer correct? Producer-reported dimension; the historical scoring formula used by evaluator `daqiri-paired-cli-byoe-v4-external-minimal` is not recorded. <br>
- Discoverability: Was the right skill loaded when needed? Producer-reported dimension; the historical scoring formula used by evaluator `daqiri-paired-cli-byoe-v4-external-minimal` is not recorded. <br>
- Effectiveness: Did the skill help complete the task? Producer-reported dimension; the historical scoring formula used by evaluator `daqiri-paired-cli-byoe-v4-external-minimal` is not recorded. <br>
- Efficiency: Did it avoid wasted tool calls and token usage? Producer-reported dimension; the historical scoring formula used by evaluator `daqiri-paired-cli-byoe-v4-external-minimal` is not recorded. Not available in this run. <br>

Underlying evaluation signals used in this run: <br>
- `assertion_pass_rate`: Assertion Pass Rate: additional evaluator signal. <br>
- `task_success`: Task Success: additional evaluator signal. <br>
- `scope_preservation`: Scope Preservation: additional evaluator signal. <br>
- `skill_activation_alignment`: Skill Activation Alignment: additional evaluator signal. <br>



## Evaluation Results: <br>
### Results at a Glance

| Measure | Codex (Baseline → Skill Uplift) | Claude (Baseline → Skill Uplift) |
|---|---:|---:|
| Overall | 100.0% → 100.0% (±0.0 points) | 100.0% → 100.0% (±0.0 points) |
| Security | 100.0% → 100.0% (±0.0 points) | 100.0% → 100.0% (±0.0 points) |
| Correctness | 100.0% → 100.0% (±0.0 points) | 100.0% → 100.0% (±0.0 points) |
| Discoverability | 100.0% — baseline ran, but no comparable score was available; uplift unavailable | 100.0% — baseline ran, but no comparable score was available; uplift unavailable |
| Effectiveness | 100.0% → 100.0% (±0.0 points) | 100.0% → 100.0% (±0.0 points) |
| Efficiency | Not available | Not available |

**How to read this table:** baseline is the same task attempted without the target skill. Scores are rounded to one decimal; threshold-adjacent values use additional precision so their displayed band matches the verdict. Uplift is derived from those displayed scores and shown in percentage points.

A partial dimension was calculated from only the available configured signals; review the detailed report before relying on it.

Scoring definitions below are those of Skill Evaluator 1.6.9; older reports may use different formulas. Supplied scores are displayed unchanged.

### Tier Status

| Tier | Purpose | Status | Evidence |
|---|---|---|---|
| Tier 1 | Static validation | **PASSED WITH OBSERVATIONS** | 11 validator(s); 13 finding(s) |
| Tier 2 | Semantic deduplication | **PASSED** | 2 validator(s); 0 finding(s) |
| Tier 3 | Self-assessed evaluation | **SELF-ASSESSED** | Reported execution: completed; CI did not run Tier 3 |

### Evaluation Status

- Reported evaluation verdict: PASS (producer-reported)
- CI Quality assessment: SELF-ASSESSED
- Publication status: Publication eligible
- Evaluation source: self-assessed
- Reported execution: completed
- CI did not run Tier 3. Scores, comparisons, and evaluation verdicts above are producer-reported.

## Skill Version(s): <br>
fefc41a (source: git SHA, committed 2026-11-09) <br>

## Ethical Considerations: <br>
NVIDIA believes Trustworthy AI is a shared responsibility and we have established policies and practices to enable development for a wide array of AI applications. When downloaded or used in accordance with our terms of service, developers should work with their internal team to ensure this skill meets requirements for the relevant industry and use case and addresses unforeseen product misuse. <br>

(For Release on NVIDIA Platforms Only) <br>
Please report quality, risk, security vulnerabilities or NVIDIA AI Concerns [here](https://app.intigriti.com/programs/nvidia/nvidiavdp/detail). <br>
