# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
import daqiri
import pytest


@pytest.mark.bindings
def test_ibverbs_engine_type():
    engine_type = daqiri.EngineType.IBVERBS
    assert engine_type.name == "IBVERBS"
    assert engine_type.value == 4
    assert daqiri.EngineType(4) == engine_type
    assert daqiri.engine_type_to_string(engine_type) == "ibverbs"


@pytest.mark.bindings
@pytest.mark.parametrize("engine_override", ["", "engine: ibverbs"], ids=["default", "explicit"])
def test_raw_ibverbs_config_engine_type(engine_override):
    try:
        daqiri.engine_type_from_string("ibverbs")
    except ValueError:
        pytest.skip("Requires a build with DAQIRI_ENGINE including ibverbs")

    config = f"""\
daqiri:
  cfg:
    version: 1
    stream_type: raw
    {engine_override}
    master_core: 0
    memory_regions:
      - {{name: RX, kind: host, affinity: 0, num_bufs: 64, buf_size: 2048}}
    interfaces:
      - name: rx_port
        address: "0000:01:00.0"
        rx:
          queues:
            - {{name: RX, id: 0, cpu_core: 1, batch_size: 16, memory_regions: [RX]}}
"""
    status, parsed = daqiri.parse_network_config_from_yaml_string(config)

    assert status == daqiri.Status.SUCCESS
    assert parsed.common.engine_type == daqiri.EngineType.IBVERBS
    assert parsed.common.engine_type.name == "IBVERBS"
    assert daqiri.engine_type_to_string(parsed.common.engine_type) == "ibverbs"
