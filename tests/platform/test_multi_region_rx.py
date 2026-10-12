# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0

"""Hardware-loopback byte-integrity coverage for raw ibverbs multi-region RX."""

import os
from pathlib import Path
import shlex
import subprocess

import pytest


def unavailable(message):
    if os.environ.get("DAQIRI_PLATFORM_REQUIRE_MULTI_REGION_RX") == "1":
        pytest.fail(message)
    pytest.skip(message)


@pytest.fixture(scope="module")
def reproducer(tmp_path_factory):
    bdf = os.environ.get("DAQIRI_PLATFORM_IBVERBS_BDF")
    if not bdf:
        unavailable("set DAQIRI_PLATFORM_IBVERBS_BDF to an mlx5 PCI BDF")
    env = os.environ.copy()
    if not env.get("DAQIRI_PLATFORM_RX_MAC"):
        addresses = sorted(Path(f"/sys/bus/pci/devices/{bdf}/net").glob("*/address"))
        if not addresses:
            unavailable("set DAQIRI_PLATFORM_RX_MAC; no netdev MAC found")
        env["DAQIRI_PLATFORM_RX_MAC"] = addresses[0].read_text().strip()
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 3:
        unavailable("multi-region RX test requires three allowed CPUs")
    for name, cpu in zip(("MASTER", "TX", "RX"), cpus[:3]):
        env.setdefault(f"DAQIRI_PLATFORM_{name}_CPU", str(cpu))
    executable = env.get("DAQIRI_MULTI_REGION_RX_EXECUTABLE")
    if executable:
        binary = Path(executable)
        if not binary.is_file() or not os.access(binary, os.X_OK):
            unavailable(f"multi-region RX executable unavailable: {binary}")
        return binary, env
    binary = tmp_path_factory.mktemp("multi-region-rx") / "reproducer"
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "daqiri"], text=True)
    cuda = Path(env.get("CUDA_HOME", "/usr/local/cuda"))
    command = [env.get("CXX", "c++"), "-std=c++17", "-O2", "-pthread",
               str(Path(__file__).with_name("multi_region_rx.cpp")), "-o", str(binary),
               *shlex.split(flags), f"-I{cuda / 'include'}", f"-L{cuda / 'lib64'}", "-lcudart"]
    built = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
    assert built.returncode == 0, built.stdout + built.stderr
    return binary, env


@pytest.mark.platform
@pytest.mark.parametrize("regions,frame_size,mode,striding", [
    (2, 5142, "indirect", None),
    (3, 5142, "indirect", None),
    (4, 5142, "indirect", None),
    (3, 1100, "indirect", None),
    (4, 1100, "indirect", None),
    (3, 100, "indirect", None),
    (3, 5142, "direct", None),
    (4, 5142, "direct", None),
    (3, 5142, "indirect", "1"),
    (5, 5142, "indirect", None),
])
def test_multi_region_rx(reproducer, regions, frame_size, mode, striding):
    binary, environment = reproducer
    env = environment.copy()
    env.pop("DAQIRI_IBV_STRIDING", None)
    if striding is not None:
        env["DAQIRI_IBV_STRIDING"] = striding
    result = subprocess.run([str(binary), str(regions), str(frame_size), mode],
                            env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    expected = "oversized region list rejected" if regions == 5 else "exact CPU/GPU byte integrity"
    assert f"PASS: {expected}" in result.stdout
    if regions == 5:
        assert "5 memory regions; at most 4 are supported" in result.stderr
