# SPDX-License-Identifier: Apache-2.0
"""Failure injection for the Spark demo: no Docker, NIC, GPU or tuning required."""
from copy import deepcopy
import json
import subprocess
import sys

import pytest
import yaml

from scripts.onboarding.common import Run, atomic_json, load_profile
from scripts.onboarding import run_stage as stages
from scripts.onboarding.runtime_tuning import apply, restore
from scripts.onboarding.summarize_run import stable_samples, summarize


@pytest.fixture
def profile():
    return {"schema": 1, "platform": "dgx-spark", "image": "sha256:" + "a" * 64,
            "gpu_uuid": "GPU-test", "master_core": 0, "cable_confirmed": True,
            "ports": [{"netdev": "tx", "bdf": "0000:01:00.0", "mac": "02:00:00:00:00:01", "physical_port": "p0"},
                      {"netdev": "rx", "bdf": "0000:01:00.1", "mac": "02:00:00:00:00:02", "physical_port": "p1"}],
            "paths": [{"tx": "tx", "rx": "rx", "tx_core": 1, "rx_core": 2, "tx_worker": 3, "rx_worker": 4}],
            "raw": {"seconds": 15, "payload_size": 8000, "batch_size": 1024, "tx_buffers": 4096, "rx_buffers": 32768},
            "tuning": {"tx": {"mtu": 9000, "mrrs": 4096}, "rx": {"mtu": 9000}},
            "inference": {"enabled": False, "cpu_core": 5, "images": 256, "manifest": None}}


@pytest.fixture
def run(tmp_path, profile):
    return Run.create(tmp_path / "run", profile)


class Settings:
    def __init__(self):
        self.values = {("tx", "mtu"): 1500, ("tx", "mrrs"): 512, ("rx", "mtu"): 1500}
        self.writes = []
        self.fail = None

    def guard(self, resource):
        pass

    def read(self, resource, field):
        return self.values[(resource, field)]

    def write(self, resource, field, value):
        self.writes.append((resource, field, value))
        self.values[(resource, field)] = value
        if self.fail == (resource, field):
            raise RuntimeError("Injected failure after write")


def test_restore_only_this_run_and_selective_retention(run):
    backend = Settings()
    backend.values[("tx", "mrrs")] = 4096  # Pre-existing tuning is the baseline.
    apply(run, backend)
    original = deepcopy(run.state["journal"])
    restore(run, backend, keep=["tx/mtu"])
    assert backend.values == {("tx", "mtu"): 9000, ("tx", "mrrs"): 4096, ("rx", "mtu"): 1500}
    assert [e["before"] for e in run.state["journal"]] == [e["before"] for e in original]
    writes = list(backend.writes)
    restore(run, backend)
    assert backend.writes == writes  # Retained disposition and restore are idempotent.


def test_partial_apply_failure_recoverable(run):
    backend = Settings()
    backend.fail = ("tx", "mrrs")
    with pytest.raises(RuntimeError):
        apply(run, backend)
    assert run.state["journal"][1]["status"] == "uncertain"
    assert Run(run.path).state["journal"] == run.state["journal"]
    backend.fail = None
    restore(run, backend)
    assert backend.values == Settings().values


def test_external_drift_preserved_other_settings_restored(run):
    backend = Settings()
    apply(run, backend)
    backend.values[("tx", "mtu")] = 4000
    with pytest.raises(RuntimeError, match="Recovery required"):
        restore(run, backend)
    assert backend.values[("tx", "mtu")] == 4000
    assert backend.values[("rx", "mtu")] == 1500
    assert backend.values[("tx", "mrrs")] == 512


def test_failed_restore_retry_preserves_original_baseline(run):
    backend = Settings()
    apply(run, backend)
    backend.fail = ("rx", "mtu")
    with pytest.raises(RuntimeError):
        restore(run, backend)
    backend.fail = None
    restore(run, backend)
    assert backend.values == Settings().values
    assert all(e["status"] == "restored" for e in run.state["journal"])


def test_no_second_baseline_or_unknown_keep(run):
    backend = Settings()
    apply(run, backend)
    with pytest.raises(RuntimeError, match="Existing journal"):
        apply(run, backend)
    with pytest.raises(ValueError, match="Unknown retention"):
        restore(run, backend, ["unknown/mtu"])


def test_write_ahead_journal_failure_prevents_hardware_write(run, monkeypatch):
    backend = Settings()
    monkeypatch.setattr(run, "save", lambda: (_ for _ in ()).throw(OSError("Disk full")))
    with pytest.raises(OSError):
        apply(run, backend)
    assert backend.writes == []


def test_restore_preview_does_not_write(run):
    backend = Settings()
    apply(run, backend)
    before = list(backend.writes)
    assert restore(run, backend, ["tx/mtu"], preview=True)[0]["action"] == "keep"
    assert backend.writes == before


def test_gpu_policy_requires_confirmation_and_reboot_review(run, monkeypatch):
    backend = Settings()
    run.profile["tuning"] = {"gpu": {"before": "default", "target": "3003,3003"}}
    apply(run, backend)
    with pytest.raises(RuntimeError, match="Recovery required"):
        restore(run, backend)
    monkeypatch.setattr("scripts.onboarding.runtime_tuning.boot_id", lambda: "other-boot")
    with pytest.raises(RuntimeError):
        restore(run, backend, gpu_policy_confirmed=True)
    assert backend.values[("gpu", "policy")] == "3003,3003"


@pytest.mark.parametrize("mutation", [
    lambda p: p.update(image="latest"),
    lambda p: p["paths"][0].update(tx_core=0),
    lambda p: p["ports"][1].update(physical_port="p0"),
    lambda p: p["ports"][0].update(mac="<discover>"),
    lambda p: p["tuning"]["tx"].update(mrrs=999),
    lambda p: p["raw"].update(seconds=1),
])
def test_invalid_profiles_rejected_before_actions(tmp_path, profile, mutation):
    mutation(profile)
    path = tmp_path / "profile.yaml"
    path.write_text(yaml.safe_dump(profile))
    with pytest.raises((ValueError, KeyError)):
        load_profile(path)


def test_two_pair_generation_uses_unique_regions_and_correct_macs(run):
    second = deepcopy(run.profile["paths"][0])
    for i, key in enumerate(("tx_core", "rx_core", "tx_worker", "rx_worker"), 6):
        second[key] = i
    second.update(tx="tx2", rx="rx2")
    run.profile["ports"] += [{**run.profile["ports"][0], "netdev": "tx2", "bdf": "0002:01:00.0", "mac": "02:00:00:00:00:03"},
                             {**run.profile["ports"][1], "netdev": "rx2", "bdf": "0002:01:00.1", "mac": "02:00:00:00:00:04"}]
    run.profile["paths"].append(second)
    generated = stages.configs(run)
    cfg = generated["raw"]["daqiri"]["cfg"]
    assert len(cfg["interfaces"]) == 4
    assert len({r["name"] for r in cfg["memory_regions"]}) == 4
    assert generated["raw"]["bench_tx"][1]["eth_dst_addr"] == "02:00:00:00:00:04"
    assert generated["raw"]["bench_tx"][1]["udp_dst_port"] == 4097
    assert len(generated["teaching"]["daqiri"]["cfg"]["interfaces"]) == 2
    assert all(r["num_bufs"] == 32768 for r in cfg["memory_regions"] if "RX" in r["name"])


def test_traffic_prerequisite_failure_runs_no_commands(run, monkeypatch):
    monkeypatch.setattr(run, "capture", lambda *a, **k: pytest.fail("Must not execute"))
    with pytest.raises(RuntimeError, match="generate"):
        stages.traffic(run, "raw")
    with pytest.raises(RuntimeError, match="Inference disabled"):
        stages.artifacts(run)


def test_missing_artifact_manifest_blocks_inference(run):
    run.profile["inference"].update(enabled=True, manifest=str(run.path / "missing.json"))
    with pytest.raises(FileNotFoundError):
        stages.artifacts(run)


def test_reused_container_name_never_adopted(run, monkeypatch):
    run.record("discover", "passed")
    monkeypatch.setattr(run, "docker", lambda *a, **k: ("exists", 0))
    with pytest.raises(RuntimeError, match="collision"):
        stages.launch(run)
    assert run.state["container"] is None


def test_container_ownership_rejected(run, monkeypatch):
    run.state["container"] = "b" * 64
    info = {"Id": "b" * 64, "Image": run.profile["image"], "Config": {"Labels": {"daqiri.demo.run": "another-run"}}}
    monkeypatch.setattr(run, "docker", lambda *a, **k: (json.dumps([info]), 0))
    with pytest.raises(RuntimeError, match="ownership"):
        run.owned_container()


def test_reboot_blocks_dependent_stage_and_resume_invalidates_evidence(run, monkeypatch):
    run.record("raw", "passed", evidence="old")
    monkeypatch.setattr("scripts.onboarding.common.boot_id", lambda: "new-boot")
    monkeypatch.setattr(stages, "boot_id", lambda: "new-boot")
    with pytest.raises(RuntimeError, match="Boot changed"):
        run.require_current_boot()
    monkeypatch.setattr(stages, "LinuxSettings", lambda run: Settings())
    monkeypatch.setattr(stages, "discover", lambda run: run.record("discover", "passed"))
    monkeypatch.setattr(run, "docker", lambda *a, **k: ("", 0))
    stages.resume(run)
    assert "raw" not in run.state["stages"]
    assert run.state["history"][0]["stages"]["raw"]["evidence"] == "old"
    run.require_current_boot()


def test_concurrent_commands_cannot_mutate_same_run(run):
    with run.lock():
        other = Run(run.path)
        with pytest.raises(BlockingIOError):
            with other.lock():
                pytest.fail("Concurrent mutation")


def test_pid_reuse_never_signals_unrelated_process(run, monkeypatch):
    run.state["processes"] = [{"pid": 999, "start": "old", "boot": stages.boot_id()}]
    monkeypatch.setattr(stages, "process_start", lambda pid: "new")
    monkeypatch.setattr(stages.os, "killpg", lambda *a: pytest.fail("Unrelated process"))
    with pytest.raises(RuntimeError, match="PID was reused"):
        stages.stop_monitors(run)


def test_stable_samples_reject_short_or_discontinuous_traffic():
    def log(values):
        return "\n".join(f"tx_bytes_phy: = {v} Mbps" for v in values)
    assert len(stable_samples(log([0] + [190000] * 15 + [0]), "tx_bytes_phy")) == 11
    assert stable_samples(log([190000] * 8 + [0] + [190000] * 8), "tx_bytes_phy") == []


def test_summary_missing_output_and_counters_cannot_pass(tmp_path):
    evidence = {"stage": "raw", "paths": [{"tx": "tx", "rx": "rx"}], "boot": "old", "config_hash": "hash", "application_exit": 0,
                "before": {"tx": {"identity": {"physical_port": "p0"}, "stats": {}}}, "after": {}, "monitors": []}
    atomic_json(tmp_path / "evidence.json", evidence)
    result = summarize(tmp_path)
    assert not result["passed"]
    assert not result["checks"]["application_output_present"]
    assert not result["checks"]["all_alias_counters_present"]


def test_cli_help_has_no_hardware_side_effects():
    for module in ("run_stage", "runtime_tuning", "summarize_run"):
        result = subprocess.run([sys.executable, "-m", "scripts.onboarding." + module, "--help"], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr


def test_live_summary_computes_rates_and_exposes_shutdown_tail(tmp_path):
    stats = {k: 0 for k in ("tx_bytes_phy", "rx_bytes_phy", "tx_packets_phy", "rx_packets_phy", "rx_discards_phy", "rx_pause_ctrl_phy", "tx_pause_ctrl_phy", "rx_buffer_passed_thres_phy")}
    before = {n: {"identity": {"physical_port": group}, "stats": dict(stats)} for n, group in (("tx", "p0"), ("rx", "p1"))}
    after = deepcopy(before)
    after["tx"]["stats"].update(tx_bytes_phy=12345, tx_packets_phy=42)
    after["rx"]["stats"].update(rx_bytes_phy=12345, rx_packets_phy=42, rx_buffer_passed_thres_phy=1)
    monitors = []
    for name, counter in (("tx", "tx_bytes_phy"), ("rx", "rx_bytes_phy")):
        log = tmp_path / (name + ".log")
        log.write_text("\n".join(f"{counter}: = 190000 Mbps" for _ in range(15)))
        monitors.append({"command": ["mlnx_perf", "-i", name], "path": str(log), "exit": 0})
    atomic_json(tmp_path / "evidence.json", {"stage": "raw", "boot": "test", "config_hash": "hash", "paths": [{"tx": "tx", "rx": "rx"}], "application_exit": 0, "before": before, "after": after, "monitors": monitors})
    (tmp_path / "application.log").write_text("ibverbs RX port rx packets=40 cqe_errors=0 app_ring_full_drops=0 handoff_full_drops=0\nibverbs TX port tx packets=42\nTX complete: interface=tx_port_0 queue=0 packets=42 inflight=2\nRX complete: interface=rx_port_0 queue=0 packets=40\nWARN timing fallback\n")
    result = summarize(tmp_path)
    assert result["passed"]
    assert result["tx_phy_Gbps"]["median"] == 190
    assert result["threshold_events"]["rx"] == 1
    assert any("inflight=2" in line for line in result["counts_and_shutdown"])
    assert result["warnings"] == ["WARN timing fallback"]
    after["rx"]["stats"]["rx_discards_phy"] = 1
    saved = json.loads((tmp_path / "evidence.json").read_text())
    saved["after"] = after
    atomic_json(tmp_path / "evidence.json", saved)
    assert not summarize(tmp_path)["passed"]


def test_artifact_hash_failure_before_execution(run, monkeypatch):
    artifact = run.path / "artifact"
    artifact.write_text("modified")
    manifest = {"schema": 1, "image": run.profile["image"], "gpu_uuid": "GPU-test", "input_dtype": "float16", "output_dtype": "float32", "normalization": "pixel-minus-128-to-imagenet", "images": 256,
                "files": {role: {"path": str(artifact), "sha256": "a" * 64} for role in ("onnx", "engine", "pcap", "labels")}}
    path = run.path / "manifest.json"
    atomic_json(path, manifest)
    run.profile["inference"].update(enabled=True, manifest=str(path))
    monkeypatch.setattr(stages, "ROOT", run.path.parent)
    with pytest.raises(ValueError, match="hash mismatch"):
        stages.artifacts(run)


def test_active_container_blocks_restoration(run, monkeypatch):
    run.state["container"] = "owned"
    monkeypatch.setattr(run, "owned_container", lambda: "owned")
    monkeypatch.setattr(run, "docker", lambda *a, **k: ("COMMAND\nbash\ndaqiri_bench\n", 0))
    monkeypatch.setattr(stages, "restore", lambda *a, **k: pytest.fail("Do not restore with active DMA"))
    with pytest.raises(RuntimeError, match="active work"):
        stages.finish(run)


def test_finish_restoration_failure_keeps_container(run, monkeypatch):
    run.state["container"] = "owned"
    monkeypatch.setattr(run, "owned_container", lambda: "owned")
    commands = []
    def docker(*args, **kwargs):
        commands.append(args)
        return "COMMAND\nbash\n", 0
    monkeypatch.setattr(run, "docker", docker)
    monkeypatch.setattr(stages, "restore", lambda *a, **k: (_ for _ in ()).throw(RuntimeError("restore failed")))
    with pytest.raises(RuntimeError, match="restore failed"):
        stages.finish(run)
    assert all(cmd[0] != "attach" for cmd in commands)
    assert run.state["container"] == "owned"


def test_finish_only_closes_recorded_owned_container(run, monkeypatch):
    run.state["container"] = "owned"
    monkeypatch.setattr(run, "owned_container", lambda: "owned")
    commands = []
    def docker(*args, **kwargs):
        commands.append(args)
        if args[0] == "ps":
            return "unrelated-container\n", 0
        return "COMMAND\nbash\n", 0
    monkeypatch.setattr(run, "docker", docker)
    stages.finish(run)
    assert ("attach", "--sig-proxy=false", "owned") in commands
    assert not any("unrelated-container" in cmd for cmd in commands)
    assert run.state["exit"] == "finished"


def test_capture_nonzero_and_timeout_preserve_evidence(run):
    output, code = run.capture([sys.executable, "-c", "print('real output');raise SystemExit(3)"], "failure", required=False)
    assert code == 3 and "real output" in output
    with pytest.raises(subprocess.TimeoutExpired):
        run.capture([sys.executable, "-c", "import time;time.sleep(10)"], "timeout", timeout=0.05)
    statuses = [json.loads(p.read_text()) for p in run.path.glob("*/status.json")]
    assert {s["exit"] for s in statuses} == {3, None}


def test_manual_gpu_resolution_keeps_original_baseline(run):
    from scripts.onboarding.runtime_tuning import resolve_gpu
    backend = Settings()
    run.profile["tuning"] = {"gpu": {"before": "default", "target": "3003,3003"}}
    apply(run, backend)
    with pytest.raises(ValueError):
        resolve_gpu(run, backend, "default", "")
    with pytest.raises(ValueError):
        resolve_gpu(run, backend, "1000,1000", "reviewed")
    resolve_gpu(run, backend, "default", "Administrator restored original policy after reboot")
    entry = run.state["journal"][0]
    assert entry["status"] == "restored"
    assert entry["before"] == "default" and entry["target"] == "3003,3003"
    assert entry["resolution"] == "manual-declaration"


def test_profile_accepts_template_with_resolved_inputs(tmp_path, profile):
    p = tmp_path / "profile.yaml"
    p.write_text(yaml.safe_dump(profile))
    assert load_profile(p) == profile


def test_pause_restores_then_resume_does_not_require_restarting_talk(run, monkeypatch):
    backend = Settings()
    apply(run, backend)
    monkeypatch.setattr(stages, "LinuxSettings", lambda run: backend)
    stages.finish(run, pause=True)
    assert run.state["exit"] == "paused"
    assert backend.values == Settings().values
    monkeypatch.setattr(stages, "discover", lambda run: run.record("discover", "passed"))
    monkeypatch.setattr(run, "docker", lambda *a, **k: ("", 0))
    stages.resume(run)
    assert run.state["exit"] == "active"
    assert run.state["stages"]["discover"]["status"] == "passed"
    assert "generate" not in run.state["stages"]


def test_generate_preview_does_not_write_config_or_execute_commands(run):
    result = subprocess.run([sys.executable, "-m", "scripts.onboarding.run_stage", "generate", "--run-dir", str(run.path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert "UNVALIDATED PREVIEW" in result.stdout
    assert not list(run.path.rglob("*.yaml"))
    assert not list(run.path.glob("*/command.json"))


def test_stop_only_recorded_live_monitor(run):
    import os
    monitored = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"], start_new_session=True)
    unrelated = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"], start_new_session=True)
    try:
        run.state["processes"] = [{"pid": monitored.pid, "start": stages.process_start(monitored.pid), "boot": stages.boot_id()}]
        stages.stop_monitors(run)
        assert monitored.wait(timeout=5) != 0
        assert unrelated.poll() is None
        assert run.state["processes"] == []
    finally:
        if monitored.poll() is None:
            os.killpg(monitored.pid, 15)
            monitored.wait(timeout=5)
        unrelated.terminate()
        unrelated.wait(timeout=5)
