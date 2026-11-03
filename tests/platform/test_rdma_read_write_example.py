# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Run the RC READ/WRITE/WRITE_IMM example on an existing RoCE address."""

import os
from pathlib import Path
import socket
import subprocess

import pytest
import yaml

pytestmark = pytest.mark.platform
ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def rdma_example():
    address = os.environ.get("DAQIRI_PLATFORM_RDMA_ADDRESS")
    executable = Path(os.environ.get(
        "DAQIRI_RDMA_READ_WRITE_EXAMPLE",
        ROOT / "build/examples/daqiri_example_rdma_read_write",
    )).resolve()
    missing = []
    if not address:
        missing.append("set DAQIRI_PLATFORM_RDMA_ADDRESS to an existing RoCE IPv4 address")
    if not executable.is_file():
        missing.append(f"build the example or set DAQIRI_RDMA_READ_WRITE_EXAMPLE: {executable}")
    if missing:
        reason = "; ".join(missing)
        if os.environ.get("DAQIRI_PLATFORM_REQUIRE_RDMA_READ_WRITE") == "1":
            pytest.fail(reason)
        pytest.skip(reason)
    return address, executable


@pytest.mark.parametrize("rx", ["direct", "indirect"])
@pytest.mark.parametrize("tx", ["direct", "indirect"])
def test_read_write_notification(rdma_example, tmp_path, rx, tx):
    address, executable = rdma_example
    cores = sorted(os.sched_getaffinity(0))
    master, worker = cores[0], cores[-1]
    template = (ROOT / "examples/daqiri_example_rdma_read_write.yaml").read_text()
    config = yaml.safe_load(template.replace("<client-ip>", address)
                            .replace("<server-ip>", address)
                            .replace("<master-cpu-core>", str(master)))
    # Select an unused service port without changing host network configuration.
    with socket.socket() as probe:
        probe.bind((address, 0))
        port = probe.getsockname()[1]
    for interface in config["daqiri"]["cfg"]["interfaces"]:
        if interface["socket_config"]["mode"] == "server":
            interface["socket_config"]["local_addr"] = f"roce://{address}:{port}"
        for direction, mode in (("rx", rx), ("tx", tx)):
            queue = interface[direction]["queues"][0]
            queue["poll_mode"] = mode
            if mode == "indirect":
                queue.update(cpu_core=worker, batch_size=1)
    path = tmp_path / "rdma-rw.yaml"
    path.write_text(yaml.safe_dump(config))
    command = [str(executable), str(path), "--mode"]
    with (tmp_path / "server.log").open("w+") as server_log:
        server = subprocess.Popen(command + ["server"], stdout=server_log,
                                  stderr=subprocess.STDOUT)
        try:
            client = subprocess.run(command + ["client"], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=50, check=False)
            server.wait(timeout=30)
            server_log.seek(0)
            output = server_log.read()
            assert client.returncode == 0, client.stdout + "\nServer:\n" + output
            assert server.returncode == 0, output
            assert "PASS: client verified WRITE -> READ" in client.stdout
            assert "PASS: server verified WRITE and WRITE_IMM" in output
            assert "offset=64 bytes=64 notification buffer unchanged" in output
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
