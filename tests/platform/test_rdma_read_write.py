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


@pytest.mark.parametrize("server_request", [False, True])
@pytest.mark.parametrize("lengths", [[0], [4096], [0, 1, 64, 257]])
@pytest.mark.parametrize("immediate", [0, 0x12345678, 0xFFFFFFFF])
def test_write_with_immediate(endpoints, server_request, lengths, immediate):
    daqiri, client, server = endpoints
    conn, peer = (server, client) if server_request else (client, server)
    status, remote = daqiri.rdma_get_memory_region(peer, "TARGET")
    success(daqiri, status)
    target = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.SEND,
                      [32768], 0, "TARGET")
    target_address = daqiri.get_packet_ptr(target, 0)
    total = sum(lengths)
    ctypes.memset(target_address, 0xCC, total + 32)
    # A batched RECEIVE must yield independently owned notification completions.
    receive = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE,
                       [0] * len(lengths), 6000, "LOCAL")
    success(daqiri, daqiri.send_tx_burst(receive))
    request = daqiri.create_tx_burst_params()
    assert request is not None
    success(daqiri, daqiri.rdma_set_header(request, daqiri.RDMAOpCode.RDMA_WRITE_IMM,
                                        conn, server_request, len(lengths), 5000, "LOCAL",
                                        immediate_data=immediate))
    success(daqiri, daqiri.get_tx_packet_burst(request))
    expected = bytes(index % 251 for index in range(total))
    offset = 0
    for index, length in enumerate(lengths):
        success(daqiri, daqiri.set_packet_lengths(request, index, [length]))
        if length:
            ctypes.memmove(daqiri.get_packet_ptr(request, index),
                           expected[offset:offset + length], length)
        offset += length
    success(daqiri, daqiri.rdma_set_remote_memory(request, remote,
                                               target_address + 16 - remote.address))
    success(daqiri, daqiri.send_tx_burst(request))
    for index in range(len(lengths)):
        received = completion(daqiri, peer, not server_request)
        assert received.rdma_status == daqiri.Status.SUCCESS
        assert not received.rdma_is_tx and received.rdma_wr_id == 6000 + index
        assert daqiri.rdma_get_opcode(received) == daqiri.RDMAOpCode.RDMA_WRITE_IMM
        status, value = daqiri.rdma_get_immediate_data(received)
        success(daqiri, status)
        assert value == immediate
        assert daqiri.get_packet_length(received, 0) == 0
        # Returning one completion must not release another receive credit's packet.
        daqiri.free_tx_burst(received)
    sent = completion(daqiri, conn, server_request)
    assert sent.rdma_status == daqiri.Status.SUCCESS and sent.rdma_wr_id == 5000
    assert daqiri.rdma_get_opcode(sent) == daqiri.RDMAOpCode.RDMA_WRITE_IMM
    status, value = daqiri.rdma_get_immediate_data(sent)
    success(daqiri, status)
    assert value == immediate
    assert ctypes.string_at(target_address + 16, total) == expected
    assert ctypes.string_at(target_address, 16) == b"\xCC" * 16
    assert ctypes.string_at(target_address + 16 + total, 16) == b"\xCC" * 16
    for connection, role in ((conn, server_request), (peer, not server_request)):
        status, extra = daqiri.get_rx_burst_for_connection(connection, role)
        assert status == daqiri.Status.NOT_READY and extra is None
    daqiri.free_tx_burst(sent)
    daqiri.free_tx_burst(target)


@pytest.mark.parametrize("server_request", [False, True])
def test_separate_write_then_notification(endpoints, server_request):
    daqiri, client, server = endpoints
    conn, peer = (server, client) if server_request else (client, server)
    status, remote = daqiri.rdma_get_memory_region(peer, "TARGET")
    success(daqiri, status)
    target = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.SEND,
                      [32768], 0, "TARGET")
    address = daqiri.get_packet_ptr(target, 0)
    expected = bytes(index % 239 for index in range(4096))
    receive = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE,
                       [0], 9000, "LOCAL")
    success(daqiri, daqiri.send_tx_burst(receive))
    write = allocate(daqiri, conn, server_request, daqiri.RDMAOpCode.RDMA_WRITE,
                     [4096], 8000, "LOCAL")
    ctypes.memmove(daqiri.get_packet_ptr(write, 0), expected, 4096)
    success(daqiri, daqiri.rdma_set_remote_memory(write, remote, address - remote.address))
    notify = daqiri.create_tx_burst_params()
    assert notify is not None
    success(daqiri, daqiri.rdma_set_header(notify, daqiri.RDMAOpCode.RDMA_WRITE_IMM,
                                        conn, server_request, 1, 8001, "LOCAL", 0xABCD1234))
    success(daqiri, daqiri.get_tx_packet_burst(notify))
    success(daqiri, daqiri.set_packet_lengths(notify, 0, [0]))
    success(daqiri, daqiri.rdma_set_remote_memory(notify, remote, address - remote.address))
    # Post both on the same QP without waiting for the plain write's completion.
    success(daqiri, daqiri.send_tx_burst(write))
    success(daqiri, daqiri.send_tx_burst(notify))
    received = completion(daqiri, peer, not server_request)
    assert received.rdma_status == daqiri.Status.SUCCESS
    status, immediate = daqiri.rdma_get_immediate_data(received)
    success(daqiri, status)
    assert immediate == 0xABCD1234
    assert ctypes.string_at(address, 4096) == expected
    daqiri.free_tx_burst(received)
    for wr_id in (8000, 8001):
        sent = completion(daqiri, conn, server_request)
        assert sent.rdma_status == daqiri.Status.SUCCESS and sent.rdma_wr_id == wr_id
        daqiri.free_tx_burst(sent)
    daqiri.free_tx_burst(target)


@pytest.mark.parametrize("server_request", [False, True])
def test_notification_waits_for_receive_credit(endpoints, server_request):
    daqiri, client, server = endpoints
    conn, peer = (server, client) if server_request else (client, server)
    status, remote = daqiri.rdma_get_memory_region(peer, "TARGET")
    success(daqiri, status)
    notify = daqiri.create_tx_burst_params()
    assert notify is not None
    success(daqiri, daqiri.rdma_set_header(notify, daqiri.RDMAOpCode.RDMA_WRITE_IMM,
                                        conn, server_request, 1, 9100, "LOCAL", 0xFF000001))
    success(daqiri, daqiri.get_tx_packet_burst(notify))
    success(daqiri, daqiri.set_packet_lengths(notify, 0, [0]))
    success(daqiri, daqiri.rdma_set_remote_memory(notify, remote))
    success(daqiri, daqiri.send_tx_burst(notify))
    time.sleep(0.02)
    for connection, role in ((conn, server_request), (peer, not server_request)):
        status, extra = daqiri.get_rx_burst_for_connection(connection, role)
        assert status == daqiri.Status.NOT_READY and extra is None
    receive = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE,
                       [0], 9200, "LOCAL")
    success(daqiri, daqiri.send_tx_burst(receive))
    received = completion(daqiri, peer, not server_request)
    assert received.rdma_status == daqiri.Status.SUCCESS
    status, immediate = daqiri.rdma_get_immediate_data(received)
    success(daqiri, status)
    assert immediate == 0xFF000001
    daqiri.free_tx_burst(received)
    sent = completion(daqiri, conn, server_request)
    assert sent.rdma_status == daqiri.Status.SUCCESS and sent.rdma_wr_id == 9100
    daqiri.free_tx_burst(sent)


@pytest.mark.parametrize("server_request", [False, True])
def test_batched_send_receive(endpoints, server_request):
    daqiri, client, server = endpoints
    conn, peer = (server, client) if server_request else (client, server)
    lengths = [1, 64, 257, 4096]
    receive = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE,
                       [32768] * len(lengths), 10000, "TARGET")
    send = allocate(daqiri, conn, server_request, daqiri.RDMAOpCode.SEND,
                    lengths, 11000, "LOCAL")
    expected = [bytes((byte + index) % 251 for byte in range(length))
                for index, length in enumerate(lengths)]
    for index, data in enumerate(expected):
        ctypes.memmove(daqiri.get_packet_ptr(send, index), data, len(data))
    success(daqiri, daqiri.send_tx_burst(receive))
    success(daqiri, daqiri.send_tx_burst(send))
    for index, data in enumerate(expected):
        received = completion(daqiri, peer, not server_request)
        assert received.rdma_status == daqiri.Status.SUCCESS
        assert received.rdma_wr_id == 10000 + index and not received.rdma_is_tx
        assert daqiri.rdma_get_opcode(received) == daqiri.RDMAOpCode.RECEIVE
        assert daqiri.get_packet_length(received, 0) == len(data)
        assert ctypes.string_at(daqiri.get_packet_ptr(received, 0), len(data)) == data
        status, _ = daqiri.rdma_get_immediate_data(received)
        assert status == daqiri.Status.NOT_SUPPORTED
        daqiri.free_tx_burst(received)
    sent = completion(daqiri, conn, server_request)
    assert sent.rdma_status == daqiri.Status.SUCCESS and sent.rdma_wr_id == 11000
    assert daqiri.rdma_get_opcode(sent) == daqiri.RDMAOpCode.SEND
    daqiri.free_tx_burst(sent)


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
