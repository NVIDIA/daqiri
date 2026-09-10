# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Physical cross-host UDP parsing, counter derivation, and verdict policy."""

from __future__ import annotations

import math
import re
import statistics
from typing import Any

COMPLETION = re.compile(r"^(Server|Client) complete:\s+(.*)$")
FIELD = re.compile(r"([a-zA-Z_][a-zA-Z0-9_]*)=([0-9]+(?:\.[0-9]+)?)")
MLNX_RATE = re.compile(
    r"^\s*([a-zA-Z0-9_]+):\s+[0-9,.]+(?:\.[0-9]+)?\s+Bps\s+=\s+([0-9,.]+(?:\.[0-9]+)?)\s+Mbps"
)
DAQIRI_COUNTER = re.compile(r"\b([a-zA-Z_][a-zA-Z0-9_]*)=([0-9]+)\b")
NIC_RX_DISCARD = re.compile(
    r"^(?:rx_out_of_buffer|rx_prio[0-9]+_buf_discard(?:_packets)?|rx_discards_phy|rx_missed_errors)$"
)


class ResultParseError(ValueError):
    """Raised when a benchmark log lacks one authoritative completion summary."""


def parse_socket_completion(text: str, side: str) -> dict[str, int | float]:
    expected = "Client" if side == "tx" else "Server"
    matches: list[dict[str, int | float]] = []
    for line in text.splitlines():
        match = COMPLETION.match(line.strip())
        if not match or match.group(1) != expected:
            continue
        values: dict[str, int | float] = {}
        for field, raw in FIELD.findall(match.group(2)):
            values[field] = float(raw) if "." in raw else int(raw)
        matches.append(values)
    if len(matches) != 1:
        raise ResultParseError(
            f"expected one {expected} completion summary, found {len(matches)}"
        )
    required = (
        ("sent_packets", "sent_bytes", "seconds")
        if side == "tx"
        else ("recv_packets", "recv_bytes", "seconds")
    )
    missing = [field for field in required if field not in matches[0]]
    if missing:
        raise ResultParseError(f"{expected} summary is missing: {', '.join(missing)}")
    if float(matches[0]["seconds"]) <= 0:
        raise ResultParseError(f"{expected} summary has non-positive seconds")
    return matches[0]


def parse_daqiri_queue_counters(text: str) -> dict[str, int]:
    counters: dict[str, int] = {}
    for line in text.splitlines():
        lowered = line.lower()
        if (
            "queue" not in lowered
            and "stats" not in lowered
            and "packet" not in lowered
        ):
            continue
        for name, raw in DAQIRI_COUNTER.findall(line):
            counters[name] = counters.get(name, 0) + int(raw)
    return counters


def parse_mlnx_perf(text: str, counter: str) -> dict[str, Any]:
    raw_samples: list[float] = []
    for line in text.splitlines():
        match = MLNX_RATE.match(line)
        if match and match.group(1) == counter:
            raw_samples.append(float(match.group(2).replace(",", "")) / 1000.0)
    stable = raw_samples[1:-1] if len(raw_samples) >= 3 else []
    return {
        "counter": counter,
        "raw_gbps": raw_samples,
        "stable_gbps": stable,
        "stable_mean_gbps": statistics.fmean(stable) if stable else None,
        "stable_median_gbps": statistics.median(stable) if stable else None,
    }


def _counter_delta(
    before: dict[str, Any], after: dict[str, Any], section: str, field: str
) -> int | None:
    try:
        return int(after["snmp"][section][field]) - int(before["snmp"][section][field])
    except KeyError:
        return None


def _netdev_delta(
    before: dict[str, Any], after: dict[str, Any], netdev: str, field: str
) -> int | None:
    try:
        return int(after["netdevs"][netdev][field]) - int(
            before["netdevs"][netdev][field]
        )
    except KeyError:
        return None


def kernel_udp_delta(
    before: dict[str, Any], after: dict[str, Any]
) -> dict[str, int | None]:
    return {
        "InErrors": _counter_delta(before, after, "Udp", "InErrors"),
        "RcvbufErrors": _counter_delta(before, after, "Udp", "RcvbufErrors"),
        "ReasmFails": _counter_delta(before, after, "Ip", "ReasmFails"),
    }


def nic_rx_discard_delta(
    before: dict[str, Any], after: dict[str, Any], netdev: str
) -> dict[str, int]:
    before_stats = before["netdevs"].get(netdev, {})
    after_stats = after["netdevs"].get(netdev, {})
    return {
        name: int(after_stats[name]) - int(before_stats[name])
        for name in sorted(set(before_stats) & set(after_stats))
        if NIC_RX_DISCARD.match(name)
    }


def cpu_busy_delta(
    before: dict[str, Any], after: dict[str, Any], cpus: list[int]
) -> dict[str, float | None]:
    values: dict[str, float | None] = {}
    for cpu in cpus:
        key = f"cpu{cpu}"
        if key not in before["cpu"] or key not in after["cpu"]:
            values[key] = None
            continue
        total = after["cpu"][key]["total"] - before["cpu"][key]["total"]
        busy = after["cpu"][key]["busy"] - before["cpu"][key]["busy"]
        values[key] = (100.0 * busy / total) if total > 0 else None
    return values


def verdict_from_evidence(
    evidence: dict[str, Any], policy: dict[str, Any]
) -> tuple[str, list[str]]:
    reasons: list[str] = []
    application = evidence.get("application", {})
    tx_packets = application.get("tx_packets")
    rx_packets = application.get("rx_packets")
    if not isinstance(tx_packets, int) or tx_packets <= 0:
        reasons.append("missing or zero application TX packets")
    if not isinstance(rx_packets, int) or rx_packets <= 0:
        reasons.append("missing or zero application RX packets")
    if isinstance(tx_packets, int) and isinstance(rx_packets, int):
        loss = tx_packets - rx_packets
        if loss < 0:
            reasons.append("application RX packets exceed TX packets")
        elif loss > policy["max_delivery_loss_packets"]:
            reasons.append(
                f"application delivery loss {loss} exceeds {policy['max_delivery_loss_packets']}"
            )
    tx_bytes = application.get("tx_bytes")
    rx_bytes = application.get("rx_bytes")
    if not isinstance(tx_bytes, int) or not isinstance(rx_bytes, int):
        reasons.append("application byte totals are missing")
    elif tx_bytes != rx_bytes:
        reasons.append(f"application byte disagreement: TX {tx_bytes}, RX {rx_bytes}")

    queue_counters = evidence.get("daqiri_queue_counters")
    if not isinstance(queue_counters, dict) or not queue_counters:
        reasons.append("required DAQIRI queue counters are missing")
    else:
        missing_roles = sorted(
            role_id for role_id, counters in queue_counters.items() if not counters
        )
        if missing_roles:
            reasons.append(
                "required DAQIRI queue counters are missing for "
                + ", ".join(missing_roles)
            )
        daqiri_tx = sum(
            counters.get("tx_pkts", 0)
            for role_id, counters in queue_counters.items()
            if role_id.startswith("tx-")
        )
        daqiri_rx = sum(
            counters.get("rx_pkts", 0)
            for role_id, counters in queue_counters.items()
            if role_id.startswith("rx-")
        )
        if isinstance(tx_packets, int) and daqiri_tx != tx_packets:
            reasons.append(
                f"DAQIRI TX queue total {daqiri_tx} disagrees with application {tx_packets}"
            )
        if isinstance(rx_packets, int) and daqiri_rx != rx_packets:
            reasons.append(
                f"DAQIRI RX queue total {daqiri_rx} disagrees with application {rx_packets}"
            )

    kernel = evidence.get("kernel_udp", {})
    if not kernel or any(value is None for value in kernel.values()):
        reasons.append("required kernel UDP counters are missing")
    elif any(int(value) < 0 for value in kernel.values()):
        reasons.append("kernel UDP counters decreased during the active window")
    elif sum(int(value) for value in kernel.values()) > policy["max_kernel_udp_errors"]:
        reasons.append("kernel UDP/reassembly error counters increased")

    nic = evidence.get("nic_rx_discards")
    if not isinstance(nic, dict) or not nic:
        reasons.append("required NIC receive-discard counters are missing")
    elif any(int(value) < 0 for value in nic.values()):
        reasons.append("NIC receive counters decreased during the active window")
    elif sum(int(value) for value in nic.values()) > policy["max_nic_rx_discards"]:
        reasons.append("NIC receive discard counters increased")

    rdma_errors = evidence.get("rdma_errors")
    if rdma_errors is not None and rdma_errors > 0:
        reasons.append("RDMA completion or connection errors were reported")

    phy_links = evidence.get("phy", {}).get("links", {})
    if not phy_links:
        reasons.append("physical-path evidence is missing")
    for link_id, link in sorted(phy_links.items()):
        tx = link.get("tx_packets")
        rx = link.get("rx_packets")
        if not isinstance(tx, int) or not isinstance(rx, int) or tx <= 0 or rx <= 0:
            reasons.append(f"{link_id} directional PHY packet counters did not advance")
            continue
        difference = abs(tx - rx) * 100.0 / max(tx, rx)
        if difference > policy["phy_packet_tolerance_percent"]:
            reasons.append(
                f"{link_id} PHY packet disagreement {difference:.6f}% exceeds tolerance"
            )
        tx_bytes_phy = link.get("tx_bytes")
        rx_bytes_phy = link.get("rx_bytes")
        if (
            not isinstance(tx_bytes_phy, int)
            or not isinstance(rx_bytes_phy, int)
            or tx_bytes_phy <= 0
            or rx_bytes_phy <= 0
        ):
            reasons.append(f"{link_id} directional PHY byte counters did not advance")
        else:
            byte_difference = (
                abs(tx_bytes_phy - rx_bytes_phy)
                * 100.0
                / max(tx_bytes_phy, rx_bytes_phy)
            )
            if byte_difference > policy["phy_packet_tolerance_percent"]:
                reasons.append(
                    f"{link_id} PHY byte disagreement {byte_difference:.6f}% exceeds tolerance"
                )

    samples = evidence.get("mlnx_perf", {})
    if not samples:
        reasons.append("mlnx_perf evidence is missing")
    else:
        for link_id, link in sorted(samples.items()):
            if (
                len(link.get("rx", {}).get("stable_gbps", []))
                < policy["minimum_mlnx_samples"]
            ):
                reasons.append(f"{link_id} has too few stable mlnx_perf RX samples")
            if (
                len(link.get("tx", {}).get("stable_gbps", []))
                < policy["minimum_mlnx_samples"]
            ):
                reasons.append(f"{link_id} has too few stable mlnx_perf TX samples")

    requested = evidence.get("requested_rate", {}).get("aggregate_gbps")
    achieved = evidence.get("achieved_rate", {}).get("mlnx_rx_aggregate_gbps")
    if not isinstance(requested, (int, float)) or not isinstance(
        achieved, (int, float)
    ):
        reasons.append("requested or achieved rate is missing")
    elif (
        requested > 0 and achieved * 100.0 / requested < policy["minimum_rate_percent"]
    ):
        reasons.append(
            f"achieved rate {achieved:.3f} Gbps is below {policy['minimum_rate_percent']}% of requested"
        )

    requested_duration = evidence.get("requested_duration_seconds")
    active_duration = evidence.get("active_duration_seconds")
    if not isinstance(requested_duration, (int, float)) or not isinstance(
        active_duration, (int, float)
    ):
        reasons.append("requested or active duration is missing")
    elif requested_duration <= 0 or active_duration <= 0:
        reasons.append("requested or active duration is not positive")
    elif (
        active_duration * 100.0 / requested_duration
        < policy["minimum_duration_percent"]
    ):
        reasons.append(
            f"active duration {active_duration:.3f}s is below "
            f"{policy['minimum_duration_percent']}% of requested {requested_duration}s"
        )

    return ("valid" if not reasons else "invalid", reasons)


def build_udp_evidence(
    *,
    cell: dict[str, Any],
    site: dict[str, Any],
    roles: list[dict[str, Any]],
    role_logs: dict[str, dict[str, str]],
    snapshots: dict[str, dict[str, dict[str, Any]]],
    collector_logs: dict[str, str],
    active_duration_seconds: float,
    requested_duration_seconds: int,
) -> dict[str, Any]:
    selected_pairs = site["pairs"][: cell["pair_count"]]
    completions: dict[str, dict[str, int | float]] = {}
    daqiri: dict[str, dict[str, int]] = {}
    for role in roles:
        completions[role["role_id"]] = parse_socket_completion(
            role_logs[role["role_id"]]["stdout"], role["direction"]
        )
        daqiri[role["role_id"]] = parse_daqiri_queue_counters(
            role_logs[role["role_id"]]["stderr"]
        )

    tx_packets = sum(
        int(value["sent_packets"])
        for key, value in completions.items()
        if key.startswith("tx-")
    )
    tx_bytes = sum(
        int(value["sent_bytes"])
        for key, value in completions.items()
        if key.startswith("tx-")
    )
    rx_packets = sum(
        int(value["recv_packets"])
        for key, value in completions.items()
        if key.startswith("rx-")
    )
    rx_bytes = sum(
        int(value["recv_bytes"])
        for key, value in completions.items()
        if key.startswith("rx-")
    )
    app_seconds = max(
        float(value["seconds"])
        for key, value in completions.items()
        if key.startswith("tx-")
    )

    kernel: dict[str, int | None] = {"InErrors": 0, "RcvbufErrors": 0, "ReasmFails": 0}
    nic: dict[str, int] = {}
    cpu: dict[str, dict[str, float | None]] = {}
    phy_links: dict[str, dict[str, int | None]] = {}
    perf_links: dict[str, dict[str, Any]] = {}
    phy_rx_bytes_total = 0
    for host_id in sorted(snapshots["active_start"]):
        before = snapshots["active_start"][host_id]
        after = snapshots["active_end"][host_id]
        host_kernel = kernel_udp_delta(before, after)
        for name, value in host_kernel.items():
            if value is None:
                kernel[name] = None
            elif kernel[name] is not None:
                kernel[name] = int(kernel[name]) + value
        host_cpus = sorted(
            {
                placement[field]
                for pair in selected_pairs
                for direction, placement in (("tx", pair["tx"]), ("rx", pair["rx"]))
                if site["links"][pair["link"]][direction]["host"] == host_id
                for field in ("master_cpu", "io_cpu", "worker_cpu")
            }
        )
        cpu[host_id] = cpu_busy_delta(before, after, host_cpus)

    for pair in selected_pairs:
        link_id = pair["link"]
        if link_id in phy_links:
            continue
        link = site["links"][link_id]
        tx_host = link["tx"]["host"]
        rx_host = link["rx"]["host"]
        tx_netdev = link["tx"]["netdev"]
        rx_netdev = link["rx"]["netdev"]
        tx_before = snapshots["active_start"][tx_host]
        tx_after = snapshots["active_end"][tx_host]
        rx_before = snapshots["active_start"][rx_host]
        rx_after = snapshots["active_end"][rx_host]
        tx_packets_phy = _netdev_delta(tx_before, tx_after, tx_netdev, "tx_packets_phy")
        rx_packets_phy = _netdev_delta(rx_before, rx_after, rx_netdev, "rx_packets_phy")
        tx_bytes_phy = _netdev_delta(tx_before, tx_after, tx_netdev, "tx_bytes_phy")
        rx_bytes_phy = _netdev_delta(rx_before, rx_after, rx_netdev, "rx_bytes_phy")
        phy_links[link_id] = {
            "tx_packets": tx_packets_phy,
            "rx_packets": rx_packets_phy,
            "tx_bytes": tx_bytes_phy,
            "rx_bytes": rx_bytes_phy,
        }
        if isinstance(rx_bytes_phy, int):
            phy_rx_bytes_total += rx_bytes_phy
        for name, value in nic_rx_discard_delta(rx_before, rx_after, rx_netdev).items():
            nic[f"{link_id}.{name}"] = value
        perf_links[link_id] = {
            "tx": parse_mlnx_perf(collector_logs[f"mlnx-tx-{link_id}"], "tx_bytes_phy"),
            "rx": parse_mlnx_perf(collector_logs[f"mlnx-rx-{link_id}"], "rx_bytes_phy"),
        }

    requested = float(cell["requested_rate_gbps"])
    if cell["rate_scope"] == "per_pair":
        requested_aggregate = requested * len(selected_pairs)
    elif cell["rate_scope"] == "per_link":
        requested_aggregate = requested * len({pair["link"] for pair in selected_pairs})
    else:
        requested_aggregate = requested
    mlnx_rx_rates = [
        link["rx"]["stable_mean_gbps"]
        for link in perf_links.values()
        if link["rx"]["stable_mean_gbps"] is not None
    ]
    mlnx_tx_rates = [
        link["tx"]["stable_mean_gbps"]
        for link in perf_links.values()
        if link["tx"]["stable_mean_gbps"] is not None
    ]
    return {
        "application": {
            "tx_packets": tx_packets,
            "rx_packets": rx_packets,
            "tx_bytes": tx_bytes,
            "rx_bytes": rx_bytes,
            "delivery_loss_packets": tx_packets - rx_packets,
            "delivery_loss_bytes": tx_bytes - rx_bytes,
            "reported_seconds": app_seconds,
            "tx_payload_gbps": (tx_bytes * 8.0 / app_seconds / 1e9),
            "rx_payload_gbps": (rx_bytes * 8.0 / app_seconds / 1e9),
            "completions": completions,
        },
        "active_duration_seconds": active_duration_seconds,
        "requested_duration_seconds": requested_duration_seconds,
        "requested_rate": {
            "value_gbps": requested,
            "scope": cell["rate_scope"],
            "aggregate_gbps": requested_aggregate,
        },
        "achieved_rate": {
            "mlnx_rx_aggregate_gbps": math.fsum(mlnx_rx_rates)
            if mlnx_rx_rates
            else None,
            "mlnx_tx_aggregate_gbps": math.fsum(mlnx_tx_rates)
            if mlnx_tx_rates
            else None,
            "phy_rx_window_gbps": (
                phy_rx_bytes_total * 8.0 / active_duration_seconds / 1e9
                if active_duration_seconds > 0
                else None
            ),
        },
        "kernel_udp": kernel,
        "nic_rx_discards": nic,
        "phy": {"links": phy_links},
        "mlnx_perf": perf_links,
        "cpu_busy_percent": cpu,
        "daqiri_queue_counters": daqiri,
    }
