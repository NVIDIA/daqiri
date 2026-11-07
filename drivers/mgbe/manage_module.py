#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Temporarily load the prototype; never overwrite the installed module.

Loading affects all ports owned by nvethernet. Refuse live/configured ports and
busy dependents. A durable journal makes rollback possible after interruption.
"""
import argparse
import glob
import json
import os
import pathlib
import re
import subprocess
import time

STOCK_SOURCE = "ACEB0B6366A461508EE22A8"


def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, capture_output=True)


def read(path):
    return pathlib.Path(path).read_text().strip()


def ports():
    found = []
    for p in pathlib.Path("/sys/class/net").iterdir():
        driver = p / "device/driver"
        if driver.exists() and driver.resolve().name == "nvethernet":
            found.extend(json.loads(run("ip", "-j", "address", "show", "dev", p.name).stdout))
    return found


def irq_affinity(interface):
    result = {}
    for line in read("/proc/interrupts").splitlines():
        if re.search(r"\b" + re.escape(interface) + r"[.\s]", line):
            irq = int(line.split(":", 1)[0].strip())
            result[line.split()[-1]] = (irq, read(f"/proc/irq/{irq}/smp_affinity_list"))
    return result


def feature_state(interface):
    result = {}
    for line in run("ethtool", "-k", interface).stdout.splitlines():
        match = re.match(r"\s*([a-z0-9_-]+):\s*(on|off)(.*)$", line)
        if match and "[fixed]" not in match[3]:
            result[match[1]] = match[2]
    return result


def runtime_state(interface):
    text = run("ethtool", "-c", interface).stdout
    coal = {}
    for key in ["rx-usecs", "rx-frames", "tx-usecs", "tx-frames"]:
        match = re.search(r"^" + key + r":\s*(\d+)\s*$", text, re.M)
        if not match:
            raise RuntimeError(f"cannot save {key} for {interface}")
        coal[key] = int(match[1])
    text = run("ethtool", "-g", interface).stdout.split("Current hardware settings:")[-1]
    rings = {}
    for direction in ["RX", "TX"]:
        match = re.search(r"^" + direction + r":\s*(\d+)\s*$", text, re.M)
        if not match:
            raise RuntimeError(f"cannot save {direction} ring size for {interface}")
        rings[direction.lower()] = int(match[1])
    return {"coalescing": coal, "rings": rings,
            "threaded": read(f"/sys/class/net/{interface}/threaded"),
            "irqs": {name: affinity for name, (_, affinity) in irq_affinity(interface).items()},
            "features": feature_state(interface)}


def journal(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    with temporary.open("w") as f:
        os.chmod(temporary, 0o600)
        json.dump(data, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    temporary.replace(path)
    directory_fd = os.open(path.parent, os.O_DIRECTORY | os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def restore(path):
    data = json.loads(path.read_text())
    if data["kernel"] != os.uname().release:
        raise RuntimeError("journal belongs to another kernel")
    parameter = pathlib.Path("/sys/module/nvethernet/parameters/pmd_interface")
    candidate = data.get("replacement_source")
    active = pathlib.Path("/sys/module/nvethernet")
    loaded = read(active / "srcversion") if active.exists() else None
    if parameter.exists() or (candidate and loaded == candidate):
        if candidate and loaded != candidate:
            raise RuntimeError("loaded module does not match this replacement journal")
        if parameter.exists() and parameter.read_text().strip() != data["interface"]:
            raise RuntimeError("another PMD instance is loaded")
        for p in ports():
            run("ip", "link", "set", p["ifname"], "down")
        run("rmmod", "nvethernet")  # Fails safely if any file/VMA still owns it.
    elif loaded is not None and loaded != STOCK_SOURCE:
        raise RuntimeError("another nvethernet replacement is loaded")
    if not pathlib.Path("/sys/module/nvethernet").exists():
        run("modprobe", "nvethernet", "macsec_enable=" + data["macsec"])
    for dependency in data["unloaded"]:
        run("modprobe", dependency)
    for p in data["ports"]:
        interface = p["ifname"]
        settings = p.get("runtime")
        run("ip", "link", "set", interface, "down")
        run("ip", "link", "set", p["ifname"], "mtu", str(p["mtu"]))
        if settings:
            run("ethtool", "-G", interface, *[str(x) for pair in settings["rings"].items() for x in pair])
            run("ethtool", "-C", interface, *[str(x) for pair in settings["coalescing"].items() for x in pair])
            pathlib.Path(f"/sys/class/net/{interface}/threaded").write_text(settings["threaded"])
            features = feature_state(interface)
            changed = {key: value for key, value in settings["features"].items()
                       if features.get(key) != value}
            if changed:
                # Feature callbacks may program hardware; apply with clocks up.
                run("ip", "link", "set", interface, "up")
                run("ethtool", "-K", interface, *[str(x) for pair in changed.items() for x in pair])
        run("ip", "link", "set", p["ifname"], "up" if "UP" in p["flags"] else "down")
        if settings:
            for name, (irq, _) in irq_affinity(interface).items():
                if name in settings["irqs"]:
                    pathlib.Path(f"/proc/irq/{irq}/smp_affinity_list").write_text(settings["irqs"][name])
    data["restored"] = True
    journal(path, data)


def load(module, interface, path, unload_idle_coe):
    if path.exists() and not json.loads(path.read_text()).get("restored"):
        raise RuntimeError("active journal exists; restore it before loading again")
    if run("modinfo", "-F", "srcversion", "nvethernet").stdout.strip() != STOCK_SOURCE:
        raise RuntimeError("installed stock module does not match the verified source")
    if pathlib.Path("/sys/module/nvethernet/parameters/pmd_interface").exists():
        raise RuntimeError("prototype already loaded")
    if read("/sys/module/nvethernet/srcversion") != STOCK_SOURCE:
        raise RuntimeError("loaded driver is not the verified stock module")
    if not module.is_file() or module.name != "nvethernet.ko":
        raise RuntimeError("expected built nvethernet.ko")
    if not run("modinfo", "-F", "vermagic", str(module)).stdout.startswith(os.uname().release + " "):
        raise RuntimeError("module kernel version mismatch")
    parameters = run("modinfo", "-F", "parm", str(module)).stdout
    if "pmd_interface:" not in parameters:
        raise RuntimeError("module has no PMD parameter")
    candidate = run("modinfo", "-F", "srcversion", str(module)).stdout.strip()
    current = ports()
    if interface not in {p["ifname"] for p in current}:
        raise RuntimeError("target is not an nvethernet interface")
    for p in current:
        if p.get("addr_info") or "LOWER_UP" in p["flags"]:
            raise RuntimeError(f"refusing to unload configured/live port {p['ifname']}")
        p["runtime"] = runtime_state(p["ifname"])
    holders = [p.name for p in pathlib.Path("/sys/module/nvethernet/holders").iterdir()]
    if holders and (holders != ["tegra_capture_coe"] or not unload_idle_coe):
        raise RuntimeError(f"driver holders: {holders}; idle CoE removal requires --unload-idle-coe")
    if holders:
        if read("/sys/module/tegra_capture_coe/refcnt") != "0":
            raise RuntimeError("CoE driver is in use")
        nodes = []
        for pattern in ["/dev/capture-*", "/dev/video*", "/dev/media*", "/dev/camchar*", "/dev/tegra_camera*"]:
            nodes.extend(glob.glob(pattern))
        if nodes and run("fuser", "-s", *nodes, check=False).returncode != 1:
            raise RuntimeError("camera devices are open, or camera-use check failed")
    data = {"kernel": os.uname().release, "interface": interface, "ports": current,
            "macsec": read("/sys/module/nvethernet/parameters/macsec_enable"),
            "unloaded": [], "restored": False, "module": str(module.resolve()),
            "replacement_source": candidate, "driver": "pmd"}
    journal(path, data)
    try:
        if holders:
            # Write intent before mutation, so interruption is recoverable.
            data["unloaded"] = holders
            journal(path, data)
            run("rmmod", "tegra_capture_coe")
        for p in current:
            run("ip", "link", "set", p["ifname"], "down")
        run("rmmod", "nvethernet")
        options = ["macsec_enable=" + data["macsec"]]
        options.append("pmd_interface=" + interface)
        run("insmod", str(module.resolve()), *options)
        node = pathlib.Path("/dev/mgbe-pmd-" + interface)
        for _ in range(50):
            ready = node.exists()
            if ready:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("replacement device was not created")
        run("ip", "link", "set", interface, "down")
        print(node)
    except BaseException:
        restore(path)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("inspect")
    l = sub.add_parser("load")
    l.add_argument("--module", type=pathlib.Path, required=True)
    l.add_argument("--interface", default="mgbe0_0")
    l.add_argument("--state", type=pathlib.Path, required=True)
    l.add_argument("--unload-idle-coe", action="store_true")
    r = sub.add_parser("restore")
    r.add_argument("--state", type=pathlib.Path, required=True)
    args = p.parse_args()
    if args.command == "inspect":
        print(json.dumps({"kernel": os.uname().release, "ports": ports(),
                          "stock_srcversion": run("modinfo", "-F", "srcversion", "nvethernet").stdout.strip()}, indent=2))
    elif os.geteuid() != 0:
        p.error("load/restore require root")
    elif args.command == "load":
        load(args.module, args.interface, args.state, args.unload_idle_coe)
    else:
        restore(args.state)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error) + ("\n" + error.stderr if isinstance(error, subprocess.CalledProcessError) else ""))
