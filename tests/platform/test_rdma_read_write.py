# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Real RoCE tests. Both endpoint IPv4 addresses must be local RDMA addresses."""
import ctypes
import os
from pathlib import Path
import time

import pytest

pytestmark = pytest.mark.platform
TRANSPORT = os.environ.get("DAQIRI_PLATFORM_RDMA_TRANSPORT", "RC")


def require_operation(operation):
    if ((TRANSPORT == "UD" and operation not in ("SEND", "RECEIVE")) or
            (TRANSPORT == "UC" and operation == "RDMA_READ")):
        pytest.skip(f"{operation} is unavailable on {TRANSPORT}")


def post_receive(daqiri, receive):
    success(daqiri, daqiri.send_tx_burst(receive))
    # Unreliable transports have no RNR retry. Give an indirect RX worker time
    # to post credits before sending from the other endpoint.
    if TRANSPORT != "RC":
        time.sleep(0.02)


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
            timestamp_status, timestamp_ns = daqiri.get_packet_rx_timestamp(burst, 0)
            expected = (os.environ.get("DAQIRI_PLATFORM_RDMA_TIMESTAMPS") == "1" and
                        not burst.rdma_is_tx and burst.rdma_status == daqiri.Status.SUCCESS)
            assert timestamp_status == (daqiri.Status.SUCCESS if expected else daqiri.Status.NOT_SUPPORTED)
            assert timestamp_ns > 0 if expected else timestamp_ns == 0
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
    poll_mode = os.environ.get("DAQIRI_PLATFORM_RDMA_POLL_MODE", "indirect")
    rx_mode = os.environ.get("DAQIRI_PLATFORM_RDMA_RX_POLL_MODE", poll_mode)
    tx_mode = os.environ.get("DAQIRI_PLATFORM_RDMA_TX_POLL_MODE", poll_mode)
    accurate_send = os.environ.get("DAQIRI_PLATFORM_RDMA_ACCURATE_SEND") == "1"
    interfaces = []
    for index, (mode, ip) in enumerate((("client", client_ip), ("server", server_ip))):
        def queue(direction_mode):
            result = {"name": mode, "id": 0, "poll_mode": direction_mode,
                      "memory_regions": ["LOCAL", "TARGET"]}
            if direction_mode != "direct":
                result.update(batch_size=4, cpu_core=cpus[index + 1])
            return result
        interfaces.append({"name": mode, "address": ip,
                           "socket_config": {"mode": mode, "local_addr": f"roce://{ip}:{port}"},
                           "roce_config": {"transport_mode": TRANSPORT},
                           "rx": {"queues": [queue(rx_mode)], "hardware_timestamps":
                                  os.environ.get("DAQIRI_PLATFORM_RDMA_TIMESTAMPS") == "1"},
                           "tx": {"queues": [queue(tx_mode)], "accurate_send": accurate_send}})
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
    require_operation(operation)
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
            deadline = time.monotonic() + 5
            while True:
                actual = ctypes.string_at(target_address + 16, total)
                if actual == expected or time.monotonic() >= deadline:
                    break
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
    receive_address = daqiri.get_packet_ptr(receive, 0)
    ctypes.memset(receive_address, 0xCC, 4096 + 40)
    send = allocate(daqiri, client, False, daqiri.RDMAOpCode.SEND, [4096], 7, "LOCAL")
    expected = bytes(index % 251 for index in range(4096))
    ctypes.memmove(daqiri.get_packet_ptr(send, 0), expected, len(expected))
    post_receive(daqiri, receive)
    success(daqiri, daqiri.send_tx_burst(send))
    sent = completion(daqiri, client, False)
    received = completion(daqiri, server, True)
    assert sent.rdma_status == received.rdma_status == daqiri.Status.SUCCESS
    assert daqiri.rdma_get_opcode(sent) == daqiri.RDMAOpCode.SEND
    assert daqiri.rdma_get_opcode(received) == daqiri.RDMAOpCode.RECEIVE
    assert ctypes.string_at(daqiri.get_packet_ptr(received, 0), 4096) == expected
    assert daqiri.get_packet_length(received, 0) == 4096
    assert ctypes.string_at(receive_address + 4096, 40) == b"\xCC" * 40
    daqiri.free_tx_burst(sent)
    daqiri.free_tx_burst(received)


@pytest.mark.parametrize("server_request", [False, True])
@pytest.mark.parametrize("lengths", [[0], [4096], [0, 1, 64, 257]])
@pytest.mark.parametrize("immediate", [0, 0x12345678, 0xFFFFFFFF])
def test_write_with_immediate(endpoints, server_request, lengths, immediate):
    require_operation("RDMA_WRITE_IMM")
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
    post_receive(daqiri, receive)
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
        status, value, written = daqiri.rdma_get_immediate_data(received, with_length=True)
        success(daqiri, status)
        assert value == immediate and written == lengths[index]
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
    require_operation("RDMA_WRITE")
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
    post_receive(daqiri, receive)
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
    if TRANSPORT != "RC":
        pytest.skip("RNR wait/retry is an RC guarantee")
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
    post_receive(daqiri, receive)
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
    post_receive(daqiri, receive)
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


@pytest.mark.parametrize("server_request", [False, True])
@pytest.mark.parametrize("operation,length", [("SEND", 64), ("RDMA_READ", 64), ("RDMA_WRITE", 64),
                                             ("RDMA_WRITE_IMM", 64), ("RDMA_WRITE_IMM", 0)])
def test_scheduled_one_sided(endpoints, server_request, operation, length):
    require_operation(operation)
    if os.environ.get("DAQIRI_PLATFORM_RDMA_ACCURATE_SEND") != "1":
        pytest.skip("requires accurate TX scheduling")
    if os.environ.get("DAQIRI_PLATFORM_RDMA_TIMESTAMPS") != "1":
        pytest.skip("requires RX timestamps for hardware timing verification")
    daqiri, client, server = endpoints
    conn, peer = (server, client) if server_request else (client, server)
    opcode = getattr(daqiri.RDMAOpCode, operation)
    status, remote = daqiri.rdma_get_memory_region(peer, "TARGET")
    success(daqiri, status)
    target = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.SEND, [128], 0, "TARGET")
    address = daqiri.get_packet_ptr(target, 0)
    expected = bytes(range(length))
    ctypes.memset(address, 0xCC, 128)
    if operation == "RDMA_READ":
        ctypes.memmove(address + 16, expected, length)
    request = allocate(daqiri, conn, server_request, opcode, [length], 12000, "LOCAL")
    if length and operation != "RDMA_READ":
        ctypes.memmove(daqiri.get_packet_ptr(request, 0), expected, length)
    if operation != "SEND":
        success(daqiri, daqiri.rdma_set_remote_memory(request, remote, address + 16 - remote.address))
    notify = None
    if operation == "RDMA_WRITE_IMM":
        notify = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE, [0], 12001, "LOCAL")
    elif operation == "SEND":
        notify = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE, [length], 12001, "LOCAL")
    # A baseline SEND samples the same NIC clock used by WAIT and RX CQEs.
    # On unsynchronized qualification hosts, test the 8-second cyclic hardware
    # comparison using an equivalent host-window target. This does not qualify
    # PTP alignment or absolute epoch scheduling accuracy.
    receive = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE, [1], 12002, "LOCAL")
    send = allocate(daqiri, conn, server_request, daqiri.RDMAOpCode.SEND, [1], 12003, "LOCAL")
    post_receive(daqiri, receive)
    success(daqiri, daqiri.send_tx_burst(send))
    baseline = completion(daqiri, peer, not server_request)
    sent = completion(daqiri, conn, server_request)
    status, nic_time = daqiri.get_packet_rx_timestamp(baseline, 0)
    success(daqiri, status)
    observed = time.monotonic_ns()
    daqiri.free_tx_burst(baseline)
    daqiri.free_tx_burst(sent)
    if notify is not None:
        post_receive(daqiri, notify)
    target_nic = nic_time + 100_000_000
    host_now = time.time_ns()
    cycle = 8_000_000_000
    target_host = host_now + (target_nic - host_now + cycle // 2) % cycle - cycle // 2
    success(daqiri, daqiri.set_packet_tx_time(request, 0, target_host))
    submitted = time.monotonic_ns()
    success(daqiri, daqiri.send_tx_burst(request))
    done = completion(daqiri, conn, server_request)
    finished = time.monotonic_ns()
    assert done.rdma_status == daqiri.Status.SUCCESS and done.rdma_wr_id == 12000
    assert daqiri.rdma_get_opcode(done) == opcode
    assert submitted - observed < 50_000_000, "test missed its scheduling lead time"
    assert finished - observed >= 90_000_000, "scheduled work completed before its NIC target"
    assert finished - observed < 1_000_000_000, "scheduled work missed the cyclic target"
    if operation == "RDMA_READ":
        assert ctypes.string_at(daqiri.get_packet_ptr(done, 0), length) == expected
    elif operation != "SEND":
        assert ctypes.string_at(address + 16, length) == expected
    assert ctypes.string_at(address, 16) == b"\xCC" * 16
    assert ctypes.string_at(address + 16 + length, 16) == b"\xCC" * 16
    if notify is not None:
        notification = completion(daqiri, peer, not server_request)
        assert daqiri.rdma_get_opcode(notification) == (daqiri.RDMAOpCode.RECEIVE if operation == "SEND"
                                                       else daqiri.RDMAOpCode.RDMA_WRITE_IMM)
        if operation == "SEND":
            assert daqiri.get_packet_length(notification, 0) == length
            assert ctypes.string_at(daqiri.get_packet_ptr(notification, 0), length) == expected
        status, arrived = daqiri.get_packet_rx_timestamp(notification, 0)
        success(daqiri, status)
        assert 0 <= arrived - target_nic < 100_000_000
        daqiri.free_tx_burst(notification)
    daqiri.free_tx_burst(done)
    daqiri.free_tx_burst(target)


@pytest.mark.parametrize("server_request", [False, True])
@pytest.mark.parametrize("operation", ["RDMA_READ", "RDMA_WRITE", "RDMA_WRITE_IMM"])
def test_transport_rejects_unsupported(endpoints, server_request, operation):
    if TRANSPORT == "RC" or (TRANSPORT == "UC" and operation != "RDMA_READ"):
        pytest.skip("operation is supported")
    daqiri, client, server = endpoints
    conn = server if server_request else client
    request = allocate(daqiri, conn, server_request, getattr(daqiri.RDMAOpCode, operation),
                       [64], 50000, "LOCAL")
    assert daqiri.send_tx_burst(request) == daqiri.Status.NOT_SUPPORTED
    tx_mode = os.environ.get("DAQIRI_PLATFORM_RDMA_TX_POLL_MODE",
                             os.environ.get("DAQIRI_PLATFORM_RDMA_POLL_MODE", "indirect"))
    if tx_mode == "direct":
        daqiri.free_tx_burst(request)
    status, extra = daqiri.get_rx_burst_for_connection(conn, server_request)
    assert status == daqiri.Status.NOT_READY and extra is None


@pytest.mark.parametrize("server_request", [False, True])
def test_uc_notification_without_receive_credit(endpoints, server_request):
    if TRANSPORT != "UC":
        pytest.skip("UC receive-credit underrun semantics")
    daqiri, client, server = endpoints
    conn, peer = (server, client) if server_request else (client, server)
    status, remote = daqiri.rdma_get_memory_region(peer, "TARGET")
    success(daqiri, status)
    target = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.SEND, [64], 0, "TARGET")
    address = daqiri.get_packet_ptr(target, 0)
    request = allocate(daqiri, conn, server_request, daqiri.RDMAOpCode.RDMA_WRITE_IMM,
                       [64], 51000, "LOCAL")
    ctypes.memset(daqiri.get_packet_ptr(request, 0), 0x5A, 64)
    success(daqiri, daqiri.rdma_set_remote_memory(request, remote, address - remote.address))
    success(daqiri, daqiri.send_tx_burst(request))
    done = completion(daqiri, conn, server_request)
    assert done.rdma_status == daqiri.Status.SUCCESS
    # Posting a credit later must not resurrect the dropped notification.
    receive = allocate(daqiri, peer, not server_request, daqiri.RDMAOpCode.RECEIVE,
                       [0], 51001, "LOCAL")
    post_receive(daqiri, receive)
    status, extra = daqiri.get_rx_burst_for_connection(peer, not server_request)
    assert status == daqiri.Status.NOT_READY and extra is None
    success(daqiri, daqiri.rdma_set_header(done, daqiri.RDMAOpCode.RDMA_WRITE_IMM,
                                        conn, server_request, 1, 51002, "LOCAL", immediate_data=222))
    success(daqiri, daqiri.set_packet_lengths(done, 0, [64]))
    success(daqiri, daqiri.rdma_set_remote_memory(done, remote, address - remote.address))
    success(daqiri, daqiri.send_tx_burst(done))
    sent = completion(daqiri, conn, server_request)
    notification = completion(daqiri, peer, not server_request)
    assert sent.rdma_status == notification.rdma_status == daqiri.Status.SUCCESS
    status, immediate = daqiri.rdma_get_immediate_data(notification)
    success(daqiri, status)
    assert immediate == 222 and ctypes.string_at(address, 64) == b"\x5A" * 64
    daqiri.free_tx_burst(notification)
    daqiri.free_tx_burst(sent)
    daqiri.free_tx_burst(target)


def test_ud_rejects_oversize_send(endpoints):
    if TRANSPORT != "UD":
        pytest.skip("UD MTU limit")
    daqiri, client, _ = endpoints
    request = allocate(daqiri, client, False, daqiri.RDMAOpCode.SEND, [4097], 50001, "LOCAL")
    assert daqiri.send_tx_burst(request) == daqiri.Status.INVALID_PARAMETER
    tx_mode = os.environ.get("DAQIRI_PLATFORM_RDMA_TX_POLL_MODE",
                             os.environ.get("DAQIRI_PLATFORM_RDMA_POLL_MODE", "indirect"))
    if tx_mode == "direct":
        daqiri.free_tx_burst(request)


def test_invalid_rkey_completion(endpoints):
    require_operation("RDMA_READ")
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
