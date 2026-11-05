# SPDX-License-Identifier: Apache-2.0
"""Summarize one explicit live-run evidence directory; missing evidence fails closed."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import statistics


def stable_samples(text, counter):
    values = [float(x.replace(",", "")) / 1000 for x in re.findall(r"^\s*" + counter + r":.*?=\s*([\d,.]+)\s*Mbps", text, re.M)]
    active = [i for i, v in enumerate(values) if v > 1]
    if not active or active != list(range(active[0], active[-1] + 1)):
        return []
    return values[active[0]:active[-1] + 1][2:-2]


def summarize(directory):
    directory = Path(directory)
    e = json.loads((directory / "evidence.json").read_text())
    output = (directory / "application.log").read_text() if (directory / "application.log").exists() else ""
    checks = {"application_exit_zero": e.get("application_exit") == 0, "application_output_present": bool(output)}
    result = {"evidence": str(directory.resolve()), "stage": e["stage"], "boot": e["boot"],
              "config_hash": e["config_hash"], "run_id": e.get("run_id"), "image": e.get("image"),
              "application_start_epoch": e.get("application_start"), "application_end_epoch": e.get("application_end"), "checks": checks, "scope": "cabled PHY; one alias per connector; RX confirms TX, never add them"}
    deltas = {}
    required = ("tx_bytes_phy", "rx_bytes_phy", "tx_packets_phy", "rx_packets_phy", "rx_discards_phy", "rx_pause_ctrl_phy", "tx_pause_ctrl_phy")
    checks["all_alias_counters_present"] = True
    for name, before in e["before"].items():
        after = e["after"].get(name, {})
        if before["identity"] != after.get("identity") or not set(required) <= before["stats"].keys() or not set(required) <= after.get("stats", {}).keys():
            checks["all_alias_counters_present"] = False
            continue
        deltas[name] = {k: v - before["stats"].get(k, v) for k, v in after["stats"].items()}
    checks["capture_cleanup_complete"] = not e.get("cleanup_errors", [])
    checks["counters_monotonic"] = bool(deltas) and all(v >= 0 for row in deltas.values() for v in row.values())
    checks["no_pause_or_discards"] = bool(deltas) and all(row.get(k) == 0 for row in deltas.values() for k in ("rx_discards_phy", "rx_pause_ctrl_phy", "tx_pause_ctrl_phy"))
    physical = {}
    for name, before in e["before"].items():
        physical.setdefault(before["identity"]["physical_port"], []).append(name)
    checks["physical_aliases_agree"] = all(all(deltas.get(names[0], {}).get(k) == deltas.get(n, {}).get(k) for n in names for k in required[:4]) for names in physical.values())
    paths = e["paths"]
    checks["wire_delivery"] = bool(deltas) and all(
        deltas.get(p["tx"], {}).get("tx_bytes_phy", 0) > 0 and
        deltas.get(p["tx"], {}).get("tx_bytes_phy") == deltas.get(p["rx"], {}).get("rx_bytes_phy") and
        deltas.get(p["tx"], {}).get("tx_packets_phy") == deltas.get(p["rx"], {}).get("rx_packets_phy") for p in paths)
    result["counter_deltas"] = deltas
    result["threshold_events"] = {n: row.get("rx_buffer_passed_thres_phy") for n, row in deltas.items()}
    phy_monitors = [m for m in e["monitors"] if m["command"][0] == "mlnx_perf"]
    checks["phy_monitors_exit_zero"] = len(phy_monitors) == len(physical) and all(m.get("exit") == 0 for m in phy_monitors)
    for direction, counter in (("tx", "tx_bytes_phy"), ("rx", "rx_bytes_phy")):
        representative = next((m for m in phy_monitors if m["command"][2] == paths[0][direction]), None)
        # A representative can be another alias of the same physical port.
        if representative is None:
            group = e["before"].get(paths[0][direction], {}).get("identity", {}).get("physical_port")
            representative = next((m for m in phy_monitors if m["command"][2] in physical.get(group, [])), None)
        samples = stable_samples(Path(representative["path"]).read_text(), counter) if representative and Path(representative["path"]).exists() else []
        result[direction + "_phy_Gbps"] = {"samples": samples, "median": statistics.median(samples) if samples else None,
                                           "min": min(samples) if samples else None, "max": max(samples) if samples else None}
        if e["stage"] == "raw":
            checks[direction + "_ten_stable_samples"] = len(samples) >= 10
    indicators = ("cqe_errors", "app_ring_full_drops", "handoff_full_drops")
    queue_lines = re.findall(r"ibverbs (?:RX|TX) port .*", output)
    checks["queue_health_present"] = len(queue_lines) >= 2 * len(paths)
    checks["no_buffer_errors"] = not bool(re.search(r"NO_FREE_(?:BURST|PACKET)_BUFFERS", output))
    for key in indicators:
        values = [int(v) for v in re.findall(r"\b" + key + r"=(\d+)", output)]
        checks[key + "_zero"] = bool(values) and all(v == 0 for v in values)
    result["warnings"] = [line for line in output.splitlines() if re.search(r"WARN|CRITICAL|ERROR", line)]
    result["counts_and_shutdown"] = [line for line in output.splitlines() if any(k in line for k in ("complete:", "worker stopped", "inflight=", "images=", "partial_", "latency", "ibverbs RX port", "ibverbs TX port"))]
    if e["stage"] == "raw":
        counts = {direction + "/" + interface: int(value) for direction, interface, value in
                  re.findall(r"(TX|RX) complete: interface=(\w+) queue=0 packets=(\d+)", output)}
        checks["application_counts_present"] = len(counts) == 2 * len(paths) and all(v > 0 for v in counts.values())
        tx = sum(v for k, v in counts.items() if k.startswith("TX/"))
        rx = sum(v for k, v in counts.items() if k.startswith("RX/"))
        result["application_counts"] = counts
        result["application_rx_shortfall"] = tx - rx
        result["shutdown_note"] = "Timed application RX shortfall is distinct from final worker/PHY delivery; inspect outstanding TX and worker lines."
    gpu_monitors = [m for m in e["monitors"] if "nvidia-smi" in m["command"]]
    gpu_text = "\n".join(Path(m["path"]).read_text() for m in gpu_monitors if Path(m["path"]).exists())
    result["sampled_gpu_sm_MHz"] = sorted({int(v) for v in re.findall(r",\s*(\d+) MHz,", gpu_text)})
    if e["stage"] == "inference":
        checks["gpu_samples_present"] = bool(result["sampled_gpu_sm_MHz"])
        checks["artifacts_unchanged"] = e.get("artifacts_unchanged") is True
        checks["cache_loaded"] = "Loaded cached engine from" in output
        checks["frame_delivery"] = "sent 32768/32768 frames" in output and bool(re.search(r"ibverbs RX port .*packets=32768\b", output))
        checks["images_batches"] = "images=256 batches=8" in output and "delivered_bursts=8 pushed_images=256" in output
        checks["tx_drained"] = "posted=32768 completed=32768 inflight=0" in output
        for key in ("send_failures", "fill_failures", "dropped_bursts", "queue_full", "short_batch", "partial_bursts", "partial_packets", "early_flush_shortfall", "remainder_dropped"):
            values = [int(v) for v in re.findall(r"\b" + key + r"=(\d+)", output)]
            checks[key + "_zero"] = bool(values) and not any(values)
    result["limits"] = ["Startup/shutdown excluded by discarding two intervals at each end of contiguous >1-Gbps traffic.",
                         "Application shutdown counts and outstanding TX are reported separately from PHY delivery.",
                         "Finite inference bins are not sustained inference throughput; independent stage timings remain unmeasured.",
                         "Feature embeddings are computed live; dataset labels are ground truth, not predictions."]
    result["passed"] = all(checks.values())
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("evidence_directory")
    args = p.parse_args()
    result = summarize(args.evidence_directory)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
