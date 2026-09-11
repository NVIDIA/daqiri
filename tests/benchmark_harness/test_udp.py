# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import pytest
from benchmark_harness.udp import (
    ResultParseError,
    parse_mlnx_perf,
    parse_socket_completion,
    verdict_from_evidence,
)


def clean_evidence():
    samples = {"stable_gbps": [24.9, 25.0, 25.1]}
    return {
        "application": {
            "tx_packets": 1000,
            "rx_packets": 1000,
            "tx_bytes": 8_000_000,
            "rx_bytes": 8_000_000,
        },
        "daqiri_queue_counters": {
            "tx-pair-0": {"tx_pkts": 1000},
            "rx-pair-0": {"rx_pkts": 1000},
        },
        "kernel_udp": {"InErrors": 0, "RcvbufErrors": 0, "ReasmFails": 0},
        "nic_rx_discards": {"link-0.rx_out_of_buffer": 0},
        "phy": {
            "links": {
                "link-0": {
                    "tx_packets": 1000,
                    "rx_packets": 1000,
                    "tx_bytes": 8_000_000,
                    "rx_bytes": 8_000_000,
                }
            }
        },
        "mlnx_perf": {"link-0": {"tx": samples, "rx": samples}},
        "requested_rate": {"aggregate_gbps": 25.0},
        "achieved_rate": {"mlnx_rx_aggregate_gbps": 25.0},
        "requested_duration_seconds": 30,
        "active_duration_seconds": 30.0,
    }


@pytest.fixture
def policy():
    return {
        "max_delivery_loss_packets": 0,
        "max_kernel_udp_errors": 0,
        "max_nic_rx_discards": 0,
        "phy_packet_tolerance_percent": 0.5,
        "minimum_rate_percent": 95,
        "minimum_mlnx_samples": 2,
        "minimum_duration_percent": 95,
    }


def test_socket_parser_accepts_one_complete_summary():
    value = parse_socket_completion(
        "Client complete: sent_packets=2 sent_bytes=16000 seconds=1.25\n", "tx"
    )
    assert value == {"sent_packets": 2, "sent_bytes": 16000, "seconds": 1.25}


@pytest.mark.parametrize(
    "text",
    [
        "",
        "Client complete: sent_packets=1\n",
        "Client complete: sent_packets=1 sent_bytes=8 seconds=1\nClient complete: sent_packets=1 sent_bytes=8 seconds=1\n",
    ],
)
def test_socket_parser_rejects_missing_or_ambiguous_summaries(text):
    with pytest.raises(ResultParseError):
        parse_socket_completion(text, "tx")


def test_mlnx_parser_discards_startup_and_shutdown_samples():
    text = "\n".join(
        f"rx_bytes_phy: 1,000 Bps = {rate:,} Mbps"
        for rate in (1, 24000, 25000, 26000, 2)
    )
    parsed = parse_mlnx_perf(text, "rx_bytes_phy")
    assert parsed["stable_gbps"] == [24.0, 25.0, 26.0]
    assert parsed["stable_mean_gbps"] == 25.0


def test_clean_udp_evidence_is_valid(policy):
    assert verdict_from_evidence(clean_evidence(), policy) == ("valid", [])


@pytest.mark.parametrize(
    ("mutation", "reason"),
    [
        (lambda value: value["application"].update(rx_packets=999), "delivery loss"),
        (
            lambda value: value["application"].update(rx_bytes=7_992_000),
            "byte disagreement",
        ),
        (lambda value: value.update(daqiri_queue_counters={}), "DAQIRI queue"),
        (lambda value: value["kernel_udp"].update(InErrors=1), "kernel UDP"),
        (lambda value: value["kernel_udp"].update(InErrors=-1), "decreased"),
        (
            lambda value: value["nic_rx_discards"].update(
                {"link-0.rx_out_of_buffer": 1}
            ),
            "NIC receive",
        ),
        (lambda value: value.update(rdma_errors=1), "RDMA completion"),
        (
            lambda value: value["phy"]["links"]["link-0"].update(rx_packets=900),
            "PHY packet disagreement",
        ),
        (
            lambda value: value["phy"]["links"]["link-0"].update(rx_bytes=7_000_000),
            "PHY byte disagreement",
        ),
        (
            lambda value: value["achieved_rate"].update(mlnx_rx_aggregate_gbps=20),
            "below",
        ),
        (lambda value: value.update(active_duration_seconds=20), "active duration"),
    ],
)
def test_verdict_rejects_each_evidence_class(policy, mutation, reason):
    evidence = clean_evidence()
    mutation(evidence)
    state, reasons = verdict_from_evidence(evidence, policy)
    assert state == "invalid"
    assert any(reason in item for item in reasons)


def test_zero_packet_process_success_is_invalid(policy):
    evidence = clean_evidence()
    evidence["application"].update(tx_packets=0, rx_packets=0)
    state, reasons = verdict_from_evidence(evidence, policy)
    assert state == "invalid"
    assert any("zero" in reason for reason in reasons)
