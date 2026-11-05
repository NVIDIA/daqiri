# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
import os

import daqiri
import pytest


@pytest.mark.bindings
def test_tx_timestamp_unsupported_engine():
    config = {
        "daqiri": {
            "cfg": {
                "version": 1,
                "stream_type": "socket",
                "master_core": min(os.sched_getaffinity(0)),
                "memory_regions": [
                    {"name": "DATA", "kind": "host", "affinity": 0,
                     "num_bufs": 8, "buf_size": 64}
                ],
                "interfaces": [{
                    "name": "udp",
                    "address": "127.0.0.1",
                    "socket_config": {
                        "mode": "client", "remote_addr": "udp://127.0.0.1:9",
                        "max_payload_size": 64,
                    },
                    "tx": {"queues": [{
                        "name": "TX", "id": 0, "cpu_core": -1,
                        "batch_size": 1, "memory_regions": ["DATA"],
                    }]},
                }],
            }
        }
    }
    assert daqiri.daqiri_init(config) == daqiri.Status.SUCCESS
    try:
        burst = daqiri.BurstParams()
        assert daqiri.request_packet_tx_timestamp(burst, 0) == (daqiri.Status.NOT_SUPPORTED, 0)
        assert daqiri.request_packet_tx_timestamp(None, 0) == (daqiri.Status.NULL_PTR, 0)
        assert daqiri.get_packet_tx_timestamp(1 << 40) == (daqiri.Status.NOT_SUPPORTED, 0)
        assert daqiri.release_tx_timestamp(1 << 40) == daqiri.Status.NOT_SUPPORTED
    finally:
        daqiri.shutdown()
