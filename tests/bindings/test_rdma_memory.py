# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
import pytest
import daqiri


@pytest.mark.bindings
def test_rdma_memory_descriptor():
    region = daqiri.RDMAMemoryRegion()
    assert (region.address, region.length, region.rkey) == (0, 0, 0)
    region.address = 0x100000000
    region.length = 8192
    region.rkey = 0xDEADBEEF
    assert (region.address, region.length, region.rkey) == (0x100000000, 8192, 0xDEADBEEF)
    burst = daqiri.BurstParams()
    burst.rdma_wr_id = 42
    assert burst.rdma_wr_id == 42
    assert burst.rdma_status == daqiri.Status.SUCCESS
    assert burst.rdma_is_tx is False
    assert callable(daqiri.rdma_get_memory_region)
    assert callable(daqiri.rdma_set_remote_memory)

    assert callable(daqiri.rdma_get_immediate_data)
