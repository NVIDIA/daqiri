# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Manifest loading, semantic validation, and deterministic UDP plan expansion."""

from __future__ import annotations

import copy
import hashlib
import ipaddress
import itertools
import json
import posixpath
import re
from pathlib import Path
from typing import Any

import yaml

SUITE_VERSION = "daqiri.benchmark-suite/v1"
SITE_VERSION = "daqiri.benchmark-site/v1"
RESOLVED_VERSION = "daqiri.benchmark-resolved/v1"
SUPPORTED_COLLECTORS = {
    "application",
    "cpu",
    "daqiri_queue",
    "kernel_udp",
    "mlnx_perf",
    "nic",
    "phy",
    "provenance",
}
REQUIRED_UDP_COLLECTORS = SUPPORTED_COLLECTORS
MATRIX_FIELDS = (
    "message_size_bytes",
    "pair_count",
    "requested_rate_gbps",
    "batch_size",
    "buffer_size_bytes",
    "buffers_per_region",
)
RATE_SCOPES = {"per_pair", "per_link", "aggregate"}
SAFE_ID = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_.-]*$")


class ManifestError(ValueError):
    """Raised when a suite or site profile is ambiguous or unsafe."""


def _load_yaml(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as stream:
            data = yaml.safe_load(stream)
    except (OSError, yaml.YAMLError) as exc:
        raise ManifestError(f"cannot load {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise ManifestError(f"{path} must contain a YAML mapping")
    return data


def load_inputs(
    suite_path: Path, site_path: Path
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Load and validate a portable suite and site-local binding profile."""

    suite = _load_yaml(suite_path)
    site = _load_yaml(site_path)
    validate_suite(suite)
    validate_site(site)
    validate_combination(suite, site)
    return suite, site


def _exact_keys(value: dict[str, Any], allowed: set[str], context: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ManifestError(f"{context} has unknown field(s): {', '.join(unknown)}")


def _mapping(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ManifestError(f"{context} must be a mapping")
    return value


def _list(value: Any, context: str) -> list[Any]:
    if not isinstance(value, list) or not value:
        raise ManifestError(f"{context} must be a non-empty list")
    return value


def _positive_int(value: Any, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ManifestError(f"{context} must be a positive integer")
    return value


def _nonnegative_number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
        raise ManifestError(f"{context} must be a non-negative number")
    return float(value)


def _safe_id(value: Any, context: str) -> str:
    if not isinstance(value, str) or not SAFE_ID.fullmatch(value):
        raise ManifestError(f"{context} must use only letters, digits, '.', '_' or '-'")
    return value


def validate_suite(suite: dict[str, Any]) -> None:
    _exact_keys(
        suite,
        {
            "schema_version",
            "name",
            "description",
            "topology",
            "evidence_class",
            "adapter",
            "workload",
            "duration_seconds",
            "repetitions",
            "drain_seconds",
            "readiness_timeout_seconds",
            "cleanup_timeout_seconds",
            "execution_order",
            "rate_scope",
            "allow_cpu_overlap",
            "matrix",
            "collectors",
            "acceptance",
            "requirements",
        },
        "suite",
    )
    if suite.get("schema_version") != SUITE_VERSION:
        raise ManifestError(f"suite.schema_version must be {SUITE_VERSION!r}")
    _safe_id(suite.get("name"), "suite.name")
    if suite.get("topology") != "physical_cross_host":
        raise ManifestError("v1 supports only topology: physical_cross_host")
    if suite.get("evidence_class") != "physical_cross_host":
        raise ManifestError("evidence_class must explicitly be physical_cross_host")
    if suite.get("adapter") != "udp_socket":
        raise ManifestError("v1 supports only adapter: udp_socket")
    if suite.get("workload") != "none":
        raise ManifestError("v1 supports only workload: none")
    if "description" in suite and not isinstance(suite["description"], str):
        raise ManifestError("suite.description must be a string")
    for field in (
        "duration_seconds",
        "repetitions",
        "drain_seconds",
        "readiness_timeout_seconds",
        "cleanup_timeout_seconds",
    ):
        _positive_int(suite.get(field), f"suite.{field}")
    if suite.get("execution_order") != "sequential":
        raise ManifestError("v1 requires execution_order: sequential")
    if suite.get("rate_scope") not in RATE_SCOPES:
        raise ManifestError("suite.rate_scope must be per_pair, per_link, or aggregate")
    if not isinstance(suite.get("allow_cpu_overlap"), bool):
        raise ManifestError("suite.allow_cpu_overlap must be true or false")

    matrix = _mapping(suite.get("matrix"), "suite.matrix")
    _exact_keys(matrix, set(MATRIX_FIELDS), "suite.matrix")
    for field in MATRIX_FIELDS:
        values = _list(matrix.get(field), f"suite.matrix.{field}")
        if len(set(values)) != len(values):
            raise ManifestError(f"suite.matrix.{field} contains duplicate values")
        for index, value in enumerate(values):
            context = f"suite.matrix.{field}[{index}]"
            if field == "requested_rate_gbps":
                if _nonnegative_number(value, context) <= 0:
                    raise ManifestError(f"{context} must be greater than zero")
            else:
                _positive_int(value, context)

    collectors = _list(suite.get("collectors"), "suite.collectors")
    if any(not isinstance(item, str) for item in collectors):
        raise ManifestError("suite.collectors entries must be strings")
    if len(set(collectors)) != len(collectors):
        raise ManifestError("suite.collectors must not contain duplicates")
    unknown_collectors = sorted(set(collectors) - SUPPORTED_COLLECTORS)
    if unknown_collectors:
        raise ManifestError(
            f"unsupported collector(s): {', '.join(unknown_collectors)}"
        )
    missing_collectors = sorted(REQUIRED_UDP_COLLECTORS - set(collectors))
    if missing_collectors:
        raise ManifestError(
            f"physical UDP requires collector(s): {', '.join(missing_collectors)}"
        )

    acceptance = _mapping(suite.get("acceptance"), "suite.acceptance")
    _exact_keys(
        acceptance,
        {
            "max_delivery_loss_packets",
            "max_kernel_udp_errors",
            "max_nic_rx_discards",
            "phy_packet_tolerance_percent",
            "minimum_rate_percent",
            "minimum_mlnx_samples",
            "minimum_duration_percent",
        },
        "suite.acceptance",
    )
    for field in (
        "max_delivery_loss_packets",
        "max_kernel_udp_errors",
        "max_nic_rx_discards",
    ):
        value = acceptance.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ManifestError(
                f"suite.acceptance.{field} must be a non-negative integer"
            )
    _nonnegative_number(
        acceptance.get("phy_packet_tolerance_percent"),
        "suite.acceptance.phy_packet_tolerance_percent",
    )
    minimum_rate = _nonnegative_number(
        acceptance.get("minimum_rate_percent"), "suite.acceptance.minimum_rate_percent"
    )
    if minimum_rate > 100:
        raise ManifestError("suite.acceptance.minimum_rate_percent cannot exceed 100")
    _positive_int(
        acceptance.get("minimum_mlnx_samples"), "suite.acceptance.minimum_mlnx_samples"
    )
    minimum_duration = _nonnegative_number(
        acceptance.get("minimum_duration_percent"),
        "suite.acceptance.minimum_duration_percent",
    )
    if minimum_duration > 100:
        raise ManifestError(
            "suite.acceptance.minimum_duration_percent cannot exceed 100"
        )

    requirements = _mapping(suite.get("requirements"), "suite.requirements")
    _exact_keys(requirements, {"gpu", "hugepages"}, "suite.requirements")
    if any(
        not isinstance(requirements.get(field), bool) for field in ("gpu", "hugepages")
    ):
        raise ManifestError("suite requirements gpu/hugepages must be booleans")


def validate_site(site: dict[str, Any]) -> None:
    _exact_keys(
        site,
        {
            "schema_version",
            "name",
            "topology_ownership",
            "hosts",
            "links",
            "pairs",
            "contamination_checks",
        },
        "site",
    )
    if site.get("schema_version") != SITE_VERSION:
        raise ManifestError(f"site.schema_version must be {SITE_VERSION!r}")
    _safe_id(site.get("name"), "site.name")
    if site.get("topology_ownership") != "external":
        raise ManifestError(
            "v1 accepts only topology_ownership: external and never mutates networking"
        )

    hosts = _mapping(site.get("hosts"), "site.hosts")
    if len(hosts) < 2:
        raise ManifestError("site.hosts must define at least two physical hosts")
    for host_id, raw_host in hosts.items():
        _safe_id(host_id, "site host id")
        host = _mapping(raw_host, f"site.hosts.{host_id}")
        _exact_keys(
            host,
            {
                "transport",
                "destination",
                "worktree",
                "runtime_root",
                "python",
                "worker_prefix",
                "benchmark_binary",
                "expected_binary_sha256",
                "expected_commit",
                "require_clean",
                "container",
            },
            f"site.hosts.{host_id}",
        )
        if host.get("transport") not in {"local", "ssh"}:
            raise ManifestError(f"site.hosts.{host_id}.transport must be local or ssh")
        if host["transport"] == "ssh":
            destination = host.get("destination")
            if (
                not isinstance(destination, str)
                or not destination
                or destination.startswith("-")
            ):
                raise ManifestError(
                    f"site.hosts.{host_id}.destination is required for ssh and cannot start with '-'"
                )
        elif "destination" in host:
            raise ManifestError(
                f"site.hosts.{host_id}.destination must be omitted for local transport"
            )
        for field in (
            "worktree",
            "runtime_root",
            "python",
            "benchmark_binary",
            "expected_binary_sha256",
            "expected_commit",
        ):
            if not isinstance(host.get(field), str) or not host[field]:
                raise ManifestError(
                    f"site.hosts.{host_id}.{field} must be a non-empty string"
                )
        if not re.fullmatch(r"[0-9a-fA-F]{40}", host["expected_commit"]):
            raise ManifestError(
                f"site.hosts.{host_id}.expected_commit must be a full 40-digit Git SHA"
            )
        if not re.fullmatch(r"[0-9a-fA-F]{64}", host["expected_binary_sha256"]):
            raise ManifestError(
                f"site.hosts.{host_id}.expected_binary_sha256 must be a SHA-256 digest"
            )
        for field in ("worktree", "runtime_root"):
            if not posixpath.isabs(host[field]) or host[field] == "/":
                raise ManifestError(
                    f"site.hosts.{host_id}.{field} must be an absolute, non-root path"
                )
        prefix = host.get("worker_prefix")
        if prefix not in ([], ["sudo", "-n"]):
            raise ManifestError(
                f"site.hosts.{host_id}.worker_prefix must be [] or ['sudo', '-n'] in v1"
            )
        if not isinstance(host.get("require_clean"), bool):
            raise ManifestError(f"site.hosts.{host_id}.require_clean must be boolean")
        container = _mapping(host.get("container"), f"site.hosts.{host_id}.container")
        _exact_keys(
            container,
            {"runtime", "image", "digest", "workdir", "gpus"},
            f"site.hosts.{host_id}.container",
        )
        if container.get("runtime") not in {"none", "docker"}:
            raise ManifestError(
                f"site.hosts.{host_id}.container.runtime must be none or docker"
            )
        for field in ("image", "digest", "workdir"):
            if not isinstance(container.get(field), str):
                raise ManifestError(
                    f"site.hosts.{host_id}.container.{field} must be a string"
                )
        if not isinstance(container.get("gpus"), bool):
            raise ManifestError(f"site.hosts.{host_id}.container.gpus must be boolean")
        if container["runtime"] == "none":
            if any(container[field] for field in ("image", "digest", "workdir")):
                raise ManifestError(
                    f"site.hosts.{host_id}.container metadata must be empty for runtime none"
                )
            if container["gpus"]:
                raise ManifestError(
                    f"site.hosts.{host_id}.container.gpus must be false for runtime none"
                )
        else:
            if not container["image"]:
                raise ManifestError(
                    f"site.hosts.{host_id}.container.image is required for docker"
                )
            if not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", container["digest"]):
                raise ManifestError(
                    f"site.hosts.{host_id}.container.digest must be a Docker image ID"
                )
            if not posixpath.isabs(container["workdir"]):
                raise ManifestError(
                    f"site.hosts.{host_id}.container.workdir must be absolute"
                )
            if not posixpath.isabs(host["benchmark_binary"]):
                raise ManifestError(
                    f"site.hosts.{host_id}.benchmark_binary must be absolute for docker"
                )

    links = _mapping(site.get("links"), "site.links")
    if not links:
        raise ManifestError("site.links must not be empty")
    for link_id, raw_link in links.items():
        _safe_id(link_id, "site link id")
        link = _mapping(raw_link, f"site.links.{link_id}")
        _exact_keys(link, {"tx", "rx"}, f"site.links.{link_id}")
        for direction in ("tx", "rx"):
            endpoint = _mapping(
                link.get(direction), f"site.links.{link_id}.{direction}"
            )
            _exact_keys(
                endpoint,
                {
                    "host",
                    "netdev",
                    "pci",
                    "ip",
                    "mac",
                    "mtu",
                    "speed_mbps",
                    "irq_cpus",
                    "napi_cpus",
                    "queue_capacity",
                },
                f"site.links.{link_id}.{direction}",
            )
            if endpoint.get("host") not in hosts:
                raise ManifestError(f"site.links.{link_id}.{direction}.host is unknown")
            for field in ("netdev", "pci", "ip", "mac"):
                if not isinstance(endpoint.get(field), str) or not endpoint[field]:
                    raise ManifestError(
                        f"site.links.{link_id}.{direction}.{field} must be a non-empty string"
                    )
            try:
                ipaddress.ip_address(endpoint["ip"])
            except ValueError as exc:
                raise ManifestError(
                    f"site.links.{link_id}.{direction}.ip is invalid"
                ) from exc
            _positive_int(
                endpoint.get("queue_capacity"),
                f"site.links.{link_id}.{direction}.queue_capacity",
            )
            _positive_int(endpoint.get("mtu"), f"site.links.{link_id}.{direction}.mtu")
            _positive_int(
                endpoint.get("speed_mbps"),
                f"site.links.{link_id}.{direction}.speed_mbps",
            )
            for field in ("irq_cpus", "napi_cpus"):
                cpus = _list(
                    endpoint.get(field), f"site.links.{link_id}.{direction}.{field}"
                )
                for index, cpu in enumerate(cpus):
                    if isinstance(cpu, bool) or not isinstance(cpu, int) or cpu < 0:
                        raise ManifestError(
                            f"site.links.{link_id}.{direction}.{field}[{index}] must be a CPU index"
                        )
        if link["tx"]["host"] == link["rx"]["host"]:
            raise ManifestError(
                f"physical cross-host link {link_id} must join distinct hosts"
            )

    pairs = _list(site.get("pairs"), "site.pairs")
    pair_ids: set[str] = set()
    for index, raw_pair in enumerate(pairs):
        pair = _mapping(raw_pair, f"site.pairs[{index}]")
        _exact_keys(pair, {"id", "link", "tx", "rx"}, f"site.pairs[{index}]")
        pair_id = _safe_id(pair.get("id"), f"site.pairs[{index}].id")
        if pair_id in pair_ids:
            raise ManifestError(f"duplicate pair id: {pair_id}")
        pair_ids.add(pair_id)
        if pair.get("link") not in links:
            raise ManifestError(f"site.pairs[{index}].link is unknown")
        for direction in ("tx", "rx"):
            placement = _mapping(
                pair.get(direction), f"site.pairs[{index}].{direction}"
            )
            _exact_keys(
                placement,
                {
                    "local_port",
                    "remote_port",
                    "queue_id",
                    "io_cpu",
                    "worker_cpu",
                    "master_cpu",
                    "numa_node",
                    "cpu_cluster",
                },
                f"site.pairs[{index}].{direction}",
            )
            for field in ("local_port", "remote_port"):
                port = _positive_int(
                    placement.get(field), f"site.pairs[{index}].{direction}.{field}"
                )
                if port > 65535:
                    raise ManifestError(
                        f"site.pairs[{index}].{direction}.{field} exceeds 65535"
                    )
            for field in (
                "queue_id",
                "io_cpu",
                "worker_cpu",
                "master_cpu",
                "numa_node",
            ):
                value = placement.get(field)
                if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                    raise ManifestError(
                        f"site.pairs[{index}].{direction}.{field} must be a non-negative integer"
                    )
            _safe_id(
                placement.get("cpu_cluster"),
                f"site.pairs[{index}].{direction}.cpu_cluster",
            )
        if pair["tx"]["local_port"] != pair["rx"]["remote_port"]:
            raise ManifestError(
                f"pair {pair_id} has mismatched TX local/RX remote ports"
            )
        if pair["tx"]["remote_port"] != pair["rx"]["local_port"]:
            raise ManifestError(
                f"pair {pair_id} has mismatched TX remote/RX local ports"
            )

    checks = site.get("contamination_checks")
    if not isinstance(checks, list):
        raise ManifestError("site.contamination_checks must be a list")
    for index, raw_check in enumerate(checks):
        check = _mapping(raw_check, f"site.contamination_checks[{index}]")
        _exact_keys(
            check,
            {"name", "host", "argv", "expect_stdout_empty"},
            f"site.contamination_checks[{index}]",
        )
        _safe_id(check.get("name"), f"site.contamination_checks[{index}].name")
        if check.get("host") not in hosts:
            raise ManifestError(f"site.contamination_checks[{index}].host is unknown")
        argv = _list(check.get("argv"), f"site.contamination_checks[{index}].argv")
        if any(not isinstance(arg, str) or not arg for arg in argv):
            raise ManifestError(
                f"site.contamination_checks[{index}].argv must contain strings"
            )
        if not isinstance(check.get("expect_stdout_empty"), bool):
            raise ManifestError(
                f"site.contamination_checks[{index}].expect_stdout_empty must be boolean"
            )


def validate_combination(suite: dict[str, Any], site: dict[str, Any]) -> None:
    max_pairs = max(suite["matrix"]["pair_count"])
    if max_pairs > len(site["pairs"]):
        raise ManifestError(
            f"suite requests {max_pairs} pair(s), but site defines only {len(site['pairs'])}"
        )
    if max(suite["matrix"]["message_size_bytes"]) > 65507:
        raise ManifestError("UDP message_size_bytes cannot exceed 65507")
    for message_size, buffer_size in itertools.product(
        suite["matrix"]["message_size_bytes"], suite["matrix"]["buffer_size_bytes"]
    ):
        if buffer_size < message_size:
            raise ManifestError(
                f"buffer_size_bytes {buffer_size} does not cover message_size_bytes {message_size}"
            )

    selected_pairs = site["pairs"][:max_pairs]
    counts: dict[tuple[str, str], int] = {}
    queue_ids: dict[tuple[str, str], set[int]] = {}
    local_ports: dict[str, set[int]] = {}
    for pair in selected_pairs:
        for direction in ("tx", "rx"):
            key = (pair["link"], direction)
            counts[key] = counts.get(key, 0) + 1
            queue_id = pair[direction]["queue_id"]
            used_queues = queue_ids.setdefault(key, set())
            if queue_id in used_queues:
                raise ManifestError(
                    f"link {pair['link']} {direction} queue {queue_id} is assigned twice"
                )
            used_queues.add(queue_id)
            host_id = site["links"][pair["link"]][direction]["host"]
            local_port = pair[direction]["local_port"]
            used_ports = local_ports.setdefault(host_id, set())
            if local_port in used_ports:
                raise ManifestError(
                    f"host {host_id} UDP local port {local_port} is assigned twice"
                )
            used_ports.add(local_port)
    for (link_id, direction), count in counts.items():
        capacity = site["links"][link_id][direction]["queue_capacity"]
        if count > capacity:
            raise ManifestError(
                f"link {link_id} {direction} needs {count} queues but capacity is {capacity}"
            )

    validate_cpu_placement(selected_pairs, site, suite["allow_cpu_overlap"])

    for pair_count, requested_rate in itertools.product(
        suite["matrix"]["pair_count"], suite["matrix"]["requested_rate_gbps"]
    ):
        pairs_for_cell = site["pairs"][:pair_count]
        requested_by_link: dict[str, float] = {}
        for pair in pairs_for_cell:
            pair_rate = _pair_rate(
                suite["rate_scope"], float(requested_rate), pairs_for_cell, pair
            )
            requested_by_link[pair["link"]] = (
                requested_by_link.get(pair["link"], 0.0) + pair_rate
            )
        for link_id, link_rate in requested_by_link.items():
            capacity_gbps = (
                min(
                    site["links"][link_id][direction]["speed_mbps"]
                    for direction in ("tx", "rx")
                )
                / 1000.0
            )
            if link_rate > capacity_gbps:
                raise ManifestError(
                    f"link {link_id} requests {link_rate:g} Gbps, above declared "
                    f"{capacity_gbps:g} Gbps capacity"
                )

    for message_size in suite["matrix"]["message_size_bytes"]:
        required_mtu = message_size + 28
        for pair in selected_pairs:
            for direction in ("tx", "rx"):
                endpoint = site["links"][pair["link"]][direction]
                if endpoint["mtu"] < required_mtu:
                    raise ManifestError(
                        f"link {pair['link']} {direction} MTU {endpoint['mtu']} does not cover "
                        f"UDP payload {message_size} (requires {required_mtu})"
                    )


def validate_cpu_placement(
    pairs: list[dict[str, Any]], site: dict[str, Any], allow_overlap: bool
) -> None:
    """Reject accidental benchmark/IRQ/NAPI overlap on each host."""

    if allow_overlap:
        return
    assignments: dict[str, dict[int, list[str]]] = {}
    for pair in pairs:
        link = site["links"][pair["link"]]
        for direction in ("tx", "rx"):
            host_id = link[direction]["host"]
            host_assignments = assignments.setdefault(host_id, {})
            placement = pair[direction]
            for field in ("master_cpu", "io_cpu", "worker_cpu"):
                cpu = placement[field]
                host_assignments.setdefault(cpu, []).append(
                    f"{pair['id']}.{direction}.{field}"
                )

    for link_id, link in site["links"].items():
        for direction in ("tx", "rx"):
            endpoint = link[direction]
            host_assignments = assignments.setdefault(endpoint["host"], {})
            for cpu in sorted(set(endpoint["irq_cpus"]) | set(endpoint["napi_cpus"])):
                host_assignments.setdefault(cpu, []).append(
                    f"{link_id}.{direction}.irq_napi"
                )

    conflicts: list[str] = []
    for host_id, cpus in sorted(assignments.items()):
        for cpu, labels in sorted(cpus.items()):
            if len(labels) > 1:
                conflicts.append(f"{host_id}:cpu{cpu}=" + "+".join(labels))
    if conflicts:
        raise ManifestError(
            "CPU overlap requires allow_cpu_overlap: true: " + "; ".join(conflicts)
        )


def validate_rdma_capacity(roles: list[dict[str, int]]) -> None:
    """Reusable semantic validation for a later RDMA adapter."""

    rx_required = sum(role.get("rx_depth", 0) for role in roles)
    tx_required = sum(role.get("tx_depth", 0) for role in roles)
    rx_pool = max((role.get("shared_rx_pool", 0) for role in roles), default=0)
    tx_pool = max((role.get("shared_tx_pool", 0) for role in roles), default=0)
    if rx_pool < rx_required:
        raise ManifestError(
            f"shared RDMA RX pool {rx_pool} is smaller than depth sum {rx_required}"
        )
    if tx_pool < tx_required:
        raise ManifestError(
            f"shared RDMA TX pool {tx_pool} is smaller than depth sum {tx_required}"
        )


def _canonical_json(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def digest(value: Any) -> str:
    return hashlib.sha256(_canonical_json(value)).hexdigest()


def _input_digest(value: dict[str, Any]) -> str:
    return digest(value)


def _absolute_binary(host: dict[str, Any]) -> str:
    binary = host["benchmark_binary"]
    if posixpath.isabs(binary):
        return binary
    return posixpath.join(host["worktree"], binary)


def _container_name(process_run_id: str) -> str:
    return "daqiri-bench-" + hashlib.sha256(process_run_id.encode()).hexdigest()[:24]


def _execution(
    host: dict[str, Any],
    benchmark_argv: list[str],
    process_run_id: str,
    suite_run_id: str,
) -> tuple[list[str], str | None]:
    container = host["container"]
    if container["runtime"] == "none":
        return benchmark_argv, None
    name = _container_name(process_run_id)
    command = [
        "docker",
        "run",
        "--rm",
        "--pull",
        "never",
        "--name",
        name,
        "--label",
        f"com.nvidia.daqiri.benchmark.process_id={process_run_id}",
        "--network",
        "host",
        "--privileged",
        "--ulimit",
        "memlock=-1:-1",
        "--volume",
        f"{host['runtime_root']}:{host['runtime_root']}:rw",
        "--workdir",
        container["workdir"],
        "--env",
        f"DAQIRI_BENCH_SUITE_RUN_ID={suite_run_id}",
        "--env",
        f"DAQIRI_BENCH_RUN_ID={process_run_id}",
    ]
    if container["gpus"]:
        command.extend(["--gpus", "all"])
    command.extend([container["digest"], *benchmark_argv])
    return command, name


def _pair_rate(
    scope: str, requested: float, pairs: list[dict[str, Any]], pair: dict[str, Any]
) -> float:
    if scope == "per_pair":
        return requested
    if scope == "aggregate":
        return requested / len(pairs)
    link_count = sum(1 for candidate in pairs if candidate["link"] == pair["link"])
    return requested / link_count


def _role_config(
    site: dict[str, Any], pair: dict[str, Any], direction: str, cell: dict[str, Any]
) -> str:
    link = site["links"][pair["link"]]
    local = link[direction]
    peer_direction = "rx" if direction == "tx" else "tx"
    peer = link[peer_direction]
    placement = pair[direction]
    mode = "client" if direction == "tx" else "server"
    bench_key = f"socket_bench_{mode}"
    region_name = f"DATA_{mode.upper()}_{pair['id'].upper()}"
    queue_name = f"{mode.upper()}_{pair['id'].upper()}"
    receive = direction == "rx"
    config = {
        "daqiri": {
            "cfg": {
                "version": 1,
                "stream_type": "socket",
                "master_core": placement["master_cpu"],
                "debug": False,
                "log_level": "info",
                "memory_regions": [
                    {
                        "name": region_name,
                        "kind": "host",
                        "affinity": placement["numa_node"],
                        "num_bufs": cell["buffers_per_region"],
                        "buf_size": cell["buffer_size_bytes"],
                    }
                ],
                "interfaces": [
                    {
                        "name": f"udp_{mode}_{pair['id']}",
                        "address": local["ip"],
                        "socket_config": {
                            "mode": mode,
                            "local_addr": f"udp://{local['ip']}:{placement['local_port']}",
                            "remote_addr": f"udp://{peer['ip']}:{placement['remote_port']}",
                            "max_payload_size": min(65535, cell["buffer_size_bytes"]),
                        },
                        "rx": {
                            "queues": [
                                {
                                    "name": f"{queue_name}_RX",
                                    "id": placement["queue_id"],
                                    "cpu_core": placement["io_cpu"],
                                    "batch_size": cell["batch_size"],
                                    "memory_regions": [region_name],
                                }
                            ]
                        },
                        "tx": {
                            "queues": [
                                {
                                    "name": f"{queue_name}_TX",
                                    "id": placement["queue_id"],
                                    "cpu_core": placement["io_cpu"],
                                    "batch_size": 1,
                                    "memory_regions": [region_name],
                                }
                            ]
                        },
                    }
                ],
            }
        },
        bench_key: {
            "cpu_core": placement["worker_cpu"],
            "server": mode == "server",
            "send": not receive,
            "receive": receive,
            "iterations": 0,
            "message_size": cell["message_size_bytes"],
            "server_address": link["rx"]["ip"],
            "server_port": pair["rx"]["local_port"],
        },
    }
    if mode == "client":
        config[bench_key]["client_address"] = link["tx"]["ip"]
    return yaml.safe_dump(config, sort_keys=False, default_flow_style=False)


def expand_matrix(suite: dict[str, Any]) -> list[dict[str, Any]]:
    matrix = suite["matrix"]
    cells: list[dict[str, Any]] = []
    for values in itertools.product(*(matrix[field] for field in MATRIX_FIELDS)):
        cell = dict(zip(MATRIX_FIELDS, values, strict=True))
        rate_text = str(cell["requested_rate_gbps"]).replace(".", "p")
        cell["cell_id"] = (
            f"udp-m{cell['message_size_bytes']}-n{cell['pair_count']}"
            f"-g{rate_text}-{suite['rate_scope']}"
            f"-b{cell['batch_size']}-buf{cell['buffer_size_bytes']}"
            f"-numbuf{cell['buffers_per_region']}"
        )
        cells.append(cell)
    return cells


def resolve_plan(
    suite: dict[str, Any], site: dict[str, Any], run_id: str
) -> dict[str, Any]:
    """Build the complete role/config/command plan for every repetition."""

    _safe_id(run_id, "run_id")
    plan: dict[str, Any] = {
        "schema_version": RESOLVED_VERSION,
        "run_id": run_id,
        "suite": copy.deepcopy(suite),
        "site": copy.deepcopy(site),
        "input_digests": {"suite": _input_digest(suite), "site": _input_digest(site)},
        "cells": [],
    }
    for cell in expand_matrix(suite):
        selected_pairs = site["pairs"][: cell["pair_count"]]
        resolved_cell = copy.deepcopy(cell)
        resolved_cell["rate_scope"] = suite["rate_scope"]
        resolved_cell["repetitions"] = []
        for repetition in range(1, suite["repetitions"] + 1):
            roles: list[dict[str, Any]] = []
            for pair in selected_pairs:
                for direction in ("rx", "tx"):
                    link_endpoint = site["links"][pair["link"]][direction]
                    host_id = link_endpoint["host"]
                    host = site["hosts"][host_id]
                    role_id = f"{direction}-{pair['id']}"
                    remote_dir = posixpath.join(
                        host["runtime_root"],
                        run_id,
                        "cells",
                        cell["cell_id"],
                        f"repetition-{repetition}",
                        role_id,
                    )
                    config_path = posixpath.join(remote_dir, "config.yaml")
                    seconds = suite["duration_seconds"]
                    benchmark_argv = [
                        _absolute_binary(host),
                        config_path,
                        "--mode",
                        "server" if direction == "rx" else "client",
                        "--seconds",
                        str(
                            seconds
                            + suite["drain_seconds"]
                            + (2 * suite["readiness_timeout_seconds"])
                            + 5
                            if direction == "rx"
                            else seconds
                        ),
                    ]
                    target: float | None = None
                    if direction == "tx":
                        target = _pair_rate(
                            suite["rate_scope"],
                            float(cell["requested_rate_gbps"]),
                            selected_pairs,
                            pair,
                        )
                        benchmark_argv.extend(["--target-gbps", f"{target:g}"])
                    process_run_id = (
                        f"{run_id}.{cell['cell_id']}.r{repetition}.{role_id}"
                    )
                    command, container_name = _execution(
                        host, benchmark_argv, process_run_id, run_id
                    )
                    role: dict[str, Any] = {
                        "role_id": role_id,
                        "process_run_id": process_run_id,
                        "container_name": container_name,
                        "benchmark_command": benchmark_argv,
                        "direction": direction,
                        "pair_id": pair["id"],
                        "link_id": pair["link"],
                        "host_id": host_id,
                        "runtime_dir": remote_dir,
                        "config_path": config_path,
                        "stdout_path": posixpath.join(remote_dir, "stdout.log"),
                        "stderr_path": posixpath.join(remote_dir, "stderr.log"),
                        "state_path": posixpath.join(remote_dir, "process.json"),
                        "exit_path": posixpath.join(remote_dir, "exit.json"),
                        "config_sha256": hashlib.sha256(
                            _role_config(site, pair, direction, cell).encode()
                        ).hexdigest(),
                        "config_yaml": _role_config(site, pair, direction, cell),
                        "command": command,
                        "cwd": host["worktree"],
                        "environment": {"DAQIRI_BENCH_SUITE_RUN_ID": run_id},
                        "placement": copy.deepcopy(pair[direction]),
                        "link": copy.deepcopy(link_endpoint),
                    }
                    if target is not None:
                        role["target_gbps"] = target
                    roles.append(role)
            collectors: list[dict[str, Any]] = []
            seen_collectors: set[tuple[str, str, str]] = set()
            for pair in selected_pairs:
                for direction in ("tx", "rx"):
                    endpoint = site["links"][pair["link"]][direction]
                    key = (endpoint["host"], pair["link"], direction)
                    if key in seen_collectors:
                        continue
                    seen_collectors.add(key)
                    host_id = endpoint["host"]
                    host = site["hosts"][host_id]
                    collector_id = f"mlnx-{direction}-{pair['link']}"
                    remote_dir = posixpath.join(
                        host["runtime_root"],
                        run_id,
                        "cells",
                        cell["cell_id"],
                        f"repetition-{repetition}",
                        collector_id,
                    )
                    collectors.append(
                        {
                            "role_id": collector_id,
                            "process_run_id": (
                                f"{run_id}.{cell['cell_id']}.r{repetition}.{collector_id}"
                            ),
                            "container_name": None,
                            "collector": "mlnx_perf",
                            "direction": direction,
                            "link_id": pair["link"],
                            "host_id": host_id,
                            "netdev": endpoint["netdev"],
                            "runtime_dir": remote_dir,
                            "stdout_path": posixpath.join(remote_dir, "stdout.log"),
                            "stderr_path": posixpath.join(remote_dir, "stderr.log"),
                            "state_path": posixpath.join(remote_dir, "process.json"),
                            "exit_path": posixpath.join(remote_dir, "exit.json"),
                            "command": [
                                "mlnx_perf",
                                "-i",
                                endpoint["netdev"],
                                "-t",
                                "1",
                            ],
                            "cwd": host["worktree"],
                            "environment": {
                                "DAQIRI_BENCH_SUITE_RUN_ID": run_id,
                                "PYTHONUNBUFFERED": "1",
                            },
                        }
                    )
            resolved_cell["repetitions"].append(
                {"repetition": repetition, "roles": roles, "collectors": collectors}
            )
        plan["cells"].append(resolved_cell)
    plan["plan_sha256"] = digest(plan)
    return plan


def load_resolved(path: Path) -> dict[str, Any]:
    plan = _load_yaml(path)
    if plan.get("schema_version") != RESOLVED_VERSION:
        raise ManifestError(f"resolved manifest must use {RESOLVED_VERSION}")
    recorded = plan.get("plan_sha256")
    unhashed = copy.deepcopy(plan)
    unhashed.pop("plan_sha256", None)
    actual = digest(unhashed)
    if recorded != actual:
        raise ManifestError("resolved manifest hash does not match immutable plan")
    validate_suite(plan["suite"])
    validate_site(plan["site"])
    validate_combination(plan["suite"], plan["site"])
    return plan


def canonical_json(value: Any) -> str:
    return _canonical_json(value).decode()
