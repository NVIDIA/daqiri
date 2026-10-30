# SPDX-License-Identifier: Apache-2.0
"""Run one reviewed Spark demo stage; never advances a presentation checkpoint."""
from __future__ import annotations

import argparse
from copy import deepcopy
from dataclasses import asdict
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid

from scripts.daqiri_config import RawPairSpec, generate_raw_pair, render_document
from scripts.onboarding.common import ROOT, Run, atomic_json, boot_id, digest, load_profile
from scripts.onboarding.runtime_tuning import LinuxSettings, restore


def port_map(run):
    return {p["netdev"]: p for p in run.profile["ports"]}


def configs(run, on_spec=None):
    """Compose distinct pairs using the production generator, not copied YAML."""
    profile = run.profile
    ports = port_map(run)
    combined = None
    for index, path in enumerate(profile["paths"]):
        tx, rx = ports[path["tx"]], ports[path["rx"]]
        spec = RawPairSpec(tx_address=tx["bdf"], rx_address=rx["bdf"], master_core=profile["master_core"],
                           tx_queue_cores=(path["tx_core"],), rx_queue_cores=(path["rx_core"],),
                           tx_worker_cores=(path["tx_worker"],), rx_worker_cores=(path["rx_worker"],),
                           eth_src_addr=tx["mac"], eth_dst_addr=rx["mac"], engine="ibverbs", memory_kind="host_pinned",
                           payload_size=profile["raw"]["payload_size"], header_size=64,
                           num_bufs=profile["raw"]["tx_buffers"], batch_size=profile["raw"]["batch_size"], udp_port_base=4096 + index)
        if on_spec:
            on_spec(asdict(spec))
        doc = generate_raw_pair(spec)
        cfg = doc["daqiri"]["cfg"]
        names = {r["name"]: r["name"] + f"_{index}" for r in cfg["memory_regions"]}
        names.update({"tx_port": f"tx_port_{index}", "rx_port": f"rx_port_{index}"})
        def rename(value):
            if isinstance(value, str):
                return names.get(value, value)
            if isinstance(value, list):
                return [rename(v) for v in value]
            if isinstance(value, dict):
                return {k: rename(v) for k, v in value.items()}
            return value
        doc = rename(doc)
        cfg = doc["daqiri"]["cfg"]
        for region in cfg["memory_regions"]:
            if region["name"].startswith("Data_RX"):
                region["num_bufs"] = profile["raw"]["rx_buffers"]
        if combined is None:
            combined = doc
        else:
            for key in ("interfaces", "memory_regions"):
                combined["daqiri"]["cfg"][key].extend(cfg[key])
            for key in ("bench_tx", "bench_rx"):
                combined[key].extend(doc[key])
    teaching = deepcopy(combined)
    teaching["daqiri"]["cfg"]["interfaces"] = teaching["daqiri"]["cfg"]["interfaces"][:2]
    teaching["daqiri"]["cfg"]["memory_regions"] = teaching["daqiri"]["cfg"]["memory_regions"][:2]
    teaching["bench_tx"] = teaching["bench_tx"][:1]
    teaching["bench_rx"] = teaching["bench_rx"][:1]
    return {"teaching": teaching, "raw": combined}


def artifacts(run):
    inf = run.profile["inference"]
    if not inf["enabled"]:
        raise RuntimeError("Inference disabled: explicitly skip or prepare it outside the demo")
    manifest_path = Path(inf["manifest"])
    if not manifest_path.is_absolute():
        raise ValueError("Artifact manifest must use an absolute path")
    manifest = json.loads(manifest_path.read_text())
    expected = {"schema": 1, "image": run.profile["image"], "gpu_uuid": run.profile["gpu_uuid"],
                "input_dtype": "float16", "output_dtype": "float32", "normalization": "pixel-minus-128-to-imagenet", "images": 256}
    if any(manifest.get(k) != v for k, v in expected.items()) or inf["images"] != 256:
        raise ValueError("Artifact contract mismatch; prepare FP16 normalized 256-image artifacts first")
    if not {"onnx", "engine", "pcap", "labels"} <= manifest["files"].keys():
        raise ValueError("Incomplete artifact manifest")
    for entry in manifest["files"].values():
        p = Path(entry["path"])
        if not p.is_absolute() or not p.resolve().is_relative_to(ROOT):
            raise ValueError("Prepared artifacts must be under the mounted checkout")
        if digest(p) != entry["sha256"]:
            raise ValueError("Artifact hash mismatch: " + str(p))
    return manifest


def inference_config(run, manifest):
    doc = __import__("yaml").safe_load((ROOT / "applications/resnet50_inference/configs/resnet50_wire_loopback.yaml").read_text())
    cfg = doc["daqiri"]["cfg"]
    cfg.update(engine="ibverbs", master_core=run.profile["master_core"])
    path = run.profile["paths"][0]
    ports = port_map(run)
    for interface, role in zip(cfg["interfaces"], ("tx", "rx")):
        interface["address"] = ports[path[role]]["bdf"]
        interface[role]["queues"][0]["cpu_core"] = path[role + "_core"]
    cfg["interfaces"][1]["rx"]["queues"][0]["timeout_us"] = 100000
    doc["bench_tx"][0].update(cpu_core=path["tx_worker"], eth_src_addr=ports[path["tx"]]["mac"], eth_dst_addr=ports[path["rx"]]["mac"])
    doc["bench_rx"][0]["cpu_core"] = path["rx_worker"]
    doc["inference"].update(cpu_core=run.profile["inference"]["cpu_core"],
                            onnx_path=manifest["files"]["onnx"]["path"], engine_path=manifest["files"]["engine"]["path"])
    doc["dataset"] = {"pcap_path": manifest["files"]["pcap"]["path"], "labels_path": manifest["files"]["labels"]["path"]}
    return doc


def discover(run):
    settings = LinuxSettings(run)
    for port in run.profile["ports"]:
        settings.guard(port["netdev"])
    settings.guard("gpu")
    cores = {run.profile["master_core"]}
    for path in run.profile["paths"]:
        cores.update(path[k] for k in ("tx_core", "rx_core", "tx_worker", "rx_worker"))
    if not cores <= os.sched_getaffinity(0):
        raise RuntimeError("Selected cores are outside host-process affinity")
    print("Host-process CPUs:", sorted(os.sched_getaffinity(0)))
    run.docker("image", "inspect", run.profile["image"], label="image")
    run.capture(["git", "-C", str(ROOT), "rev-parse", "HEAD"], "source-version")
    run.capture(["nvidia-smi"], "gpu")
    run.capture(["ip", "-br", "address"], "addresses")
    run.capture(["ibdev2netdev", "-v"], "rdma")
    run.capture(["lspci", "-Dvv"], "pci")
    run.capture(["python3", str(ROOT / "python/tune_system.py"), "--check", "all", "--schematic-output", str(run.path / "topology.svg")], "system-checks", timeout=90)
    snapshot(run, "discovery")
    run.record("discover", "passed")


def launch_command(run):
    if not run.path.is_relative_to(ROOT):
        raise ValueError("Run directory must be under the mounted checkout")
    mounts = []
    inf = run.profile["inference"]
    if inf.get("enabled") and inf.get("manifest") and Path(inf["manifest"]).is_file():
        manifest = json.loads(Path(inf["manifest"]).read_text())
        for folder in sorted({str(Path(e["path"]).resolve().parent) for e in manifest["files"].values()}):
            parent = Path(folder)
            if not parent.is_relative_to(ROOT) or parent == ROOT or run.path.is_relative_to(parent):
                raise ValueError("Artifact mount must be a separate directory beneath checkout")
            mounts.extend(["--mount", f"type=bind,src={parent},dst={parent},readonly"])
    return ["docker", "run", "--pull", "never", "--rm", "-d", "-i", "--name", "daqiri-demo-" + run.state["id"],
            "--label", "daqiri.demo.run=" + run.state["id"], "--user", "0:0", "--privileged",
            "--runtime=nvidia", "--gpus", "all", "--network", "host",
            "--mount", "type=bind,src=/dev/hugepages,dst=/dev/hugepages",
            "--mount", f"type=bind,src={ROOT},dst={ROOT}", "--workdir", str(ROOT),
            *mounts, "--entrypoint", "/bin/bash", run.profile["image"]]


def launch(run):
    run.require("discover")
    if run.state["container"]:
        raise RuntimeError("Container already recorded; inspect/resume rather than launch another")
    name = "daqiri-demo-" + run.state["id"]
    _, exists = run.docker("container", "inspect", name, label="name-check", required=False)
    if exists == 0:
        raise RuntimeError("Container name collision; inspect ownership before recovery")
    text, _ = run.capture(launch_command(run), "launch")
    cid = text.strip()
    if len(cid) != 64 or any(c not in "0123456789abcdef" for c in cid):
        raise RuntimeError("Unexpected container ID; inspect labeled container before any retry")
    run.state["container"] = cid
    run.save()
    run.cexec("id", label="root")
    run.cexec("nvidia-smi", label="container-gpu")
    text, _ = run.cexec("python3", "-c", "import os,json;print(json.dumps(sorted(os.sched_getaffinity(0))))", label="container-cpus")
    cpus = set(json.loads(text))
    required = {run.profile["master_core"]}
    for path in run.profile["paths"]:
        required.update(path[k] for k in ("tx_core", "rx_core", "tx_worker", "rx_worker"))
    if not required <= cpus:
        raise RuntimeError("Selected CPUs unavailable in container")
    text, _ = run.cexec("/opt/daqiri/bin/daqiri_config_validate", "--list-engines", label="engines")
    if "ibverbs" not in text.split():
        raise RuntimeError("Image lacks required ibverbs engine")
    run.record("launch", "passed")


def generate(run):
    run.require("launch")
    folder = run.path / ("configs-" + uuid.uuid4().hex[:8])
    folder.mkdir()
    paths = {}
    specs = []
    def record_spec(spec):
        specs.append(spec)
        print("PRODUCTION GENERATOR INPUT", json.dumps(spec, indent=2))
    documents = configs(run, record_spec)
    atomic_json(folder / "generation.json", {"generator": "scripts.daqiri_config.generate_raw_pair", "specs": specs, "profile_hash": run.state["profile_hash"]})
    for name, doc in documents.items():
        target = folder / (name + ".yaml")
        target.write_text(render_document(doc))
        print(target.read_text())
        run.cexec("/opt/daqiri/bin/daqiri_config_validate", str(target), label="validate-" + name)
        paths[name] = {"path": str(target), "sha256": digest(target)}
    run.record("generate", "passed", configs=paths, scope="config-validated, hardware untested")


def snapshot(run, label):
    data = {}
    for port in run.profile["ports"]:
        name = port["netdev"]
        LinuxSettings(run).guard(name)
        text, _ = run.capture(["ethtool", "-S", name], label + "-" + name)
        stats = {}
        for line in text.splitlines():
            key, sep, value = line.strip().partition(":")
            if sep and value.strip().isdigit():
                stats[key] = int(value)
        data[name] = {"identity": port, "stats": stats, "carrier": (Path("/sys/class/net") / name / "carrier").read_text().strip(),
                      "physical_switch": (Path("/sys/class/net") / name / "phys_switch_id").read_text().strip(),
                      "physical_port": (Path("/sys/class/net") / name / "phys_port_name").read_text().strip()}
    return data


def process_start(pid):
    # comm may contain whitespace or parentheses; fields after the final ')' start at field 3.
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


def stop_monitors(run):
    failures = []
    for child in run.state["processes"]:
        if child["boot"] != boot_id():
            continue
        try:
            if process_start(child["pid"]) != child["start"]:
                raise RuntimeError("PID was reused")
            if os.getpgid(child["pid"]) != child["pid"]:
                raise RuntimeError("Process group changed")
            os.killpg(child["pid"], signal.SIGTERM)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                try:
                    stat = Path(f"/proc/{child['pid']}/stat").read_text().rsplit(")", 1)[1].split()
                    if stat[0] == "Z":
                        break
                    if stat[19] != child["start"]:
                        raise RuntimeError("PID was reused during shutdown")
                except FileNotFoundError:
                    break
                time.sleep(0.05)
            else:
                os.killpg(child["pid"], signal.SIGKILL)
                raise RuntimeError("Monitor needed forced termination; inspect before retrying cleanup")
        except FileNotFoundError:
            continue
        except Exception as error:
            failures.append(str(error))
    if failures:
        raise RuntimeError("Monitor identity conflict: " + "; ".join(failures))
    run.state["processes"] = []
    run.save()


def traffic(run, stage):
    run.require("generate")
    if run.state["stages"].get("tune", {}).get("status") not in ("passed", "skipped"):
        raise RuntimeError("Apply or explicitly skip tuning first")
    if not run.profile.get("cable_confirmed"):
        raise RuntimeError("Confirm physical cable endpoints before traffic")
    if run.state["processes"]:
        raise RuntimeError("Recover previous monitor processes before traffic")
    manifest = None
    if stage == "inference":
        run.require("raw")
        manifest = artifacts(run)
        info_text, _ = run.docker("inspect", run.owned_container(), label="artifact-mounts")
        mounts = json.loads(info_text)[0]["Mounts"]
        for entry in manifest["files"].values():
            candidates = [m for m in mounts if Path(entry["path"]).is_relative_to(m["Destination"])]
            deepest = max(candidates, key=lambda m: len(m["Destination"]), default=None)
            if not deepest or deepest["RW"]:
                raise RuntimeError("Inference artifacts must be mounted read-only; prepare mounts before launch")
        target = run.path / ("inference-" + uuid.uuid4().hex[:8] + ".yaml")
        target.write_text(render_document(inference_config(run, manifest)))
    else:
        item = run.state["stages"]["generate"]["configs"]["raw"]
        target = Path(item["path"])
        if digest(target) != item["sha256"]:
            raise RuntimeError("Config changed since validation; regenerate/revalidate")
    print(target.read_text())
    run.cexec("/opt/daqiri/bin/daqiri_config_validate", str(target), label="traffic-validation")
    # Revalidate port identity, live link and jumbo MTU immediately before sending.
    required_mtu = run.profile["raw"]["payload_size"] + 64 - 14 if stage == "raw" else 1500
    for name in {p[k] for p in run.profile["paths"] for k in ("tx", "rx")}:
        base = Path("/sys/class/net") / name
        LinuxSettings(run).guard(name)
        if (base / "carrier").read_text().strip() != "1" or int((base / "mtu").read_text()) < required_mtu:
            raise RuntimeError("Link/MTU prerequisite not satisfied: " + name)
    directory = run.path / (stage + "-" + uuid.uuid4().hex[:8])
    directory.mkdir()
    evidence = {"stage": stage, "paths": run.profile["paths"], "boot": boot_id(), "config_hash": digest(target), "before": snapshot(run, stage + "-before"), "monitors": [], "application_exit": None, "after": {}, "cleanup_errors": [], "run_id": run.state["id"], "image": run.profile["image"]}
    atomic_json(directory / "evidence.json", evidence)
    children = []
    try:
        # One representative per physical connector, all aliases captured in snapshots.
        representatives = {}
        for p in run.profile["ports"]:
            representatives.setdefault(p["physical_port"], p["netdev"])
        monitor_seconds = run.profile["raw"]["seconds"] + 15 if stage == "raw" else 130
        commands = [["mlnx_perf", "-i", name, "-t", "1", "-c", str(monitor_seconds)] for name in representatives.values()]
        commands += [["timeout", str(monitor_seconds), "nvidia-smi", "--query-gpu=timestamp,clocks.sm,utilization.gpu", "--format=csv", "--loop-ms=100"]]
        for index, cmd in enumerate(commands):
            logfile = directory / f"monitor-{index}.log"
            print("MONITOR", json.dumps(cmd), flush=True)
            stream = logfile.open("w")
            try:
                child = subprocess.Popen(cmd, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            finally:
                stream.close()
            children.append(child)
            evidence["monitors"].append({"command": cmd, "path": str(logfile)})
            atomic_json(directory / "evidence.json", evidence)
            run.state["processes"].append({"pid": child.pid, "start": process_start(child.pid), "boot": boot_id()})
            run.save()
        binary = "daqiri_bench_raw_gpudirect" if stage == "raw" else "daqiri_resnet50_inference"
        args = ["--seconds", str(run.profile["raw"]["seconds"])] if stage == "raw" else ["--replay-once", "--expected-images", "256"]
        evidence["application_start"] = time.time()
        output, code = run.cexec("timeout", "--signal=INT", "--kill-after=10s", "120s", "/opt/daqiri/bin/" + binary, str(target), *args,
                                 label=stage + "-application", timeout=145, required=False)
        evidence["application_end"] = time.time()
        (directory / "application.log").write_text(output)
        evidence["application_exit"] = code
        atomic_json(directory / "evidence.json", evidence)
        # Preserve natural completion/exit codes for PHY monitors (never report killed monitors as successful).
        for child in children[:-1]:
            child.wait(timeout=140)
    finally:
        try:
            stop_monitors(run)
        except Exception as error:
            evidence["cleanup_errors"].append(str(error))
        for index, child in enumerate(children):
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=5)
            evidence["monitors"][index]["exit"] = child.returncode
        try:
            evidence["after"] = snapshot(run, stage + "-after")
        except Exception as error:
            evidence["cleanup_errors"].append(str(error))
        if manifest:
            try:
                evidence["artifact_manifest"] = manifest
                evidence["artifacts_unchanged"] = artifacts(run) == manifest
            except Exception:
                evidence["artifacts_unchanged"] = False
        atomic_json(directory / "evidence.json", evidence)
    from scripts.onboarding.summarize_run import summarize
    result = summarize(directory)
    atomic_json(directory / "summary.json", result)
    print("RUN SUMMARY", json.dumps(result, indent=2), flush=True)
    run.record(stage, "passed" if result["passed"] else "failed", evidence=str(directory))
    if not result["passed"]:
        raise RuntimeError("Stage evidence incomplete/failed; inspect summary before continuing")


def resume(run):
    changed_boot = run.state["boot"] != boot_id()
    if run.state["processes"]:
        stop_monitors(run)
    # Re-discover and revalidate: a checkpoint is evidence, not permission or current state.
    for p in run.profile["ports"]:
        LinuxSettings(run).guard(p["netdev"])
    LinuxSettings(run).guard("gpu")
    old = deepcopy(run.state["stages"])
    run.state.setdefault("history", []).append({"boot": run.state["boot"], "stages": old})
    run.state["stages"] = {}
    run.state["boot"] = boot_id()
    run.state["exit"] = "active"
    run.save()
    discover(run)
    if not run.state["container"]:
        listing, _ = run.docker("ps", "-aq", "--no-trunc", "--filter", "label=daqiri.demo.run=" + run.state["id"], label="resume-owned-label")
        candidates = listing.split()
        if len(candidates) > 1:
            raise RuntimeError("Multiple containers claim this run label; inspect before recovery")
        if candidates:
            run.state["container"] = candidates[0]
            try:
                run.owned_container()
            except Exception:
                run.state["container"] = None
                raise
            run.save()
    if run.state["container"]:
        try:
            run.owned_container()
        except Exception:
            listing, _ = run.docker("ps", "-aq", "--no-trunc", label="resume-container-list")
            if run.state["container"] in listing.split():
                run.state["exit"] = "recovery-required"
                run.save()
                raise RuntimeError("Recorded container exists but is unavailable/mismatched; resolve ownership before relaunch")
            run.state.setdefault("prior_containers", []).append(run.state["container"])
            run.state["container"] = None
            run.save()
        if run.state["container"]:
            text, _ = run.cexec("/opt/daqiri/bin/daqiri_config_validate", "--list-engines", label="resume-engines")
            if "ibverbs" not in text.split():
                raise RuntimeError("Resumed container lacks ibverbs")
            run.record("launch", "passed", resumed=True)
    if run.state["journal"]:
        for e in run.state["journal"]:
            if e["status"] in ("restored", "unchanged", "retained"):
                continue
            if e["resource"] == "gpu" or changed_boot:
                raise RuntimeError("Review/restore outstanding tuning before resuming after boot/policy change")
            if LinuxSettings(run).read(e["resource"], e["field"]) != e["target"]:
                raise RuntimeError("Tuning drift: resolve journal before continuing")
        run.record("tune", "passed", resumed=True)
    print("Resume inspection complete. Regenerate/revalidate configs; old traffic remains historical.")


def finish(run, keep=(), pause=False, gpu_confirm=False):
    stop_monitors(run)
    cid = run.state["container"]
    if cid:
        run.owned_container()
        top, _ = run.docker("top", cid, "-eo", "comm", label="idle-check")
        if any(line.strip() not in ("COMMAND", "bash") for line in top.splitlines()):
            raise RuntimeError("Container has active work; wait for bounded application exit before restoration")
    restore(run, LinuxSettings(run), keep, gpu_confirm)
    if not pause and cid:
        run.docker("attach", "--sig-proxy=false", cid, label="shell-exit", input_text="exit\n", timeout=20)
        for _ in range(10):
            text, _ = run.docker("ps", "-aq", "--no-trunc", label="removal-check")
            if cid not in text.split():
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("Owned container removal not confirmed")
        run.state["container"] = None
    run.state["exit"] = "paused" if pause else "finished"
    statuses = {name: item["status"] for name, item in run.state["stages"].items()}
    prerequisites = all(statuses.get(name) == "passed" for name in ("discover", "launch", "generate", "raw"))
    outcome = "early-exit"
    if prerequisites and statuses.get("inference") == "passed":
        outcome = "full-demo"
    elif prerequisites and statuses.get("inference") == "skipped":
        outcome = "raw-only"
    run.state["outcome"] = "paused" if pause else outcome
    if run.state["stages"].get("tune", {}).get("status") == "passed":
        run.state["stages"]["tune"]["disposition"] = "see-journal"
    run.save()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("stage", choices=("init", "status", "discover", "launch", "generate", "raw", "inference", "skip", "resume", "finish"))
    p.add_argument("--run-dir", required=True)
    p.add_argument("--profile")
    p.add_argument("--execute", action="store_true", help="Run already-authorized privileged stage; default is preview")
    p.add_argument("--skip-stage", choices=("tune", "inference"))
    p.add_argument("--reason")
    p.add_argument("--keep", action="append", default=[])
    p.add_argument("--pause", action="store_true")
    p.add_argument("--confirm-gpu-policy", action="store_true")
    args = p.parse_args()
    if args.stage == "init":
        if not args.profile:
            p.error("init requires --profile")
        run = Run.create(args.run_dir, load_profile(args.profile))
        print("Created", run.path)
        return
    run = Run(args.run_dir)
    with run.lock():
        if args.stage == "status":
            print(json.dumps(run.state, indent=2))
            return
        if args.stage == "skip":
            if not args.reason or not args.skip_stage:
                p.error("skip requires --skip-stage and --reason")
            if args.skip_stage == "tune" and run.state["journal"]:
                raise RuntimeError("Resolve existing tuning journal before skipping")
            run.record(args.skip_stage, "skipped", reason=args.reason)
            return
        if args.stage == "generate" and not args.execute:
            for name, document in configs(run, lambda spec: print("GENERATOR INPUT", json.dumps(spec, indent=2))).items():
                print("UNVALIDATED PREVIEW", name, "\n" + render_document(document))
            return
        if args.stage in ("launch", "raw", "inference", "finish", "resume") and not args.execute:
            proposal = launch_command(run) if args.stage == "launch" else {"stage": args.stage, "profile": run.profile, "retention": args.keep}
            print(json.dumps(proposal, indent=2))
            return
        if args.stage not in ("finish", "resume"):
            run.require_current_boot()
        try:
            if args.stage in ("raw", "inference"):
                traffic(run, args.stage)
            elif args.stage == "finish":
                finish(run, args.keep, args.pause, args.confirm_gpu_policy)
            else:
                globals()[args.stage](run)
        except BaseException as error:
            if args.stage == "finish":
                run.state["exit"] = "recovery-required"
            run.record(args.stage, "failed", error=str(error))
            raise


if __name__ == "__main__":
    main()
