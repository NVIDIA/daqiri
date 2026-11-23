# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Resolve perftest endpoints and build a native worker configuration."""

from __future__ import annotations

import ipaddress
import json
import os
import re
import subprocess
from dataclasses import asdict, dataclass
from itertools import chain, cycle
from pathlib import Path

from .core import ConfigError
from .profiles import RAW_MAX_FRAME_SIZE, UDP_IPV4_HEADER_SIZE

ROLES = ("server", "client")
MAC = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
BDF = re.compile(r"(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]")
MEMORY_KINDS = ("host", "host_pinned", "device", "huge")
MIN_ETHERNET_FRAME_SIZE = 60  # L2 bytes excluding FCS.
ROCE_MAX_WR = 1024
# A single receive occupies one of the engine's shared TX burst slots.
# Keep this capacity in sync with RdmaEngine::setup_pools_and_rings().
ROCE_BURST_SLOTS = 2047


@dataclass(frozen=True)
class Region:
    kind: str
    buf_size: int
    num_bufs: int
    affinity: int = 0


@dataclass(frozen=True)
class Interface:
    name: str
    bdf: str = ""
    mac: str = ""


def region_arg(value: str) -> Region:
    parts = value.split(":")
    if len(parts) not in (3, 4) or parts[0] not in MEMORY_KINDS:
        raise ConfigError("expected kind:buffer_bytes:buffer_count[:affinity]")
    try:
        result = Region(parts[0], *(int(part) for part in parts[1:]))
    except ValueError as exc:
        raise ConfigError("memory region dimensions must be integers") from exc
    if not (0 < result.buf_size <= 65535 and 0 < result.num_bufs < 2**31
            and 0 <= result.affinity <= 65535):
        raise ConfigError("invalid memory region size, buffer count or affinity")
    return result


def run_ip(*args: str) -> list[dict]:
    try:
        result = subprocess.run(["ip", "-j", *args], capture_output=True, text=True,
                                check=True, timeout=5)
        return json.loads(result.stdout)
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        raise ConfigError(f"cannot inspect network with ip {' '.join(args)}: {exc}") from exc


def resolve_interface(value: str, sysfs: Path = Path("/sys"), raw: bool = False) -> Interface:
    if BDF.fullmatch(value):
        bdf = value.lower()
        if len(bdf) == 7:
            bdf = f"0000:{bdf}"
        names = sorted((sysfs / "bus/pci/devices" / bdf / "net").glob("*"))
        if len(names) > 1:
            raise ConfigError(f"{bdf} maps to multiple netdevs. Specify one by name.")
        if not names:
            raise ConfigError(f"no netdev found for {bdf}")
        value = names[0].name
    if not value or value in (".", "..") or "/" in value:
        raise ConfigError(f"invalid netdev name {value!r}")
    path = sysfs / "class/net" / value
    try:
        mac = (path / "address").read_text().strip()
    except OSError as exc:
        raise ConfigError(f"cannot resolve netdev {value}: {exc}") from exc
    device = (path / "device").resolve().name
    bdf = device if BDF.fullmatch(device) else ""
    if raw and (not bdf or not MAC.fullmatch(mac)):
        raise ConfigError(f"{value} must be a PCI-backed Ethernet interface")
    return Interface(value, bdf, mac)


def socket_interfaces(args, roles: tuple[str, ...], warnings: list[str]) -> dict[str, Interface]:
    requested = {role: getattr(args, f"{role}_interface") if args.role == "local"
                 else args.interface for role in roles}
    if not any(requested.values()) and all(
        ipaddress.IPv4Address(getattr(args, f"{role}_address")).is_loopback for role in roles
    ):
        return {role: Interface("lo") for role in roles}
    try:
        addresses = run_ip("-4", "address", "show")
    except ConfigError as exc:
        if any(requested.values()):
            raise
        warnings.append(str(exc))
        return {}
    result = {}
    for role in roles:
        address = getattr(args, f"{role}_address")
        matches = [x["ifname"] for x in addresses
                   if any(y.get("local") == address for y in x.get("addr_info", []))]
        if requested[role]:
            interface = resolve_interface(requested[role])
            if interface.name not in matches:
                raise ConfigError(f"{address} is not assigned to {interface.name}")
            result[role] = interface
        elif len(matches) == 1:
            result[role] = resolve_interface(matches[0])
        elif not matches and not ipaddress.IPv4Address(address).is_loopback:
            raise ConfigError(f"{address} is not assigned in this network namespace")
    return result


def frame_size(args, size: int) -> int:
    return size + UDP_IPV4_HEADER_SIZE if args.raw and args.size_kind == "message" else size


def resolve_sizes(args) -> None:
    packet_sizes = args.packet_sizes or ([args.packet_size] if args.packet_size else [])
    message_sizes = args.message_sizes or ([args.message_size] if args.message_size else [])
    if packet_sizes and not args.raw:
        raise ConfigError("--packet-size and --packet-sizes require raw Ethernet")
    args.size_kind = "packet" if args.raw and not message_sizes else "message"
    args.named_sender = args.engine == "ibverbs" and args.size_kind == "message"
    args.sizes = packet_sizes or message_sizes or args.sizes or [
        args.size or (64 if args.test == "lat" else 1024)
    ]
    if args.size_kind == "packet":
        minimum, maximum = 64, RAW_MAX_FRAME_SIZE
    else:
        minimum = 8 if args.test == "lat" else 1
        maximum = (RAW_MAX_FRAME_SIZE - UDP_IPV4_HEADER_SIZE if args.raw
                   else 65507 if args.transport == "udp" else 65535)
    if len(set(args.sizes)) != len(args.sizes) or not all(minimum <= n <= maximum for n in args.sizes):
        raise ConfigError(f"{args.size_kind} sizes must be distinct and between "
                          f"{minimum} and {maximum} bytes")


def isolated_cpus() -> set[int]:
    try:
        listing = Path("/sys/devices/system/cpu/isolated").read_text().strip()
    except OSError:
        return set()
    isolated = set()
    for field in listing.split(",") if listing else ():
        first, _, last = field.partition("-")
        isolated.update(range(int(first), int(last or first) + 1))
    return isolated


def raw_directions(args, role: str, rtt: bool) -> tuple[bool, bool]:
    both = args.bidirectional or rtt
    return role == "client" or both, role == "server" or both


def resolve_cpus(args, warnings: list[str]) -> None:
    allowed = os.sched_getaffinity(0)
    isolated = isolated_cpus()
    usable = allowed & isolated
    lane_count = args.queues * len(args.roles)
    layout = {"app_cores": range(lane_count)}
    if args.test == "bw" and args.transport == "tcp" and args.bidirectional:
        layout["rx_app_cores"] = range(lane_count)
    elif args.rx_app_cores is not None:
        raise ConfigError("--rx-app-cores requires bidirectional TCP bandwidth")

    # Paired latency can require TX and RX on each peer for an RTT echo.
    paired_latency = args.test == "lat" and args.role != "local"
    for index, direction in enumerate(("tx", "rx")):
        name = f"{direction}_cores"
        if getattr(args, f"{direction}_poll") == "indirect":
            if args.raw:
                layout[name] = [i for i in range(lane_count) if raw_directions(
                    args, args.roles[i // args.queues], paired_latency)[index]]
            else:
                # Linux socket TX runs on the caller's application core.
                layout[name] = range(lane_count) if args.roce or direction == "rx" else ()
        elif getattr(args, name) is not None:
            raise ConfigError(f"--{direction}-cores requires indirect polling")

    explicit: set[int] = set()
    automatic = 0
    for name, indices in layout.items():
        cores = getattr(args, name)
        if cores is None:
            automatic += len(indices)
            continue
        if len(cores) not in (1, lane_count) or any(cpu not in allowed for cpu in cores):
            raise ConfigError(f"{name.replace('_', ' ')} requires one or {lane_count} allowed CPUs")
        cores = cores * lane_count if len(cores) == 1 else cores
        setattr(args, name, cores)
        explicit.update(cores[i] for i in indices)

    # Opposite allocation order helps separate peers with the same host affinity.
    reverse = args.role == "client"
    if args.master_core is None:
        housekeeping = allowed - usable - explicit
        spare = allowed - explicit
        args.master_core = (sorted(housekeeping, reverse=reverse) or
                            sorted(spare, reverse=not reverse) or sorted(allowed))[0]
    elif args.master_core not in allowed:
        raise ConfigError("master core requires an allowed CPU")

    available = usable - ({args.master_core} - explicit)
    requested = automatic + len(explicit)
    if not isolated:
        warnings.append("No isolated CPUs detected. Using the process's allowed CPUs.")
    elif not usable:
        warnings.append("No isolated CPUs are available in the process affinity mask.")
    elif len(available) < requested:
        warnings.append("Too few isolated CPUs for the benchmark layout "
                        f"({len(available)} available, {requested} requested).")
    if usable and explicit - usable:
        cpus = ", ".join(map(str, sorted(explicit - usable)))
        warnings.append(f"Explicit benchmark CPUs outside the kernel isolation mask {cpus}")

    preferred = sorted(usable, reverse=reverse) + sorted(allowed - usable, reverse=reverse)
    reserved = explicit | {args.master_core}
    free = [cpu for cpu in preferred if cpu not in reserved]
    if automatic > len(free):
        warnings.append("CPU affinity is limited. Some automatic benchmark assignments share CPUs.")
    assignments = chain(free, cycle(preferred))
    args.benchmark_cpus = explicit.copy()
    for name, indices in layout.items():
        if getattr(args, name) is not None:
            continue
        # Keep a value per lane, including configuration slots with no poller.
        cores = list(args.app_cores) if name != "app_cores" else [0] * lane_count
        for i in indices:
            cores[i] = next(assignments)
            args.benchmark_cpus.add(cores[i])
        setattr(args, name, cores)


def resolve(args) -> list[str]:
    warnings = []
    args.raw = args.transport == "raw"
    args.roce = args.transport == "roce"
    args.local_route = False
    if args.raw and not args.engine:
        raise ConfigError("raw Ethernet requires --engine ibverbs or --engine dpdk")
    if not args.raw and args.engine:
        raise ConfigError("--engine applies to raw Ethernet. --transport roce selects ibverbs.")
    resolve_sizes(args)
    supports_direct = args.engine == "ibverbs" or args.roce
    default_poll = "direct" if args.test == "lat" and supports_direct else "indirect"
    args.tx_poll = args.tx_poll or default_poll
    args.rx_poll = args.rx_poll or default_poll
    if "direct" in (args.tx_poll, args.rx_poll) and not supports_direct:
        raise ConfigError("direct polling requires raw ibverbs or RoCE")
    if args.topology in ("hw-loopback", "sw-loopback"):
        engine = "ibverbs" if args.topology == "hw-loopback" else "dpdk"
        if args.role != "local" or args.engine != engine:
            raise ConfigError(f"{args.topology} requires a local raw {engine} run")
        if args.client_interface or args.server_interface:
            raise ConfigError("loopback expects only --interface for hardware loopback")
        if args.topology == "sw-loopback" and (args.interface or args.bidirectional or args.queues != 1):
            raise ConfigError("software loopback requires one queue pair, one direction and no interface")
    if args.topology == "two-host" and args.role == "local":
        raise ConfigError("two-host requires --role server or client")
    if args.hardware_rtt and (args.test != "lat" or args.engine != "ibverbs" or args.role == "local"):
        raise ConfigError("--hardware-rtt requires paired raw ibverbs latency roles")
    if args.role == "local" and args.topology == "same-host" and args.interface:
        raise ConfigError("local same-host runs use --client-interface and --server-interface")
    if args.role != "local" and (args.client_interface or args.server_interface):
        raise ConfigError("paired runs use --interface for their local port")
    if args.role == "local" and (args.control or args.peer_mac):
        raise ConfigError("--control and --peer-mac apply to paired runs")
    if not args.raw and args.peer_mac:
        raise ConfigError("--peer-mac applies to raw Ethernet")
    if args.raw and args.role != "local" and not (args.peer_mac and MAC.fullmatch(args.peer_mac)):
        raise ConfigError("paired raw Ethernet requires a valid --peer-mac")
    if not args.raw and not (args.client_address and args.server_address):
        raise ConfigError("set --client-address and --server-address")
    args.client_address = args.client_address or "192.0.2.1"
    args.server_address = args.server_address or "192.0.2.2"
    if args.roce and args.client_address == args.server_address:
        raise ConfigError("RoCE requires distinct client and server IPv4 addresses")
    if args.role != "local" and not args.control:
        if args.raw:
            raise ConfigError("paired raw Ethernet requires a separate IPv4 --control address:port")
        args.control = (args.server_address, 18500)
    args.roles = ROLES if args.role == "local" else (args.role,)
    if args.raw:
        if args.topology == "sw-loopback":
            args.interfaces = {role: Interface("loopback", mac=f"02:00:00:00:00:0{i}")
                               for i, role in enumerate(ROLES, 1)}
        elif args.topology == "hw-loopback":
            interface = resolve_interface(args.interface or "", raw=True)
            args.interfaces = dict.fromkeys(ROLES, interface)
        else:
            args.interfaces = {
                role: resolve_interface((getattr(args, f"{role}_interface") if args.role == "local"
                                         else args.interface) or "", raw=True)
                for role in args.roles
            }
            if args.role == "local" and len({x.bdf for x in args.interfaces.values()}) != 2:
                raise ConfigError("raw same-host requires two distinct PCI ports")
    else:
        args.interfaces = socket_interfaces(args, args.roles, warnings)
        for role in args.roles:
            local = getattr(args, f"{role}_address")
            peer = args.client_address if role == "server" else args.server_address
            if all(ipaddress.IPv4Address(x).is_loopback for x in (local, peer)):
                warnings.append("IPv4 loopback endpoints use the local kernel path")
                args.local_route = True
                break
            try:
                route = run_ip("-4", "route", "get", peer, "from", local)[0]
            except (ConfigError, IndexError) as exc:
                warnings.append(f"cannot verify {role} route: {exc}")
                continue
            if route.get("type") == "local" or route.get("dev") == "lo":
                warnings.append(f"{role} route to {peer} stays in the local kernel")
                args.local_route = True
            elif role in args.interfaces and route.get("dev") != args.interfaces[role].name:
                raise ConfigError(f"{role} route to {peer} uses {route.get('dev')}, "
                                  f"expected {args.interfaces[role].name}")

    args.batch_size = args.batch_size or (1 if args.test == "lat" else 32)
    if args.batch_size > 65535:
        raise ConfigError("--batch-size must not exceed 65535")
    if args.test == "lat" and args.batch_size != 1:
        raise ConfigError("latency requires --batch-size 1")
    if args.transport == "udp" and args.batch_size > 32:
        raise ConfigError("UDP supports batch sizes up to 32")
    args.tx_depth = args.tx_depth or 128
    if args.roce and max(args.tx_depth, args.rx_depth or 0, args.batch_size) > ROCE_MAX_WR:
        raise ConfigError("RoCE queue depths and batch size must not exceed 1024")
    if args.roce and args.tx_poll == "indirect" and args.batch_size > args.tx_depth:
        raise ConfigError("RoCE batch size exceeds TX depth")
    if args.roce and args.test == "bw":
        rx_lanes = args.queues * sum(role == "server" or args.bidirectional for role in args.roles)
        tx_lanes = args.queues * sum(role == "client" or args.bidirectional for role in args.roles)
        tx_batch = 1 if args.tx_poll == "direct" else args.batch_size
        tx_slots = tx_lanes * (args.tx_depth // tx_batch)
        rx_limit = min(ROCE_MAX_WR, (ROCE_BURST_SLOTS - tx_slots) // rx_lanes) if rx_lanes else ROCE_MAX_WR
        if rx_limit < 1 or (args.rx_depth and args.rx_depth > rx_limit):
            raise ConfigError("RoCE receive depth exceeds the shared burst pool budget "
                              f"({max(0, rx_limit)} receives per queue with this layout)")
        args.rx_depth = args.rx_depth or min([rx_limit] + [r.num_bufs for r in args.rx_mr or ()])
    else:
        args.rx_depth = args.rx_depth or 1

    default_kind = ("huge" if args.engine == "dpdk" else
                    "host_pinned" if (args.raw or args.roce) else "host")
    tx_regions = args.tx_mr or ()
    args.prepared_device_frames = (args.raw and args.engine == "ibverbs" and args.test == "bw"
                                   and not args.named_sender and args.tx_poll == "indirect"
                                   and args.batch_size > 1 and len(tx_regions) == 1
                                   and tx_regions[0].kind == "device"
                                   and tx_regions[0].buf_size >= max(args.sizes))
    if args.raw and not (args.named_sender or args.prepared_device_frames):
        header_bytes = 0
        for region in args.tx_mr or ():
            if header_bytes >= UDP_IPV4_HEADER_SIZE:
                break
            if region.kind == "device":
                if args.engine == "dpdk":
                    raise ConfigError("DPDK TX needs 42 CPU-accessible bytes before device segments "
                                      "for the Ethernet/IPv4/UDP header")
                args.named_sender = True
                break
            header_bytes += region.buf_size
    for direction in ("tx", "rx"):
        depth = getattr(args, f"{direction}_depth") if args.roce else 0
        mode = getattr(args, f"{direction}_poll")
        batch = args.batch_size if mode == "indirect" else 1
        default_count = (max(24576, 8192 + 4 * batch) if args.engine == "dpdk"
                         else max(4096, 2 * batch, depth))
        attr = f"{direction}_mr"
        minimum_size = max(args.sizes)
        if args.raw:
            frame = frame_size(args, minimum_size)
            minimum_size = (frame - UDP_IPV4_HEADER_SIZE if direction == "tx" and args.named_sender
                            else max(MIN_ETHERNET_FRAME_SIZE, frame))
        regions = getattr(args, attr) or [Region(default_kind, minimum_size, default_count)]
        if len(regions) > (4 if args.raw else 1):
            raise ConfigError(f"too many {direction.upper()} segments for {args.transport}")
        if not args.raw and not args.roce and regions[0].kind != "host":
            raise ConfigError("TCP/UDP packet storage uses host memory")
        minimum_buffers = max(depth, batch * (2 if args.engine == "dpdk" else 1))
        if sum(region.buf_size for region in regions) < minimum_size:
            raise ConfigError(f"{direction.upper()} memory must cover {minimum_size} bytes "
                              "per packet or message")
        if min(region.num_bufs for region in regions) < minimum_buffers:
            raise ConfigError(f"{direction.upper()} memory does not cover the size and queue depth")
        if args.raw and sum(region.buf_size for region in regions) > RAW_MAX_FRAME_SIZE:
            raise ConfigError(f"{direction.upper()} raw regions exceed the {RAW_MAX_FRAME_SIZE}-byte limit")
        setattr(args, attr, regions)
    args.opaque_payload = any(region.kind == "device" for region in args.tx_mr + args.rx_mr)
    if args.topology == "sw-loopback" and (
        [(r.kind, r.buf_size, r.affinity) for r in args.tx_mr]
        != [(r.kind, r.buf_size, r.affinity) for r in args.rx_mr]
    ):
        raise ConfigError("software loopback returns TX storage and requires matching RX/TX layouts")

    port_count = args.queues * len(args.sizes)
    if args.raw:
        data_ports = [(args.server_port, 2 * port_count)]
    elif args.roce:
        data_ports = [(args.server_port, len(args.sizes))]
    else:
        data_ports = [(args.server_port, port_count), (args.client_port, port_count)]
        if args.client_address == args.server_address and max(args.client_port, args.server_port) < (
            min(args.client_port, args.server_port) + port_count
        ):
            raise ConfigError("client and server port ranges overlap on the same IP")
    for base, count in data_ports:
        if base + count - 1 > 65535:
            raise ConfigError("data port range exceeds 65535")
        if args.control and args.transport in ("tcp", "roce") and base <= args.control[1] < base + count:
            raise ConfigError("TCP control port overlaps a data port range")

    resolve_cpus(args, warnings)
    return list(dict.fromkeys(warnings))


def build_config(args, case: int, *, hardware: bool, rtt: bool) -> dict:
    size = args.sizes[case]
    if args.hardware_rtt and args.role == "server" and not args.bidirectional:
        hardware = False
    config = {"version": 1, "stream_type": "raw" if args.raw else "socket",
              "master_core": args.master_core, "log_level": "warn",
              "memory_regions": [], "interfaces": []}
    if args.raw:
        config["engine"] = args.engine
        if args.topology in ("hw-loopback", "sw-loopback"):
            config["loopback"] = args.topology[:2]

    lanes = []
    interfaces = {}
    for role_index, role in enumerate(args.roles):
        server = role == "server"
        other = "client" if server else "server"
        local = getattr(args, f"{role}_address")
        peer = getattr(args, f"{other}_address")
        interface = args.interfaces.get(role)
        peer_interface = args.interfaces.get(other)
        for queue in range(args.queues):
            lane_index = role_index * args.queues + queue
            loopback = args.topology in ("hw-loopback", "sw-loopback")
            name = ("loopback" if loopback else (role if args.raw or args.roce
                                                else f"{role}_{queue}"))
            qid = queue if args.raw or args.roce else 0
            raw_transmits, raw_receives = raw_directions(args, role, args.test == "lat" and rtt)
            if args.topology == "hw-loopback" and server:
                # Use dense queue IDs independently in RX and TX when only one role
                # uses each direction. The two logical ends share one PCI function.
                if raw_transmits:
                    qid += args.queues
            port_index = case * args.queues + queue
            server_port = args.server_port + (case if args.roce else port_index)
            client_port = args.client_port + port_index
            local_port, peer_port = (server_port, client_port) if server else (client_port, server_port)
            flow_base = args.server_port + case * args.queues * 2
            forward_port = flow_base + queue
            reverse_port = forward_port + args.queues
            tx_flow, rx_flow = ((reverse_port, forward_port) if server
                                else (forward_port, reverse_port))
            lane = {"role": role, "queue": queue, "qid": qid, "interface": name,
                    "cpu": args.app_cores[lane_index], "server_port": server_port,
                    "tx_udp_port": tx_flow, "local_ip": local, "peer_ip": peer}
            if args.rx_app_cores:
                lane["rx_app_cpu"] = args.rx_app_cores[lane_index]
            if args.raw:
                lane.update(src_mac=interface.mac, dst_mac=(peer_interface.mac if peer_interface
                                                              else args.peer_mac),
                            rx_flow_id=0 if loopback and args.engine == "dpdk" else 1 + lane_index)
            lanes.append(lane)
            if name not in interfaces:
                intf = {"name": name, "address": ((interface.bdf or interface.name)
                                                 if args.raw else local)}
                if not args.raw:
                    endpoint = f"{args.transport}://{local}"
                    if not args.roce or server:
                        endpoint += f":{local_port}"
                    intf["socket_config"] = {"mode": role, "local_addr": endpoint}
                    if args.roce:
                        intf["roce_config"] = {"transport_mode": "RC"}
                    else:
                        intf["socket_config"].update(
                            remote_addr=f"{args.transport}://{peer}:{peer_port}",
                            max_payload_size=max(args.sizes), retry_connect_s=1,
                        )
                interfaces[name] = intf
            intf = interfaces[name]
            for direction, enabled in (("tx", raw_transmits), ("rx", raw_receives)):
                if args.raw and not enabled:
                    continue
                region_specs = [
                    dict(asdict(region), name=f"{role}_{queue}_{direction}_{index}", access=["local"])
                    for index, region in enumerate(getattr(args, f"{direction}_mr"))
                ]
                if direction == "tx":
                    for region in region_specs:
                        if region["kind"] == "device":
                            region["zero_init"] = True
                lane[direction] = region_specs
                config["memory_regions"].extend(region_specs)
                q = {"name": f"{role}_{direction}_{queue}", "id": qid}
                if not args.roce:
                    q["memory_regions"] = [region["name"] for region in region_specs]
                mode = getattr(args, f"{direction}_poll")
                if mode == "direct":
                    q["poll_mode"] = mode
                else:
                    q.update(cpu_core=getattr(args, f"{direction}_cores")[lane_index],
                             batch_size=args.batch_size)
                    if direction == "rx" and args.raw:
                        q["timeout_us"] = 100
                section = intf.setdefault(direction, {"queues": []})
                section["queues"].append(q)
                if direction == "tx" and hardware and args.raw:
                    # The accurate-send CQ selection puts TX in the same packed
                    # wall-clock format as RX. Perftest sends without WAIT WQEs.
                    section["accurate_send"] = True
                if direction == "rx":
                    if hardware:
                        section["hardware_timestamps"] = True
                    if args.raw and args.topology != "sw-loopback":
                        section["flow_isolation"] = True
                        section.setdefault("flows", []).append(
                            dict(name=f"{role}_{queue}", id=1 + lane_index,
                                 match={"udp_src": rx_flow, "udp_dst": rx_flow},
                                 action={"type": "queue", "id": qid})
                        )
    config["interfaces"] = list(interfaces.values())
    return {"daqiri": {"cfg": config},
            "perftest": {"test": args.test, "role": args.role, "transport": args.transport,
                         "size": frame_size(args, size),
                         "message_size": size if args.raw and args.size_kind == "message" else 0,
                         "named_sender": args.named_sender,
                         "prepared_device_frames": args.prepared_device_frames,
                         "opaque_payload": args.opaque_payload,
                         "batch": 1 if args.tx_poll == "direct" else args.batch_size,
                         "tx_direct": args.tx_poll == "direct", "rx_direct": args.rx_poll == "direct",
                         "bidirectional": args.bidirectional, "hardware": hardware, "rtt": rtt,
                         "hardware_rtt": args.hardware_rtt,
                         "seconds": args.seconds, "warmup_seconds": args.warmup_seconds,
                         "samples": args.samples, "warmup_samples": args.warmup_samples,
                         "realtime_priority": args.realtime_priority or 0,
                         "timeout": args.timeout, "client_ip": args.client_address,
                         "server_ip": args.server_address, "tx_depth": args.tx_depth,
                         "rx_depth": args.rx_depth if args.test == "bw" else 1,
                         "lanes": lanes}}
