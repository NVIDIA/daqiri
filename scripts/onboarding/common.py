# SPDX-License-Identifier: Apache-2.0
"""Durable run state, profile validation and command capture (no hardware at import)."""
from __future__ import annotations

from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import uuid

import yaml

ROOT = Path(__file__).resolve().parents[2]
STAGES = ("discover", "launch", "tune", "generate", "raw", "inference")


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def boot_id():
    return Path("/proc/sys/kernel/random/boot_id").read_text().strip()


def machine_id():
    return Path("/etc/machine-id").read_text().strip()


def atomic_json(path, data):
    path = Path(path)
    owner = path.stat() if path.exists() else path.parent.stat()
    fd, name = tempfile.mkstemp(dir=path.parent, prefix=".state-")
    try:
        if os.geteuid() == 0:
            os.fchown(fd, owner.st_uid, owner.st_gid)
        with os.fdopen(fd, "w") as out:
            json.dump(data, out, indent=2)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def load_profile(path):
    profile = yaml.safe_load(Path(path).read_text())
    if not isinstance(profile, dict) or profile.get("schema") != 1 or profile.get("platform") != "dgx-spark":
        raise ValueError("Expected schema 1 dgx-spark profile")
    if re.search(r"<[^>]+>", json.dumps(profile)):
        raise ValueError("Resolve all profile placeholders first")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", profile["image"]):
        raise ValueError("Pin the image by sha256 digest")
    if not re.fullmatch(r"GPU-[\w-]+", profile["gpu_uuid"]):
        raise ValueError("GPU UUID required")
    ports = profile["ports"]
    if len({p["netdev"] for p in ports}) != len(ports) or len({p["bdf"] for p in ports}) != len(ports):
        raise ValueError("Duplicate NIC identity")
    for p in ports:
        if not re.fullmatch(r"[\w.-]{1,15}", p["netdev"]) or not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]", p["bdf"]):
            raise ValueError("Invalid netdev/BDF")
        if not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", p["mac"]):
            raise ValueError("Invalid MAC")
    names = {p["netdev"] for p in ports}
    if not 1 <= len(profile["paths"]) <= 2 or len({p["physical_port"] for p in ports}) != 2:
        raise ValueError("Spark demo needs one/two paths between exactly two physical connectors")
    used = []
    cores = [profile["master_core"]]
    for p in profile["paths"]:
        if p["tx"] not in names or p["rx"] not in names or p["tx"] == p["rx"]:
            raise ValueError("Path must reference different profile ports")
        physical = {x["netdev"]: x["physical_port"] for x in ports}
        if physical[p["tx"]] == physical[p["rx"]]:
            raise ValueError("Cabled test needs distinct physical connectors")
        used.extend((p["tx"], p["rx"]))
        cores.extend(p[k] for k in ("tx_core", "rx_core", "tx_worker", "rx_worker"))
    directions = {(physical[p["tx"]], physical[p["rx"]]) for p in profile["paths"]}
    if len(directions) != 1:
        raise ValueError("All paths must use the same physical cable direction")
    inf = profile["inference"]
    if type(profile["cable_confirmed"]) is not bool:
        raise ValueError("cable_confirmed must be boolean")
    if type(inf["enabled"]) is not bool or inf["images"] != 256:
        raise ValueError("Inference enabled must be boolean; finite demo uses 256 images")
    if inf["enabled"] and (type(inf["cpu_core"]) is not int or inf["cpu_core"] in cores or inf["cpu_core"] < 0):
        raise ValueError("Inference CPU must be distinct from networking cores")
    if len(set(used)) != len(used):
        raise ValueError("Each NIC function may occur in only one demo path")
    if any(type(c) is not int or c < 0 for c in cores) or len(set(cores)) != len(cores):
        raise ValueError("Choose distinct nonnegative CPU cores")
    raw = profile["raw"]
    if not 64 <= raw["payload_size"] <= 8000 or not 1 <= raw["batch_size"] <= raw["tx_buffers"] <= raw["rx_buffers"]:
        raise ValueError("Invalid raw geometry/buffer counts")
    if raw["seconds"] < 10 or raw["seconds"] > 60:
        raise ValueError("Raw run must be 10–60 seconds")
    for field, value in profile.get("tuning", {}).items():
        if field not in names and field != "gpu":
            raise ValueError("Unknown tuning resource")
        if field != "gpu" and set(value) - {"mtu", "mrrs", "pause"}:
            raise ValueError("Unknown NIC tuning field")
        if field != "gpu":
            if "mtu" in value and not 576 <= value["mtu"] <= 9700:
                raise ValueError("MTU outside supported range")
            if "mrrs" in value and value["mrrs"] not in (128, 256, 512, 1024, 2048, 4096):
                raise ValueError("Invalid MRRS")
            if "pause" in value and (set(value["pause"]) != {"autoneg", "rx", "tx"} or any(v not in ("on", "off") for v in value["pause"].values())):
                raise ValueError("Explicit pause autoneg/rx/tx required")
        elif value and (not valid_policy(value.get("before")) or not valid_policy(value.get("target"))):
            raise ValueError("Declare GPU before/target policy; no inference from idle clocks")
    return profile


def valid_policy(value):
    return value == "default" or (isinstance(value, str) and bool(re.fullmatch(r"\d+,\d+", value)) and int(value.split(",")[0]) <= int(value.split(",")[1]))


class Run:
    def __init__(self, directory):
        self.path = Path(directory).resolve()
        self.state = json.loads((self.path / "run.json").read_text())
        if self.state["schema"] != 1 or self.state["machine"] != machine_id():
            raise ValueError("Run schema/host identity mismatch")
        self.profile = self.state["profile"]

    @classmethod
    def create(cls, directory, profile):
        path = Path(directory).resolve()
        path.mkdir(mode=0o700, parents=True, exist_ok=False)
        atomic_json(path / "run.json", {"schema": 1, "id": str(uuid.uuid4()), "machine": machine_id(),
                    "boot": boot_id(), "profile": profile, "profile_hash": hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest(),
                    "stages": {}, "journal": [], "container": None, "processes": [], "exit": "active"})
        return cls(path)

    def save(self):
        atomic_json(self.path / "run.json", self.state)

    @contextmanager
    def lock(self):
        with (self.path / ".lock").open("a") as lock:
            if os.geteuid() == 0:
                owner = self.path.stat()
                os.fchown(lock.fileno(), owner.st_uid, owner.st_gid)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                self.state = json.loads((self.path / "run.json").read_text())
                self.profile = self.state["profile"]
                yield self
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def require_current_boot(self):
        if self.state["boot"] != boot_id():
            raise RuntimeError("Boot changed: run resume inspection before dependent stages")
        if self.state["exit"] != "active":
            raise RuntimeError("Run is paused/finished; inspect and resume explicitly")

    def require(self, stage):
        if self.state["stages"].get(stage, {}).get("status") != "passed":
            raise RuntimeError(f"Prerequisite {stage} has not passed")

    def record(self, stage, status, **details):
        self.state["stages"][stage] = {"status": status, "utc_epoch": time.time(), **details}
        self.save()

    def capture(self, command, label, timeout=30, input_text=None, required=True):
        attempt = self.path / f"{label}-{uuid.uuid4().hex[:8]}"
        attempt.mkdir()
        if os.geteuid() == 0:
            owner = self.path.stat()
            os.chown(attempt, owner.st_uid, owner.st_gid)
        atomic_json(attempt / "command.json", {"argv": command, "boot": boot_id(), "started": time.time()})
        print("COMMAND", json.dumps(command), flush=True)
        code = None
        try:
            with (attempt / "output.log").open("w") as out:
                if os.geteuid() == 0:
                    owner = self.path.stat()
                    os.fchown(out.fileno(), owner.st_uid, owner.st_gid)
                result = subprocess.run(command, input=input_text, text=True, stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
                code = result.returncode
        finally:
            atomic_json(attempt / "status.json", {"exit": code, "finished": time.time()})
            print((attempt / "output.log").read_text(), end="", flush=True)
            print("EXIT", code, "EVIDENCE", attempt, flush=True)
        if required and code != 0:
            raise RuntimeError(f"{label} failed: {code}")
        return (attempt / "output.log").read_text(), code

    def docker(self, *args, **kwargs):
        return self.capture(["docker", *args], **kwargs)

    def owned_container(self):
        cid = self.state["container"]
        if not cid:
            raise RuntimeError("No recorded owned container")
        text, _ = self.docker("inspect", cid, label="container-identity")
        info = json.loads(text)[0]
        cfg = info["Config"]
        if info["Id"] != cid or cfg.get("Labels", {}).get("daqiri.demo.run") != self.state["id"] or info["Image"] != self.profile["image"]:
            raise RuntimeError("Container ownership/image mismatch")
        if cfg.get("User") != "0:0" or info.get("HostConfig", {}).get("NetworkMode") != "host":
            raise RuntimeError("Container user/network context mismatch")
        mount = next((m for m in info["Mounts"] if m["Destination"] == str(ROOT)), None)
        if not mount or mount["Source"] != str(ROOT):
            raise RuntimeError("Container checkout mount mismatch")
        if not info["State"]["Running"]:
            raise RuntimeError("Owned container is not running")
        return cid

    def cexec(self, *args, **kwargs):
        return self.docker("exec", self.owned_container(), *map(str, args), **kwargs)
