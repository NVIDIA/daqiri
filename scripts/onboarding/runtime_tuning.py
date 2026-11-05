# SPDX-License-Identifier: Apache-2.0
"""Per-setting write-ahead journal; restoration never overwrites detected drift."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from scripts.onboarding.common import Run, boot_id


class LinuxSettings:
    def __init__(self, run):
        self.run = run
        self.ports = {p["netdev"]: p for p in run.profile["ports"]}

    def command(self, argv):
        text, _ = self.run.capture(argv, "setting")
        return text.strip()

    def guard(self, resource):
        if resource == "gpu":
            uuid = self.command(["nvidia-smi", "-i", self.run.profile["gpu_uuid"], "--query-gpu=uuid", "--format=csv,noheader"])
            if uuid != self.run.profile["gpu_uuid"]:
                raise RuntimeError("GPU identity changed")
        else:
            port = self.ports[resource]
            base = Path("/sys/class/net") / resource
            if (base / "device").resolve().name != port["bdf"] or (base / "address").read_text().strip() != port["mac"] or (base / "phys_port_name").read_text().strip() != port["physical_port"]:
                raise RuntimeError("NIC/physical-port identity changed: " + resource)

    def read(self, resource, field):
        self.guard(resource)
        if resource == "gpu":
            # NVIDIA tools do not expose prior lock bounds reliably. Require declaration.
            raise RuntimeError("GPU policy readback unavailable")
        if field == "mtu":
            return int((Path("/sys/class/net") / resource / "mtu").read_text())
        if field == "mrrs":
            word = int(self.command(["setpci", "-s", self.ports[resource]["bdf"], "CAP_EXP+8.w"]), 16)
            return 128 << ((word >> 12) & 7)
        text = self.command(["ethtool", "-a", resource])
        result = {}
        for key, label in (("autoneg", "Autonegotiate"), ("rx", "RX"), ("tx", "TX")):
            match = re.search(r"^" + label + r":\s*(on|off)\s*$", text, re.M)
            if not match:
                raise RuntimeError("Incomplete pause readback")
            result[key] = match[1]
        return result

    def write(self, resource, field, value):
        self.guard(resource)
        if resource == "gpu":
            args = ["--reset-gpu-clocks"] if value == "default" else ["-lgc", value]
            self.command(["nvidia-smi", "-i", self.run.profile["gpu_uuid"], *args])
        elif field == "mtu":
            self.command(["ip", "link", "set", "dev", resource, "mtu", str(value)])
        elif field == "mrrs":
            bits = (value.bit_length() - 8) << 12
            self.command(["setpci", "-s", self.ports[resource]["bdf"], f"CAP_EXP+8.w={bits:04x}:7000"])
        else:
            self.command(["ethtool", "-A", resource, "autoneg", value["autoneg"], "rx", value["rx"], "tx", value["tx"]])


def apply(run, backend):
    run.require_current_boot()
    if run.state["journal"]:
        raise RuntimeError("Existing journal: restore or retain it; never replace the baseline")
    entries = []
    for resource, settings in run.profile.get("tuning", {}).items():
        backend.guard(resource)
        if resource == "gpu":
            if settings:
                entries.append({"key": "gpu/policy", "resource": "gpu", "field": "policy", "before": settings["before"], "target": settings["target"], "verification": "command-only"})
        else:
            for field, target in settings.items():
                entries.append({"key": resource + "/" + field, "resource": resource, "field": field,
                                "before": backend.read(resource, field), "target": target, "verification": "readback"})
    # Capture every baseline and commit it before the first write.
    for entry in entries:
        entry["status"] = "unchanged" if entry["before"] == entry["target"] else "planned"
        entry["boot"] = boot_id()
    run.state["journal"] = entries
    run.save()
    for entry in entries:
        if entry["status"] == "unchanged":
            continue
        entry["status"] = "applying"
        run.save()
        try:
            if entry["verification"] == "readback" and backend.read(entry["resource"], entry["field"]) != entry["before"]:
                raise RuntimeError("Setting changed since baseline: " + entry["key"])
            backend.write(entry["resource"], entry["field"], entry["target"])
            if entry["verification"] == "readback" and backend.read(entry["resource"], entry["field"]) != entry["target"]:
                raise RuntimeError("Tuning readback mismatch: " + entry["key"])
            entry["status"] = "applied"
            run.save()
        except BaseException:
            entry["status"] = "uncertain"
            run.save()
            raise


def restore(run, backend, keep=(), gpu_policy_confirmed=False, preview=False):
    entries = run.state["journal"]
    unknown = set(keep) - {e["key"] for e in entries}
    if unknown:
        raise ValueError("Unknown retention keys: " + ", ".join(sorted(unknown)))
    plan = [{"key": e["key"], "action": "keep" if e["key"] in keep or e["status"] == "retained" else "restore",
             "before": e["before"], "target": e["target"], "status": e["status"]} for e in entries]
    if preview:
        return plan
    failures = []
    # Restore independently: a failure on one resource must not prevent others.
    for entry in reversed(entries):
        if entry["status"] in ("unchanged", "restored", "retained"):
            continue
        try:
            resource, field = entry["resource"], entry["field"]
            backend.guard(resource)
            if entry["status"] == "planned":
                entry["status"] = "restored"  # No write was attempted.
                run.save()
                continue
            if resource == "gpu":
                if not gpu_policy_confirmed or entry["boot"] != boot_id():
                    raise RuntimeError("GPU policy is not observable: confirm no intervening policy change; after reboot resolve it manually")
                current = entry["target"]
            else:
                current = backend.read(resource, field)
            if current == entry["before"]:
                entry["status"] = "restored"
            elif current != entry["target"]:
                raise RuntimeError("Setting changed outside demo: " + entry["key"])
            elif entry["key"] in keep:
                entry["status"] = "retained"
            else:
                # Keep target and original value intact if an interrupted restore needs retrying.
                entry["status"] = "restoring"
                run.save()
                backend.write(resource, field, entry["before"])
                if resource != "gpu" and backend.read(resource, field) != entry["before"]:
                    raise RuntimeError("Restoration readback mismatch")
                entry["status"] = "restored"
            entry.pop("error", None)
        except Exception as error:
            entry["status"] = "conflict"
            entry["error"] = str(error)
            failures.append(entry["key"])
        run.save()
    if failures:
        raise RuntimeError("Recovery required for: " + ", ".join(failures))
    return plan


def resolve_gpu(run, backend, policy, reason, keep=False):
    """Record an explicit administrative resolution, never infer GPU lock bounds."""
    if not reason or not reason.strip():
        raise ValueError("Manual GPU resolution needs an evidence/reason statement")
    entry = next((e for e in run.state["journal"] if e["resource"] == "gpu"), None)
    if entry is None:
        raise ValueError("No GPU policy change in this run")
    backend.guard("gpu")
    expected = entry["target"] if keep else entry["before"]
    if policy != expected:
        raise ValueError("Declared policy does not match selected original/retained policy")
    entry.update(status="retained" if keep else "restored", resolution="manual-declaration",
                 resolution_reason=reason, resolution_boot=boot_id())
    entry.pop("error", None)
    run.save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("apply", "restore", "resolve-gpu"))
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--execute", action="store_true", help="Execute already-authorized writes; otherwise preview")
    parser.add_argument("--keep", action="append", default=[], help="Exact resource/field key to retain")
    parser.add_argument("--confirm-gpu-policy", action="store_true", help="User confirmed no intervening policy changes (same boot only)")
    parser.add_argument("--policy", help="Administratively verified current policy for resolve-gpu")
    parser.add_argument("--reason", help="Manual policy resolution evidence")
    args = parser.parse_args()
    run = Run(args.run_dir)
    with run.lock():
        if args.action == "resolve-gpu":
            if not args.execute:
                print("Preview: record manual GPU resolution", args.policy, args.reason)
                return
            resolve_gpu(run, LinuxSettings(run), args.policy, args.reason, "gpu/policy" in args.keep)
        elif args.action == "apply":
            run.require_current_boot()
            run.require("launch")
            if not args.execute:
                print(json.dumps(run.profile.get("tuning", {}), indent=2))
                return
            try:
                apply(run, LinuxSettings(run))
                run.record("tune", "passed")
            except BaseException:
                run.record("tune", "failed")
                raise
        else:
            print(json.dumps(restore(run, LinuxSettings(run), args.keep, args.confirm_gpu_policy, not args.execute), indent=2))


if __name__ == "__main__":
    main()
