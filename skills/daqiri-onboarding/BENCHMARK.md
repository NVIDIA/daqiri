# DAQIRI onboarding BYOE

**PASS - self-assessed checklist and activation results.**

Evaluated inputs: `b04aa62d95d77e1829e553c2e14e1bc8ca0df73f`. Eight [cases](evals/evals.json), both agents, with/without the skill: 32 fresh sessions, one attempt per condition. Baselines use the same repository, docs, tools, inputs and permissions without the onboarding skill.

| Agent | Model | With skill | Baseline | Relevant activation | Negative control |
|---|---|---:|---:|---:|---|
| codex | gpt-6.1-sol | 8/8 | 8/8 | 7/7 | inactive |
| claude | claude-opus-5-5 | 8/8 | 8/8 | 7/7 | inactive |

Task scores and baseline comparisons are case-checklist results, not exhaustive answer-accuracy certification.

| Agent | Skill | Total tokens | Elapsed seconds |
|---|---|---:|---:|
| codex | with | 2,909,913 | 972.8 |
| codex | without | 2,385,182 | 832.1 |
| claude | with | 4,644,795 | 678.6 |
| claude | without | 4,653,239 | 748.8 |

CLI versions: Codex 0.167.0, Claude Code 2.1.319; xhigh effort. A team-run external harness used native discovery, fixed case order, parallel condition workers and serialized hardware tests. Tokens include cached context; elapsed time includes startup and tools.

Hardware coverage is GB10 host-pinned raw ibverbs loopback and paced cross-host TCP. Counts verify delivery, not payload integrity or sustained throughput. Other GPU-memory/DPDK/RoCE hardware paths were not tested. The saved diagnostic log is synthetic; the missing-hardware case passes by correctly reporting a blocker.

One attempt per case and a controlled skill set do not establish statistical significance or catalog-wide trigger reliability. Raw traces and runner code remain outside the skill. Security/signing remain NVSkills CI responsibilities.

Current findings:

- claude-with-skill-04-diagnose-log: The answer says the log's 'retains several outstanding bursts' means the pool's spare capacity got used up. The fixture does not quantify retained buffers or identify the exhausted pool; reported retention alone does not prove it exhausted packet-buffer capacity.
- claude-without-skill-07-raw-smoke: The answer opens with 'Every packet sent on det1 came back ... with no loss' and describes the 4,096 packets not consumed by the application at shutdown as 'not lost'. TX, hardware-loopback and RX-worker packet counts match, with zero selected error/drop increments. This supports no observed NIC count deficit, but does not establish payload identity or fully drained application delivery; the application consumed 4,096 fewer packets.
- claude-with-skill-07-raw-smoke: The answer says 'every packet sent came back ... nothing was dropped' and 'None were dropped' while disclosing an application count gap of 4,096. Matching TX/loopback/RX-worker counts and zero selected error/drop increments support no observed NIC count deficit. Payload identity and fully drained application delivery were not established; the application consumed 4,096 fewer packets.
