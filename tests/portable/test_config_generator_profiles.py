# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import pytest

from scripts.daqiri_config import (
    ConfigError,
    RawPairSpec,
    SocketPairSpec,
    generate_raw_pair,
    generate_raw_roles,
    generate_socket_pair,
    render_document,
)


def socket_spec(transport: str) -> SocketPairSpec:
    return SocketPairSpec(
        transport=transport,
        client_address="1.1.1.1",
        server_address="2.2.2.2",
        client_port=5002,
        server_port=5001,
        client_master_core=3,
        server_master_core=4,
        client_rx_core=5,
        client_tx_core=6,
        server_rx_core=7,
        server_tx_core=8,
        client_worker_core=9,
        server_worker_core=10,
        message_size=1024,
        buffer_size=65536,
        num_bufs=128,
        rx_num_bufs=256 if transport == "roce" else None,
        tx_num_bufs=64 if transport == "roce" else None,
        rx_batch_size=32 if transport != "roce" else None,
        tx_depth=64 if transport == "roce" else None,
    )


@pytest.mark.parametrize("transport", ["udp", "tcp", "roce"])
def test_socket_pair_emits_independent_concrete_roles(transport: str) -> None:
    documents = generate_socket_pair(socket_spec(transport))
    assert set(documents) == {"tx", "rx"}
    for role, document in documents.items():
        assert "<" not in render_document(document)
        config = document["daqiri"]["cfg"]
        assert len(config["interfaces"]) == 1
        assert config["interfaces"][0]["socket_config"]["mode"] == (
            "client" if role == "tx" else "server"
        )
    expected_prefix = "rdma_bench" if transport == "roce" else "socket_bench"
    assert f"{expected_prefix}_client" in documents["tx"]
    assert f"{expected_prefix}_server" in documents["rx"]


def test_roce_pair_preserves_direction_specific_flow_control_storage() -> None:
    documents = generate_socket_pair(socket_spec("roce"))
    for document in documents.values():
        regions = {
            region["name"]: region["num_bufs"]
            for region in document["daqiri"]["cfg"]["memory_regions"]
        }
        rx_name = next(name for name in regions if "_RX_" in name)
        tx_name = next(name for name in regions if "_TX_" in name)
        assert regions[rx_name] == 256
        assert regions[tx_name] == 64


def raw_spec(**overrides) -> RawPairSpec:
    values = {
        "tx_address": "0005:03:00.0",
        "rx_address": "0005:03:00.1",
        "master_core": 3,
        "tx_queue_cores": (4,),
        "rx_queue_cores": (5,),
        "tx_worker_cores": (6,),
        "rx_worker_cores": (7,),
        "eth_dst_addr": "48:b0:2d:f4:04:24",
        "engine": "ibverbs",
    }
    values.update(overrides)
    return RawPairSpec(**values)


@pytest.mark.parametrize("engine", ["dpdk", "ibverbs"])
@pytest.mark.parametrize("transform", ["none", "vlan", "vxlan", "gre", "nvgre"])
def test_raw_transform_matrix(transform: str, engine: str) -> None:
    document = generate_raw_pair(raw_spec(transform=transform, engine=engine))
    tx = document["daqiri"]["cfg"]["interfaces"][0]["tx"]
    if transform == "none":
        assert "flows" not in tx
    else:
        assert tx["flows"][0]["actions"][0]["type"] in (
            "vlan_push",
            "tunnel_encap",
        )
        rx = document["daqiri"]["cfg"]["interfaces"][1]["rx"]
        match = rx["flows"][0]["match"]
        if engine == "dpdk":
            assert match == {}
        elif transform == "vlan":
            assert match == {"udp_src": 4096, "udp_dst": 4096}
        elif transform == "vxlan":
            assert match == {"udp_src": 49152, "udp_dst": 4789}
        else:
            assert match == {
                "ipv4_src": "192.0.2.1",
                "ipv4_dst": "192.0.2.2",
            }


@pytest.mark.parametrize("tx_count,rx_count", [(1, 1), (1, 2), (2, 1), (2, 2)])
def test_raw_multi_queue_matrix_routes_every_flow(tx_count: int, rx_count: int) -> None:
    document = generate_raw_pair(
        raw_spec(
            engine="dpdk",
            memory_kind="host_pinned",
            tx_queue_cores=tuple(range(10, 10 + tx_count)),
            rx_queue_cores=tuple(range(20, 20 + rx_count)),
            tx_worker_cores=tuple(range(30, 30 + tx_count)),
            rx_worker_cores=tuple(range(40, 40 + rx_count)),
        )
    )
    config = document["daqiri"]["cfg"]
    assert len(config["memory_regions"]) == tx_count + rx_count
    flows = config["interfaces"][1]["rx"]["flows"]
    assert len(flows) == max(tx_count, rx_count)
    assert [flow["action"]["id"] for flow in flows] == [
        index % rx_count for index in range(max(tx_count, rx_count))
    ]
    if tx_count == 1 and rx_count == 2:
        assert document["bench_tx"][0]["udp_dst_port"] == "4096-4097"


def test_raw_pair_can_split_for_cross_host_deployment() -> None:
    documents = generate_raw_roles(raw_spec())
    assert list(documents["tx"]) == ["daqiri", "bench_tx"]
    assert list(documents["rx"]) == ["daqiri", "bench_rx"]
    assert len(documents["tx"]["daqiri"]["cfg"]["interfaces"]) == 1
    assert len(documents["rx"]["daqiri"]["cfg"]["interfaces"]) == 1


def test_invalid_profile_inputs_fail_before_rendering() -> None:
    with pytest.raises(ConfigError, match="UDP message_size"):
        socket_spec("udp").__class__(
            **{**socket_spec("udp").__dict__, "message_size": 65508, "buffer_size": 65508}
        )
    with pytest.raises(ConfigError, match="one tx_worker_core"):
        raw_spec(tx_queue_cores=(4, 5), tx_worker_cores=(6,))
    with pytest.raises(ConfigError, match="explicit dpdk or ibverbs"):
        raw_spec(transform="vlan", engine=None)
    with pytest.raises(ConfigError, match="header_size must be at least 42"):
        raw_spec(header_size=41)
    with pytest.raises(ConfigError, match="IPv4 total-length"):
        raw_spec(header_size=42, payload_size=65508)
    with pytest.raises(ConfigError, match="at least batch_size"):
        raw_spec(engine="ibverbs", batch_size=2, num_bufs=1)
    with pytest.raises(ConfigError, match="at least twice batch_size"):
        raw_spec(engine="dpdk", batch_size=2, num_bufs=3)
    with pytest.raises(ConfigError, match="rx_batch_size must not exceed num_bufs"):
        spec = socket_spec("udp")
        spec.__class__(**{**spec.__dict__, "num_bufs": 16, "rx_batch_size": 32})
    with pytest.raises(ConfigError, match="supported only for RoCE"):
        spec = socket_spec("udp")
        spec.__class__(**{**spec.__dict__, "rx_num_bufs": 512})
    with pytest.raises(
        ConfigError, match="rx_batch_size is supported only for TCP/UDP"
    ):
        spec = socket_spec("roce")
        spec.__class__(**{**spec.__dict__, "rx_batch_size": 32})
    with pytest.raises(
        ConfigError, match="iterations is supported only for TCP/UDP"
    ):
        spec = socket_spec("roce")
        spec.__class__(**{**spec.__dict__, "iterations": 10})
    with pytest.raises(ConfigError, match="supported only for RoCE"):
        spec = socket_spec("udp")
        spec.__class__(**{**spec.__dict__, "rx_depth": 7})
    with pytest.raises(ConfigError, match="supported only for RoCE"):
        spec = socket_spec("tcp")
        spec.__class__(**{**spec.__dict__, "roce_transport_mode": "UD"})


def test_production_profiles_do_not_require_benchmark_worker_cores() -> None:
    raw = generate_raw_pair(
        raw_spec(
            include_benchmark=False,
            tx_worker_cores=(),
            rx_worker_cores=(),
            header_size=1,
            payload_size=1,
        )
    )
    assert "bench_tx" not in raw and "bench_rx" not in raw

    spec = socket_spec("udp")
    socket = generate_socket_pair(
        spec.__class__(
            **{
                **spec.__dict__,
                "include_benchmark": False,
                "client_worker_core": None,
                "server_worker_core": None,
            }
        )
    )
    assert all(not any(key.startswith("socket_bench") for key in doc) for doc in socket.values())
