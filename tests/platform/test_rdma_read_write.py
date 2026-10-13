# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Real RC read/write tests. Both endpoint IPv4 addresses must be local RDMA addresses."""
import ctypes
import os
from pathlib import Path
import time

import pytest

pytestmark = pytest.mark.platform


def unavailable(reason):
    if os.environ.get("DAQIRI_PLATFORM_REQUIRE_RDMA_READ_WRITE") == "1":
        pytest.fail(reason)
    pytest.skip(reason)


def success(daqiri, status):
    assert status == daqiri.Status.SUCCESS, str(status)


def allocate(daqiri, conn, server, opcode, lengths, wr_id, name):
    burst = daqiri.create_tx_burst_params()
    assert burst is not None
    success(daqiri, daqiri.rdma_set_header(burst, opcode, conn, server,
                                        len(lengths), wr_id, name))
    success(daqiri, daqiri.get_tx_packet_burst(burst))
    for index, length in enumerate(lengths):
        success(daqiri, daqiri.set_packet_lengths(burst, index, [length]))
    return burst


def completion(daqiri, conn, server):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        status, burst = daqiri.get_rx_burst_for_connection(conn, server)
        if status == daqiri.Status.SUCCESS:
            return burst
        assert status == daqiri.Status.NOT_READY, str(status)
        time.sleep(0.0001)
    pytest.fail("Timed out waiting for a real RDMA completion")


@pytest.fixture(scope="module")
def endpoints():
    client_ip = os.environ.get("DAQIRI_PLATFORM_RDMA_CLIENT_IP")
    server_ip = os.environ.get("DAQIRI_PLATFORM_RDMA_SERVER_IP")
    if not client_ip or not server_ip:
        unavailable("set DAQIRI_PLATFORM_RDMA_CLIENT_IP and DAQIRI_PLATFORM_RDMA_SERVER_IP")
    if not Path("/dev/infiniband/rdma_cm").exists():
        unavailable("RDMA CM device is unavailable")
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 3:
        unavailable("RDMA test requires three allowed CPU cores")
    try:
        import daqiri
    except ImportError as error:
        unavailable(f"DAQIRI Python bindings unavailable: {error}")
    port = int(os.environ.get("DAQIRI_PLATFORM_RDMA_PORT", "19485"))
    interfaces = []
    for index, (mode, ip) in enumerate((("client", client_ip), ("server", server_ip))):
        queue = {"name": mode, "id": 0, "batch_size": 4,
                 "cpu_core": cpus[index + 1], "memory_regions": ["LOCAL", "TARGET"]}
        interfaces.append({"name": mode, "address": ip,
                           "socket_config": {"mode": mode, "local_addr": f"roce://{ip}:{port}"},
                           "roce_config": {"transport_mode": "RC"},
                           "rx": {"queues": [queue]}, "tx": {"queues": [queue]}})
    config = {"daqiri": {"cfg": {
        "version": 1, "stream_type": "socket", "master_core": cpus[0], "log_level": "info",
        "memory_regions": [
            {"name": name, "kind": "host", "affinity": 0, "num_bufs": 16, "buf_size": 32768,
             "access": ["local", "rdma_read", "rdma_write"]}
            for name in ("LOCAL", "TARGET")], "interfaces": interfaces}}}
    success(daqiri, daqiri.daqiri_init(config))
    try:
        # The listener is started by the engine's CM thread.
        time.sleep(0.1)
        status, client = daqiri.rdma_connect_to_server(server_ip, port, client_ip)
        success(daqiri, status)
        deadline = time.monotonic() + 10
        while True:
            status, server = daqiri.rdma_get_server_conn_id(server_ip, port)
            if status == daqiri.Status.SUCCESS:
                break
            assert time.monotonic() < deadline, "Server connection did not become ready"
            time.sleep(0.001)
        yield daqiri, client, server
    finally:
        daqiri.shutdown()


@pytest.mark.parametrize("server_request", [False, True])
@pytest.mark.parametrize("operation", ["RDMA_READ", "RDMA_WRITE"])
@pytest.mark.parametrize("lengths", [[4096], [1, 64, 257, 4096]])
def test_rdma_byte_integrity(endpoints, server_request, operation, lengths):
    daqiri, client, server = endpoints
    conn, peer = (server, client) if server_request else (client, server)
    opcode = getattr(daqiri.RDMAOpCode, operation)
    status, remote = daqiri.rdma_get_memory_region(peer, "TARGET")
    success(daqiri, status)
    iterations = int(os.environ.get("DAQIRI_PLATFORM_RDMA_ITERATIONS", "100"))
    assert iterations > 0
    for iteration in range(iterations):
        # Reserve the peer's target from its pool for the duration of the transfer.
        target = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.SEND,
                          [32768], 0, "TARGET")
        target_address = daqiri.get_packet_ptr(target, 0)
        total = sum(lengths)
        # Descending/reused caller IDs must not influence SQ completion ownership.
        wr_id = iterations - iteration
        request = allocate(daqiri, conn, server_request, opcode, lengths, wr_id, "LOCAL")
        pointers = [daqiri.get_packet_ptr(request, index) for index in range(len(lengths))]
        expected = bytes((index * 17 + iteration * 23) % 256 for index in range(total))
        ctypes.memset(target_address, 0xCC, total + 32)
        if operation == "RDMA_READ":
            ctypes.memmove(target_address + 16, expected, total)
        offset = 0
        for address, length in zip(pointers, lengths):
            if operation == "RDMA_WRITE":
                ctypes.memmove(address, expected[offset:offset + length], length)
            else:
                ctypes.memset(address, 0, length)
            offset += length
        success(daqiri, daqiri.rdma_set_remote_memory(request, remote,
                                                   target_address + 16 - remote.address))
        success(daqiri, daqiri.send_tx_burst(request))
        done = completion(daqiri, conn, server_request)
        assert done.rdma_status == daqiri.Status.SUCCESS
        assert done.rdma_is_tx and done.rdma_wr_id == wr_id
        assert daqiri.rdma_get_opcode(done) == opcode
        assert [daqiri.get_packet_ptr(done, index) for index in range(len(lengths))] == pointers
        if operation == "RDMA_READ":
            actual = b"".join(ctypes.string_at(address, length)
                              for address, length in zip(pointers, lengths))
        else:
            actual = ctypes.string_at(target_address + 16, total)
        assert actual == expected
        assert ctypes.string_at(target_address, 16) == b"\xCC" * 16
        assert ctypes.string_at(target_address + 16 + total, 16) == b"\xCC" * 16
        status, extra = daqiri.get_rx_burst_for_connection(conn, server_request)
        assert status == daqiri.Status.NOT_READY and extra is None
        daqiri.free_tx_burst(done)
        daqiri.free_tx_burst(target)


def test_send_receive_after_rdma(endpoints):
    daqiri, client, server = endpoints
    receive = allocate(daqiri, server, True, daqiri.RDMAOpCode.RECEIVE, [4096], 8, "TARGET")
    send = allocate(daqiri, client, False, daqiri.RDMAOpCode.SEND, [4096], 7, "LOCAL")
    expected = bytes(index % 251 for index in range(4096))
    ctypes.memmove(daqiri.get_packet_ptr(send, 0), expected, len(expected))
    success(daqiri, daqiri.send_tx_burst(receive))
    success(daqiri, daqiri.send_tx_burst(send))
    sent = completion(daqiri, client, False)
    received = completion(daqiri, server, True)
    assert sent.rdma_status == received.rdma_status == daqiri.Status.SUCCESS
    assert daqiri.rdma_get_opcode(sent) == daqiri.RDMAOpCode.SEND
    assert daqiri.rdma_get_opcode(received) == daqiri.RDMAOpCode.RECEIVE
    assert ctypes.string_at(daqiri.get_packet_ptr(received, 0), 4096) == expected
    daqiri.free_tx_burst(sent)
    daqiri.free_tx_burst(received)


def test_invalid_rkey_completion(endpoints):
    # Last: a remote access violation moves this connection's QP into error.
    daqiri, client, server = endpoints
    status, remote = daqiri.rdma_get_memory_region(server, "TARGET")
    success(daqiri, status)
    remote.rkey ^= 1
    request = allocate(daqiri, client, False, daqiri.RDMAOpCode.RDMA_READ, [64], 123, "LOCAL")
    success(daqiri, daqiri.rdma_set_remote_memory(request, remote))
    success(daqiri, daqiri.send_tx_burst(request))
    done = completion(daqiri, client, False)
    assert done.rdma_status == daqiri.Status.GENERIC_FAILURE
    assert done.rdma_wr_id == 123 and done.rdma_is_tx
    assert daqiri.rdma_get_opcode(done) == daqiri.RDMAOpCode.RDMA_READ
    daqiri.free_tx_burst(done)
