#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import functools
import html
import logging
import os
import re
import shlex
import subprocess
import sys
import textwrap
from collections import Counter, defaultdict
from ctypes import CDLL, byref, c_int, create_string_buffer
from dataclasses import dataclass, field


PCI_SYSFS_DEVICES = "/sys/bus/pci/devices"
PCI_BDF_RE = re.compile(
    r"^([0-9a-fA-F]{4,8}):([0-9a-fA-F]{2}):([0-9a-fA-F]{2})\.([0-7])$"
)
PCI_ROOT_RE = re.compile(r"^pci[0-9a-fA-F]{4}:[0-9a-fA-F]{2}$")
OPTIMAL_TOPOLOGY_CONNECTIONS = {"PIX", "PXB"}
PCIE_EFFECTIVE_GB_PER_SEC_PER_LANE = {
    2.5: 0.250,
    5.0: 0.500,
    8.0: 0.985,
    16.0: 1.969,
    32.0: 3.938,
    64.0: 7.563,
}


@dataclass
class PciDevice:
    bdf: str
    class_code: int
    vendor_id: str
    class_name: str
    description: str
    kind: str
    numa_node: int
    root: str
    parent_bdf: str
    link_speed: str = ""
    link_width: str = ""
    max_link_speed: str = ""
    max_link_width: str = ""
    path_chain: list = field(default_factory=list)
    label: str = ""
    net_names: list = field(default_factory=list)
    rdma_names: list = field(default_factory=list)
    block_names: list = field(default_factory=list)
    notes: list = field(default_factory=list)


@dataclass
class SchematicNode:
    key: str
    lines: list
    kind: str
    depth: int
    link_label: str = ""
    children: list = field(default_factory=list)
    render_lines: list = field(default_factory=list)
    x: int = 0
    y: int = 0
    width: int = 0
    height: int = 0


def setup_logging():
    """
    Configures the logging settings.
    """
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(levelname)s - %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


def parse_args():
    """
    Parses command-line arguments.
    """
    parser = argparse.ArgumentParser(
        description="Check system tuning for DAQIRI performance",
        epilog=(
            "Examples:\n"
            f"  python {sys.argv[0]} --check cpu-freq    # Check CPU frequency governor\n"
            f"  python {sys.argv[0]} --check mrrs        # Check MRRS settings for NVIDIA NICs\n"
            f"  python {sys.argv[0]} --check mps         # Check max payload size settings\n"
            f"  python {sys.argv[0]} --check schematic   # Write PCIe topology image\n"
            f"  python {sys.argv[0]} --check schematic --schematic-output topo.svg\n"
            f"  python {sys.argv[0]} --set mrrs          # Set PCIe MRRS\n\n"
        ),
        formatter_class=argparse.RawTextHelpFormatter,
    )

    group = parser.add_mutually_exclusive_group(required=True)

    group.add_argument(
        "--check",
        choices=[
            "all",
            "gpudirect",
            "peermem",
            "cpu-freq",
            "mrrs",
            "mps",
            "hugepages",
            "gpu-clocks",
            "bar1-size",
            "topo",
            "schematic",
            "cmdline",
            "mtu",
        ],
        help=(
            "Specify the property to check:\n"
            "  all        - Perform all checks\n"
            "  gpudirect  - Check if NVIDIA GPUs support GPUDirect.\n"
            "  peermem    - Check if the nvidia-peermem module is loaded.\n"
            "  cpu-freq   - Check if the CPU frequency governor is set to 'performance'.\n"
            "  mrrs       - Check if the Maximum Read Request Size (MRRS) of NVIDIA NICs is set to 4096.\n"
            "  mps        - Check if the Maximum Payload Size is set to the NIC's hardware maximum.\n"
            "  hugepages  - Check if hugepages are enabled\n"
            "  gpu-clocks - Check GPU clocks\n"
            "  bar1-size  - Check the BAR1 size of the GPU\n"
            "  topo       - Check the GPU and NIC topology\n"
            "  schematic  - Write a PCIe topology image with link speed labels\n"
            "  cmdline    - Check the kernel boot parameters\n"
            "  mtu        - Check MTU of each NVIDIA interface\n"
        ),
    )

    group.add_argument("--set", choices=["mrrs"], help=("  mrrs      - Update MRRS of NICs\n"))
    parser.add_argument(
        "--schematic-output",
        default="pcie_schematic.png",
        help=(
            "Output path for --check schematic. Supported extensions are .png and .svg "
            "(default: pcie_schematic.png)."
        ),
    )

    # Check if no arguments are provided
    if len(sys.argv) == 1:
        parser.print_help()
        sys.exit(1)

    return parser.parse_args()


def is_any_integrated_gpu():
    """
    Returns True if any visible CUDA device reports CU_DEVICE_ATTRIBUTE_INTEGRATED == 1.
    Integrated GPUs (e.g. NVIDIA GB10 / DGX Spark) share memory with the CPU and
    several discrete-GPU tuning checks (peermem, GPUDirect-RDMA-supported,
    PIX/PXB topology, BAR1 size) do not apply to them.
    """
    try:
        libcuda = CDLL("libcuda.so")
        if libcuda.cuInit(0) != 0:
            return False
        cuDevAttrIntegrated = 18  # CU_DEVICE_ATTRIBUTE_INTEGRATED
        count = c_int()
        libcuda.cuDeviceGetCount(byref(count))
        for i in range(count.value):
            dev = c_int()
            libcuda.cuDeviceGet(byref(dev), i)
            flag = c_int()
            libcuda.cuDeviceGetAttribute(byref(flag), cuDevAttrIntegrated, dev)
            if flag.value == 1:
                return True
    except Exception:
        pass
    return False


def _dmabuf_gpu_path_available():
    """
    Returns True if the kernel exposes the dma-buf path that recent NVIDIA drivers
    use for GPUDirect in place of nvidia-peermem. The patched DPDK shipped with
    this repo (dpdk_patches/dmabuf.patch) takes this path on platforms that
    expose it, which is why peermem is not required there.
    """
    return os.path.exists("/dev/dma_heap/system")


def _gpu_name_by_bdf():
    """
    Returns {pci_bdf: product_name} for every visible NVIDIA GPU, or {} if
    nvidia-smi is unavailable. Used by per-GPU checks (e.g. check_bar1_size)
    to apply Blackwell-specific thresholds only to the Blackwell GPU(s) in a
    heterogeneous system rather than to every GPU in the box.
    """
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=pci.bus_id,name", "--format=csv,noheader"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return {}
    names = {}
    for line in result.stdout.splitlines():
        parts = line.split(",", 1)
        if len(parts) == 2:
            names[parts[0].strip()] = parts[1].strip()
    return names


def check_peermem_kernel():
    """
    Check if the nvidia-peermem module for GPUDirect is loaded in the kernel.

    Returns:
        bool: True if nvidia-peermem is loaded, False otherwise
    """
    try:
        # Also check for nvidia_peermem (with underscore)
        result = subprocess.run(
            ["lsmod | grep peermem"], shell=True, capture_output=True, text=True
        )

        if bool(result.stdout.strip()):
            logging.info("nvidia-peermem module is loaded.")
        elif is_any_integrated_gpu():
            logging.info(
                "nvidia-peermem module is not loaded, but the platform has an integrated GPU "
                "(e.g. GB10 / DGX Spark) where peermem does not apply. Use kind: host_pinned "
                "in the daqiri YAML for GPUDirect on this platform."
            )
        elif _dmabuf_gpu_path_available():
            logging.info(
                "nvidia-peermem module is not loaded, but /dev/dma_heap/system is "
                "available. The patched DPDK shipped with this repo "
                "(dpdk_patches/dmabuf.patch) takes the dma-buf GPUDirect path on "
                "platforms that expose it and does not need peermem. If you are "
                "building DAQIRI against stock DPDK, load nvidia-peermem."
            )
        else:
            logging.warning("nvidia-peermem module is not loaded. GPUDirect may not work.")

    except Exception as e:
        print(f"Error checking for nvidia-peermem module: {e}")
        return False


def check_gpudirect_support():
    """
    Checks if NVIDIA GPUs have access to GPUDirect.
    """
    # Load CUDA Runtime API
    libcuda = CDLL("libcuda.so")

    cudaDevAttrGPUDirectRDMASupported = 116
    cuDevAttrIntegrated = 18

    result = libcuda.cuInit(0)
    if result != 0:
        logging.error(f"CUDA initialization failed with error code: {result}")
        return
    count = c_int()
    libcuda.cuDeviceGetCount(byref(count))

    for i in range(count.value):
        device = c_int()
        libcuda.cuDeviceGet(byref(device), i)

        name = create_string_buffer(100)
        libcuda.cuDeviceGetName(name, 100, device)

        supported = c_int()
        libcuda.cuDeviceGetAttribute(byref(supported), cudaDevAttrGPUDirectRDMASupported, device)

        integrated = c_int()
        libcuda.cuDeviceGetAttribute(byref(integrated), cuDevAttrIntegrated, device)

        if bool(supported.value):
            logging.info(f"GPU {i}: {name.value.decode()} has GPUDirect support.")
        elif bool(integrated.value):
            logging.info(
                f"GPU {i}: {name.value.decode()} is integrated (unified memory). "
                "GPUDirect-RDMA-supported reported as 0 is expected on this platform; "
                "use kind: host_pinned in the daqiri YAML."
            )
        else:
            logging.warning(f"GPU {i}: {name.value.decode()} does not have GPUDirect support.")


@functools.lru_cache(maxsize=1)
def get_nic_info():
    """
    Parses the output of `ibdev2netdev -v` to extract and return a list of tuples,
    where each tuple contains the interface name and its PCIe address.

    Cached with lru_cache so --check all (which calls this from check_mrrs,
    check_max_payload_size, and check_mtu_size) only invokes ibdev2netdev once
    and only emits the "ibdev2netdev not found" warning once per run.

    Returns:
        List[Tuple[str, str]]: A list of tuples containing the IF name and PCIe address
    """
    try:
        # Run ibdev2netdev -v to get detailed information about Mellanox devices
        result = subprocess.run(["ibdev2netdev", "-v"], capture_output=True, text=True, check=True)

        # Parse the output to extract interface names and PCIe addresses
        vals = []
        for line in result.stdout.splitlines():
            match = re.match(r"([\S\:\.]+) .*==>\s+(\S+)", line)
            if match:
                pcie_address = match.group(1)
                interface_name = match.group(2)
                vals.append((interface_name, pcie_address))

        return vals

    except FileNotFoundError:
        logging.warning(
            "The ibdev2netdev command is not found (try: apt install infiniband-diags). "
            "Skipping NIC-dependent checks (mrrs, mps, mtu)."
        )
        return []
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while executing ibdev2netdev: {e}")
        return []
    except Exception as e:
        logging.error(f"Unexpected error while running ibdev2netdev: {e}")
        return []


def get_online_cpus():
    """
    Returns a list of online CPUs by reading /sys/devices/system/cpu/online.
    """
    try:
        with open("/sys/devices/system/cpu/online", "r") as f:
            online_cpus = f.read().strip()

        # Parse ranges (e.g., "0-3" -> [0, 1, 2, 3])
        cpu_list = []
        for part in online_cpus.split(","):
            if "-" in part:
                start, end = map(int, part.split("-"))
                cpu_list.extend(range(start, end + 1))
            else:
                cpu_list.append(int(part))

        return cpu_list
    except FileNotFoundError:
        logging.error(
            "Could not determine online CPUs. File /sys/devices/system/cpu/online not found."
        )
        sys.exit(1)


def check_cpu_governor():
    """
    Checks if the CPU frequency governor is set to 'performance' for all online CPUs.
    Output is bucketed by result so a 256-core system does not emit 256 lines when
    every CPU is in the same state. Per-CPU detail still surfaces if results vary.
    """
    online_cpus = get_online_cpus()
    total = len(online_cpus)

    by_governor = defaultdict(list)
    missing = []
    permission_denied = []

    for cpu in online_cpus:
        scaling_governor_path = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor"
        try:
            with open(scaling_governor_path, "r") as f:
                by_governor[f.read().strip()].append(cpu)
        except FileNotFoundError:
            missing.append(cpu)
        except PermissionError:
            permission_denied.append(cpu)

    for governor, cpus in sorted(by_governor.items()):
        if governor == "performance":
            logging.info(
                f"CPU governor: {len(cpus)}/{total} online CPUs set to 'performance'."
            )
        else:
            logging.warning(
                f"CPU governor: {len(cpus)}/{total} online CPUs set to '{governor}', "
                "expected 'performance'."
            )

    if missing:
        logging.error(
            f"CPU governor: scaling_governor file not found on {len(missing)}/{total} "
            "online CPUs. The cpufreq driver may not be loaded (e.g. amd-pstate, "
            "intel_pstate, or cppc_cpufreq). Performance scaling cannot be checked."
        )
    if permission_denied:
        logging.error(
            f"CPU governor: permission denied reading scaling_governor on "
            f"{len(permission_denied)}/{total} online CPUs. Run as root."
        )


def check_mrrs():
    """
    Checks if the Maximum Read Request Size (MRRS) of NVIDIA Ethernet controllers
    is set to 4096.
    """
    try:
        nic_info = get_nic_info()
        for intf in nic_info:
            name = intf[0]
            pci_address = intf[1]

            # Query MRRS for the NIC using setpci
            mrrs_result = subprocess.run(
                ["setpci", "-s", pci_address, "68.w"], capture_output=True, text=True, check=True
            )

            # Convert MRRS value from hexadecimal to decimal
            mrrs_value = (int(mrrs_result.stdout.strip(), 16) & 0xF000) >> 12

            if mrrs_value == 5:
                logging.info(f"{name}/{pci_address}: MRRS is correctly set to 4096.")
            else:
                logging.warning(
                    f"{name}/{pci_address}: MRRS is set to {2**(7+mrrs_value)}, not 4096."
                )

    except FileNotFoundError:
        logging.error("The required tools (lspci or setpci) are not available on this system.")
        sys.exit(1)
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while checking MRRS: {e}")
        sys.exit(1)


def check_max_payload_size():
    """
    Checks the Maximum Payload Size (MPS) of NVIDIA Ethernet controllers.
    Reads the hardware-maximum MPS from DevCap and warns if the DevCtl value is less than that.
    If DevCap cannot be parsed, warns with the current MPS value for manual inspection.
    """
    try:
        nic_info = get_nic_info()
        for intf in nic_info:
            name = intf[0]
            pci_address = intf[1]

            # Query detailed device information using lspci -vv
            mps_result = subprocess.run(
                ["lspci", "-vv", "-s", pci_address], capture_output=True, text=True, check=True
            )

            lines = mps_result.stdout.splitlines()

            # Parse hardware-maximum MPS from DevCap (appears on the same line)
            devcap_max = None
            for line in lines:
                if "DevCap:" in line and "MaxPayload" in line:
                    try:
                        devcap_max = int(line.split("MaxPayload")[1].split("bytes")[0].strip())
                    except (ValueError, IndexError):
                        pass
                    break

            # Parse current MPS from the DevCtl section
            devctl_found = False
            for i, line in enumerate(lines):
                if "DevCtl:" in line:
                    devctl_found = True
                    base_indent = len(line) - len(line.lstrip())
                    for j in range(i + 1, len(lines)):
                        current_indent = len(lines[j]) - len(lines[j].lstrip())
                        if current_indent <= base_indent:
                            break
                        if "MaxPayload" in lines[j]:
                            payload_info = lines[j].strip()
                            try:
                                current_mps = int(
                                    payload_info.split("MaxPayload")[1].split("bytes")[0].strip()
                                )
                            except (ValueError, IndexError):
                                logging.error(
                                    f"{name}/{pci_address}: Could not parse MaxPayload value under DevCtl."
                                )
                                break
                            if devcap_max is None:
                                logging.warning(
                                    f"{name}/{pci_address}: Could not determine hardware-maximum MPS from DevCap."
                                    f" Current MPS is {current_mps} bytes — verify this is optimal for your system."
                                )
                            elif current_mps < devcap_max:
                                logging.warning(
                                    f"{name}/{pci_address}: PCIe Max Payload Size is {current_mps} bytes,"
                                    f" but the NIC supports up to {devcap_max} bytes."
                                )
                            else:
                                logging.info(
                                    f"{name}/{pci_address}: PCIe Max Payload Size is correctly set to {current_mps} bytes"
                                    f" (hardware maximum)."
                                )
                            break
                    else:
                        logging.error(
                            f"{name}/{pci_address}: Unable to find MaxPayload information under DevCtl."
                        )
                    break

            if not devctl_found:
                logging.error(f"{name}/{pci_address}: DevCtl section not found.")

    except FileNotFoundError:
        logging.error("The required tools (lspci) are not available on this system.")
        sys.exit(1)
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while checking Max Payload Size: {e}")
        sys.exit(1)


def check_hugepages():
    """
    Checks if hugepages are allocated and ensures that the total allocated hugepage memory
    is at least 500 MB.
    """
    try:
        # Initialize variables
        total_hugepages = 0
        hugepage_size_kB = 0

        # Read /proc/meminfo for hugepage details
        with open("/proc/meminfo", "r") as file:
            for line in file:
                if "HugePages_Total" in line:
                    total_hugepages = int(line.split(":")[1].strip())
                elif "Hugepagesize" in line:
                    hugepage_size_kB = int(line.split(":")[1].strip().split()[0])  # Size in kB

        # Check if hugepages are allocated
        if total_hugepages > 0:
            # Calculate total memory allocated to hugepages in MB
            hugepage_size_MB = hugepage_size_kB / 1024  # Convert kB to MB
            total_allocated_memory_MB = total_hugepages * hugepage_size_MB

            logging.info(f"HugePages_Total: {total_hugepages}")
            logging.info(f"HugePage Size: {hugepage_size_MB:.2f} MB")
            logging.info(f"Total Allocated HugePage Memory: {total_allocated_memory_MB:.2f} MB")

            # Check if the total memory meets the 500 MB requirement
            if total_allocated_memory_MB >= 500:
                logging.info("Hugepages are sufficiently allocated with at least 500 MB.")
                return True
            else:
                logging.warning("Hugepages are allocated but do not meet the 500 MB requirement.")
                return False
        else:
            logging.warning("No hugepages are allocated.")
            return False

    except FileNotFoundError:
        logging.error("/proc/meminfo not found. Are you sure you're running on Linux?")
        return False
    except Exception as e:
        logging.error(f"An error occurred while checking hugepages: {e}")
        return False


def check_nvidia_gpu_clocks():
    """
    Checks all NVIDIA GPUs to ensure that the SM clock and memory clock are set to their maximum values.
    If not, logs the current and maximum values for each GPU.
    """
    try:
        # Define the fields to query
        fields = ["clocks.sm", "clocks.max.sm", "clocks.mem", "clocks.max.mem"]
        query = ",".join(fields)

        # Run nvidia-smi to get clock information
        result = subprocess.run(
            ["nvidia-smi", f"--query-gpu={query}", "--format=csv,noheader,nounits"],
            capture_output=True,
            text=True,
            check=True,
        )

        # Parse the output of nvidia-smi
        output = result.stdout.strip().splitlines()

        for idx, line in enumerate(output):
            fields = [f.strip() for f in line.split(",")]

            def parse_clock(val):
                try:
                    return int(val)
                except ValueError:
                    return None

            sm_current, sm_max, mem_current, mem_max = (parse_clock(f) for f in fields)

            logging.debug(f"GPU {idx}: Checking clocks...")

            # Some GPUs have a boost clock that appears as the "max clock", but when you set the
            # GPU to that frequency it will not report as that with nvidia-smi. For example:
            # nvidia-smi --lock-gpu-clocks 3105 --mode 1
            # GPU clocks set to "(gpuClkMin 3105, gpuClkMax 3105)" for GPU 00000005:09:00.0
            #
            # Clocks
            #  SM                                : 2730 MHz
            #
            # Anecdotally if a user has not set their clocks at all the value will be very low,
            # around 100-300MHz. Having a check within 500MHz should be sufficient to catch this.
            if sm_max is None:
                logging.info(f"GPU {idx}: SM clock reported as N/A (not applicable for this GPU).")
            elif sm_current is None:
                logging.warning(f"GPU {idx}: SM clock current value is N/A but max is {sm_max} MHz — unexpected.")
            else:
                sm_margin = 500
                if abs(sm_current - sm_max) > sm_margin:
                    logging.warning(
                        f"GPU {idx}: SM Clock is set to {sm_current} MHz, but should be within {sm_margin} MHz of the {sm_max} MHz theoretical Max."
                    )
                elif sm_current < sm_max:
                    logging.info(
                        f"GPU {idx}: SM Clock is correctly set to {sm_current} MHz (within {sm_margin} of the {sm_max} MHz theoretical Max)."
                    )
                else:
                    logging.info(f"GPU {idx}: SM Clock is correctly set to {sm_current} MHz.")

            # nvidia-smi has a bug where the memory clock is reported as 1 MHz less than the max in
            # some cases
            if mem_max is None:
                logging.info(f"GPU {idx}: Memory clock reported as N/A (not applicable for this GPU).")
            elif mem_current is None:
                logging.warning(f"GPU {idx}: Memory clock current value is N/A but max is {mem_max} MHz — unexpected.")
            elif abs(mem_current - mem_max) > 1:
                logging.warning(
                    f"GPU {idx}: Memory Clock is set to {mem_current} MHz, but should be {mem_max} MHz."
                )
            else:
                logging.info(f"GPU {idx}: Memory Clock is correctly set to {mem_current} MHz.")

    except FileNotFoundError:
        logging.error("nvidia-smi command not found. Ensure NVIDIA drivers are installed.")
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while querying NVIDIA GPUs: {e}")
    except Exception as e:
        logging.error(f"An unexpected error occurred: {e}")


def check_bar1_size():
    """
    Checks the BAR1 size of all NVIDIA GPUs using nvidia-smi.
    Logs the BAR1 size for each GPU and ensures it is non-zero.
    """
    if is_any_integrated_gpu():
        logging.info(
            "BAR1 size check skipped: integrated GPU detected (unified memory). "
            "There is no resizable BAR1 to enlarge on platforms like GB10 / DGX Spark."
        )
        return
    # On RTX PRO 6000 Blackwell Server Edition (96 GB GDDR7) the generic
    # > 1024 MiB threshold passes trivially even with Resizable BAR disabled
    # (the card still exposes a multi-GiB BAR1 in some platform configs).
    # 32 GiB is the conservative "rebar is fully unlocked" floor: well below
    # the 96 GB card capacity but high enough that any platform exposing less
    # is almost certainly missing Resizable BAR / Above 4G Decoding in BIOS.
    # The threshold is applied per-GPU via gpu_names below so heterogeneous
    # boxes (e.g. RTX PRO 6000 + H100) only get the Blackwell rule on the
    # Blackwell card.
    BAR1_BLACKWELL_MIN_MIB = 32768  # 32 GiB
    gpu_names = _gpu_name_by_bdf()
    try:
        # Run nvidia-smi to get BAR1 memory information
        result = subprocess.run(
            ["nvidia-smi", "-q", "-d", "MEMORY"], capture_output=True, text=True, check=True
        )

        # Parse the output of nvidia-smi
        output = result.stdout.splitlines()

        current_gpu = None
        bar1_total = None

        for i, line in enumerate(output):
            line = line.strip()

            # Detect GPU identifier
            if line.startswith("GPU"):
                current_gpu = line.split()[1].strip(":")
                bar1_total = None

            # Parse BAR1 total size under "BAR1 Memory Usage" section
            elif "BAR1 Memory Usage" in line:
                continue  # Skip the header for BAR1 Memory Usage section
            elif "Total" in line and "BAR1" in output[i - 1]:
                bar1_total_str = line.split(":")[1].strip()
                if "MiB" in bar1_total_str:
                    bar1_total = int(bar1_total_str.split()[0])

            # Once BAR1 size is found, log it
            if current_gpu is not None and bar1_total is not None:
                gpu_name = gpu_names.get(current_gpu, "")
                gpu_is_blackwell = "Blackwell Server Edition" in gpu_name
                if gpu_is_blackwell and bar1_total < BAR1_BLACKWELL_MIN_MIB:
                    logging.warning(
                        f"GPU {current_gpu} ({gpu_name}): BAR1 size is {bar1_total} MiB. "
                        f"Expected at least {BAR1_BLACKWELL_MIN_MIB} MiB "
                        f"({BAR1_BLACKWELL_MIN_MIB // 1024} GiB) with Resizable BAR fully "
                        "enabled. Check the system BIOS for the Resizable BAR / Above 4G "
                        "Decoding settings."
                    )
                elif bar1_total > 1024:
                    logging.info(f"GPU {current_gpu}: BAR1 size is {bar1_total} MiB.")
                else:
                    logging.warning(
                        f"GPU {current_gpu}: BAR1 size is {bar1_total} MiB. This may indicate an issue."
                    )

                # Reset variables for the next GPU section
                current_gpu = None

    except FileNotFoundError:
        logging.error("nvidia-smi command not found. Ensure NVIDIA drivers are installed.")
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while querying NVIDIA GPUs: {e}")
    except Exception as e:
        logging.error(f"An unexpected error occurred: {e}")


def read_text_file(path, default=""):
    """
    Reads a small text file and returns a stripped string. Missing sysfs/procfs
    entries are common on different platforms, so callers can provide a default.
    """
    try:
        with open(path, "r") as file:
            return file.read().strip()
    except (FileNotFoundError, PermissionError, OSError):
        return default


def normalize_pci_address(address):
    """
    Normalizes a PCI BDF to the Linux sysfs form, e.g. 0000:17:00.0.
    nvidia-smi can report an 8-hex-digit domain, so keep the low 16 bits.
    """
    match = PCI_BDF_RE.match(os.path.basename(address.strip()))
    if not match:
        return None

    domain, bus, device, function = match.groups()
    return f"{domain[-4:].lower()}:{bus.lower()}:{device.lower()}.{function}"


def pci_bdfs_from_path(path):
    """
    Extracts the PCI BDF chain from a sysfs path.
    """
    bdfs = []
    for part in os.path.realpath(path).split(os.sep):
        bdf = normalize_pci_address(part)
        if bdf is not None and (not bdfs or bdfs[-1] != bdf):
            bdfs.append(bdf)
    return bdfs


def pci_root_from_path(path):
    """
    Extracts the sysfs PCI root identifier, e.g. pci0000:00.
    """
    for part in os.path.realpath(path).split(os.sep):
        if PCI_ROOT_RE.match(part):
            return part.lower()
    return "pci????:??"


def class_name_for_code(class_code):
    """
    Returns a readable PCI class fallback when lspci is unavailable.
    """
    base = (class_code >> 16) & 0xFF
    subclass = (class_code >> 8) & 0xFF

    if base == 0x01 and subclass == 0x08:
        return "Non-Volatile memory controller"
    if base == 0x01:
        return "Mass storage controller"
    if base == 0x02:
        return "Network controller"
    if base == 0x03:
        return "Display controller"
    if base == 0x06 and subclass == 0x00:
        return "Host bridge"
    if base == 0x06 and subclass == 0x04:
        return "PCI bridge"
    return f"PCI class 0x{class_code:06x}"


def classify_pci_kind(class_code, vendor_id=""):
    """
    Classifies a PCI device into the kinds shown in the schematic.
    """
    base = (class_code >> 16) & 0xFF
    subclass = (class_code >> 8) & 0xFF

    if base == 0x03 and vendor_id.lower() == "0x10de":
        return "GPU"
    if base == 0x03:
        return "DISPLAY"
    if base == 0x02:
        return "NIC"
    if base == 0x01 and subclass == 0x08:
        return "NVME"
    if base == 0x01:
        return "DISK"
    if base == 0x06 and subclass == 0x00:
        return "HOST"
    if base == 0x06 and subclass == 0x04:
        return "BRIDGE"
    return "OTHER"


def get_lspci_descriptions():
    """
    Gets machine-readable lspci descriptions keyed by normalized BDF.
    """
    descriptions = {}
    try:
        result = subprocess.run(["lspci", "-Dmm"], capture_output=True, text=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return descriptions

    for line in result.stdout.splitlines():
        try:
            parts = shlex.split(line)
        except ValueError:
            continue

        if len(parts) < 4:
            continue

        bdf = normalize_pci_address(parts[0])
        if bdf is None:
            continue

        class_name = parts[1]
        description = " ".join(part for part in parts[2:4] if part)
        descriptions[bdf] = (class_name, description)

    return descriptions


def get_lspci_link_info():
    """
    Gets PCIe link status/capability data keyed by normalized BDF.
    """
    link_info = {}
    try:
        result = subprocess.run(["lspci", "-Dvv"], capture_output=True, text=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return link_info

    current_bdf = None
    for line in result.stdout.splitlines():
        header_match = re.match(r"^([0-9a-fA-F:.]+)\s+", line)
        if header_match:
            current_bdf = normalize_pci_address(header_match.group(1))
            if current_bdf is not None:
                link_info.setdefault(current_bdf, {})
            continue

        if current_bdf is None:
            continue

        stripped = line.strip()
        if stripped.startswith("LnkCap:"):
            speed, width = parse_lspci_link_line(stripped)
            if speed:
                link_info.setdefault(current_bdf, {})["max_speed"] = speed
            if width:
                link_info.setdefault(current_bdf, {})["max_width"] = width
        elif stripped.startswith("LnkSta:"):
            speed, width = parse_lspci_link_line(stripped)
            if speed:
                link_info.setdefault(current_bdf, {})["speed"] = speed
            if width:
                link_info.setdefault(current_bdf, {})["width"] = width

    return link_info


def parse_lspci_link_line(line):
    """
    Parses speed and width from an lspci LnkCap/LnkSta line.
    """
    speed = ""
    width = ""

    speed_match = re.search(r"Speed\s+([0-9.]+GT/s)", line)
    if speed_match:
        speed = speed_match.group(1)

    width_match = re.search(r"Width\s+x([0-9]+)", line)
    if width_match:
        width = f"x{width_match.group(1)}"

    return speed, width


def parse_pcie_speed_gtps(speed):
    match = re.search(r"([0-9.]+)\s*GT/s", speed or "")
    if not match:
        return None

    try:
        return float(match.group(1))
    except ValueError:
        return None


def parse_pcie_width(width):
    match = re.search(r"x([0-9]+)", width or "")
    if not match:
        return None

    try:
        return int(match.group(1))
    except ValueError:
        return None


def pcie_generation(speed):
    gtps = parse_pcie_speed_gtps(speed)
    if gtps is None:
        return ""

    generation_by_speed = {
        2.5: "Gen1",
        5.0: "Gen2",
        8.0: "Gen3",
        16.0: "Gen4",
        32.0: "Gen5",
        64.0: "Gen6",
    }
    closest = min(generation_by_speed, key=lambda candidate: abs(candidate - gtps))
    if abs(closest - gtps) < 0.2:
        return generation_by_speed[closest]
    return f"{gtps:g}GT/s"


def pcie_bandwidth_gb_per_sec(speed, width):
    gtps = parse_pcie_speed_gtps(speed)
    lanes = parse_pcie_width(width)
    if gtps is None or lanes is None:
        return None

    closest = min(
        PCIE_EFFECTIVE_GB_PER_SEC_PER_LANE,
        key=lambda candidate: abs(candidate - gtps),
    )
    if abs(closest - gtps) >= 0.2:
        return None

    return PCIE_EFFECTIVE_GB_PER_SEC_PER_LANE[closest] * lanes


def format_pcie_link_label(device):
    """
    Formats the upstream link from a device's parent bridge to the device.
    """
    speed = device.max_link_speed or device.link_speed
    width = device.max_link_width or device.link_width
    if not speed or not width:
        return "link unknown"

    generation = pcie_generation(speed)
    bandwidth = pcie_bandwidth_gb_per_sec(speed, width)
    label = f"{generation} {width}"
    if bandwidth is not None:
        label += f"\n{bandwidth:.1f} GB/s"

    if (
        device.link_speed
        and device.link_width
        and (device.link_speed != speed or device.link_width != width)
    ):
        current_generation = pcie_generation(device.link_speed)
        label += f"\ncurrent {current_generation} {device.link_width}"

    return label


def get_nvidia_gpu_info_by_bdf():
    """
    Gets nvidia-smi GPU labels and names keyed by normalized BDF.
    """
    gpus = {}
    try:
        result = subprocess.run(
            [
                "nvidia-smi",
                "--query-gpu=index,pci.bus_id,name",
                "--format=csv,noheader",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return gpus

    for line in result.stdout.splitlines():
        parts = [part.strip() for part in line.split(",", 2)]
        if len(parts) != 3:
            continue

        index, bus_id, name = parts
        bdf = normalize_pci_address(bus_id)
        if bdf is None:
            continue

        gpus[bdf] = {"label": f"GPU{index}", "name": name}

    return gpus


def collect_names_by_pci_bdf(sysfs_dir, skip_names=None):
    """
    Maps sysfs objects such as netdevs, RDMA devices, or block devices to PCI BDFs.
    """
    names_by_bdf = defaultdict(list)
    skip_names = set(skip_names or [])

    if not os.path.isdir(sysfs_dir):
        return names_by_bdf

    for name in sorted(os.listdir(sysfs_dir)):
        if name in skip_names:
            continue

        path = os.path.join(sysfs_dir, name)
        chain = pci_bdfs_from_path(path)
        if chain:
            names_by_bdf[chain[-1]].append(name)

    return names_by_bdf


def collect_cpu_nodes():
    """
    Reads NUMA CPU node cpulists from sysfs.
    """
    nodes = {}
    node_dir = "/sys/devices/system/node"

    if os.path.isdir(node_dir):
        for name in sorted(os.listdir(node_dir)):
            if not name.startswith("node") or not name[4:].isdigit():
                continue

            node_id = int(name[4:])
            cpulist = read_text_file(os.path.join(node_dir, name, "cpulist"), "unknown")
            nodes[node_id] = cpulist or "unknown"

    if not nodes:
        nodes[0] = read_text_file("/sys/devices/system/cpu/online", "unknown") or "unknown"

    return nodes


def next_unused_label(prefix, used_labels):
    """
    Returns the next prefixN label not already in use.
    """
    index = 0
    while f"{prefix}{index}" in used_labels:
        index += 1

    label = f"{prefix}{index}"
    used_labels.add(label)
    return label


def assign_topology_labels(devices, gpu_info_by_bdf):
    """
    Assigns readable labels to GPUs, NICs, and storage endpoints.
    """
    used_gpu_labels = set()
    for bdf, gpu_info in gpu_info_by_bdf.items():
        if bdf not in devices or devices[bdf].kind != "GPU":
            continue

        devices[bdf].label = gpu_info["label"]
        used_gpu_labels.add(gpu_info["label"])
        if gpu_info.get("name"):
            devices[bdf].description = gpu_info["name"]

    for device in sorted(
        [device for device in devices.values() if device.kind == "GPU"],
        key=lambda item: item.bdf,
    ):
        if not device.label:
            device.label = next_unused_label("GPU", used_gpu_labels)

    used_nic_labels = set()
    for device in sorted(
        [device for device in devices.values() if device.kind == "NIC"],
        key=lambda item: item.bdf,
    ):
        if device.net_names:
            label = device.net_names[0]
        elif device.rdma_names:
            label = device.rdma_names[0]
        else:
            device.label = next_unused_label("NIC", used_nic_labels)
            continue

        if label in used_nic_labels:
            label = f"{label}/{device.bdf}"
        device.label = label
        used_nic_labels.add(label)

    for prefix, kind in [("NVME", "NVME"), ("DISK", "DISK")]:
        used_labels = set()
        for device in sorted(
            [device for device in devices.values() if device.kind == kind],
            key=lambda item: item.bdf,
        ):
            device.label = next_unused_label(prefix, used_labels)


def collect_pci_devices():
    """
    Collects PCI devices from sysfs and annotates them with useful endpoint aliases.
    """
    devices = {}
    if not os.path.isdir(PCI_SYSFS_DEVICES):
        return devices

    lspci_descriptions = get_lspci_descriptions()
    lspci_link_info = get_lspci_link_info()
    gpu_info_by_bdf = get_nvidia_gpu_info_by_bdf()
    net_names_by_bdf = collect_names_by_pci_bdf("/sys/class/net", skip_names={"lo"})
    rdma_names_by_bdf = collect_names_by_pci_bdf("/sys/class/infiniband")
    block_names_by_bdf = collect_names_by_pci_bdf(
        "/sys/block",
        skip_names={"loop0", "loop1", "loop2", "loop3", "loop4", "loop5", "loop6", "loop7"},
    )

    for entry in sorted(os.listdir(PCI_SYSFS_DEVICES)):
        bdf = normalize_pci_address(entry)
        if bdf is None:
            continue

        device_path = os.path.join(PCI_SYSFS_DEVICES, entry)
        class_text = read_text_file(os.path.join(device_path, "class"), "0x000000")
        try:
            class_code = int(class_text, 16)
        except ValueError:
            class_code = 0

        class_name, description = lspci_descriptions.get(
            bdf, (class_name_for_code(class_code), "")
        )
        vendor_id = read_text_file(os.path.join(device_path, "vendor"), "").lower()
        path_chain = pci_bdfs_from_path(device_path)
        parent_bdf = path_chain[-2] if len(path_chain) >= 2 else ""
        numa_text = read_text_file(os.path.join(device_path, "numa_node"), "-1")
        try:
            numa_node = int(numa_text)
        except ValueError:
            numa_node = -1
        link_info = lspci_link_info.get(bdf, {})

        devices[bdf] = PciDevice(
            bdf=bdf,
            class_code=class_code,
            vendor_id=vendor_id,
            class_name=class_name,
            description=description,
            kind=classify_pci_kind(class_code, vendor_id),
            numa_node=numa_node,
            root=pci_root_from_path(device_path),
            parent_bdf=parent_bdf,
            link_speed=link_info.get("speed", ""),
            link_width=link_info.get("width", ""),
            max_link_speed=link_info.get("max_speed", ""),
            max_link_width=link_info.get("max_width", ""),
            path_chain=path_chain,
            net_names=net_names_by_bdf.get(bdf, []),
            rdma_names=rdma_names_by_bdf.get(bdf, []),
            block_names=block_names_by_bdf.get(bdf, []),
        )

    assign_topology_labels(devices, gpu_info_by_bdf)
    return devices


def topology_endpoint_devices(devices):
    """
    Returns the endpoint devices shown in the schematic.
    """
    return sorted(
        [
            device
            for device in devices.values()
            if device.kind in {"GPU", "NIC", "NVME"}
            or (device.kind == "DISK" and device.block_names)
        ],
        key=lambda item: (item.kind, item.bdf),
    )


def infer_root_numa_nodes(devices, cpu_nodes=None):
    """
    Infers each PCI root's NUMA node from child devices.
    """
    root_numa_nodes = {}
    cpu_nodes = cpu_nodes or {}
    fallback_numa_node = next(iter(cpu_nodes.keys())) if len(cpu_nodes) == 1 else -1
    roots = sorted({device.root for device in devices.values()})

    for root in roots:
        candidates = [
            device.numa_node
            for device in devices.values()
            if device.root == root and device.numa_node >= 0
        ]
        if candidates:
            root_numa_nodes[root] = Counter(candidates).most_common(1)[0][0]
        else:
            root_numa_nodes[root] = fallback_numa_node

    return root_numa_nodes


def included_topology_bdfs(devices, endpoints):
    """
    Includes each endpoint and its PCI bridge ancestry.
    """
    included = set()
    for endpoint in endpoints:
        for bdf in endpoint.path_chain:
            if bdf in devices:
                included.add(bdf)
    return included


def common_prefix_length(left, right):
    """
    Counts matching items at the start of two lists.
    """
    count = 0
    for left_item, right_item in zip(left, right):
        if left_item != right_item:
            break
        count += 1
    return count


def common_pci_ancestor(left, right):
    """
    Returns the deepest common PCI BDF in two endpoint paths.
    """
    shared_depth = common_prefix_length(left.path_chain, right.path_chain)
    if shared_depth <= 0:
        return ""
    return left.path_chain[shared_depth - 1]


def classify_topology_connection(left, right, devices, root_numa_nodes):
    """
    Classifies two PCI endpoints using nvidia-smi topo -m nomenclature.
    """
    if left.bdf == right.bdf:
        return "X"

    if left.root == right.root:
        shared_depth = common_prefix_length(left.path_chain, right.path_chain)
        if shared_depth > 0:
            common_bdf = common_pci_ancestor(left, right)
            common_device = devices.get(common_bdf)
            if shared_depth == 1 and common_device is not None and common_device.kind == "HOST":
                return "PHB"
            hops_below_common_bridge = (
                len(left.path_chain) - shared_depth + len(right.path_chain) - shared_depth
            )
            return "PIX" if hops_below_common_bridge <= 2 else "PXB"
        return "PHB"

    left_numa = left.numa_node if left.numa_node >= 0 else root_numa_nodes.get(left.root, -1)
    right_numa = right.numa_node if right.numa_node >= 0 else root_numa_nodes.get(right.root, -1)

    if left_numa >= 0 and left_numa == right_numa:
        return "NODE"
    return "SYS"


def connection_description(connection_type):
    """
    Describes the nvidia-smi topo -m connection class.
    """
    return {
        "X": "same device",
        "PIX": "traverses at most one PCIe bridge",
        "PXB": "traverses multiple PCIe bridges without crossing a host bridge",
        "PHB": "crosses a PCIe host bridge",
        "NODE": "crosses PCIe host bridges within one NUMA node",
        "SYS": "crosses NUMA nodes or CPU sockets",
    }.get(connection_type, "unknown topology class")


def render_topology_connection_summary(devices, root_numa_nodes):
    """
    Renders optimal and suboptimal GPU-to-I/O connection lines.
    """
    gpus = sorted(
        [device for device in devices.values() if device.kind == "GPU"],
        key=lambda item: item.label,
    )
    io_devices = sorted(
        [
            device
            for device in devices.values()
            if device.kind in {"NIC", "NVME"}
            or (device.kind == "DISK" and device.block_names)
        ],
        key=lambda item: (item.kind, item.label),
    )

    lines = [
        "",
        "Topology connection classes:",
        "  PIX  " + connection_description("PIX"),
        "  PXB  " + connection_description("PXB"),
        "  PHB  " + connection_description("PHB"),
        "  NODE " + connection_description("NODE"),
        "  SYS  " + connection_description("SYS"),
        "",
    ]

    if not gpus:
        lines.append("No GPUs were found for GPU-to-I/O connection classification.")
        return lines
    if not io_devices:
        lines.append("No NICs, NVMe devices, or PCIe disks were found for classification.")
        return lines

    optimal = []
    suboptimal = []
    for gpu in gpus:
        for io_device in io_devices:
            connection_type = classify_topology_connection(
                gpu, io_device, devices, root_numa_nodes
            )
            line = (
                f"  {gpu.label} <-> {io_device.label}: {connection_type} - "
                f"{connection_description(connection_type)} "
                f"({gpu.bdf} <-> {io_device.bdf})"
            )
            if connection_type in OPTIMAL_TOPOLOGY_CONNECTIONS:
                optimal.append(line)
            else:
                suboptimal.append(line)

    lines.append("Optimal connections (PIX/PXB):")
    if optimal:
        lines.extend(optimal)
    else:
        lines.append("  None found.")

    lines.append("")
    lines.append("Suboptimal connections (PHB/NODE/SYS):")
    if suboptimal:
        lines.extend(suboptimal)
    else:
        lines.append("  None found.")

    return lines


def annotate_optimal_peer_fabrics(devices, root_numa_nodes):
    """
    Marks shared PCIe switch/bridge ancestors for optimal GPU-to-I/O peer paths.
    """
    for device in devices.values():
        device.notes = []

    gpus = sorted(
        [device for device in devices.values() if device.kind == "GPU"],
        key=lambda item: item.label,
    )
    io_devices = sorted(
        [
            device
            for device in devices.values()
            if device.kind in {"NIC", "NVME"}
            or (device.kind == "DISK" and device.block_names)
        ],
        key=lambda item: (item.kind, item.label),
    )

    peer_fabrics = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for gpu in gpus:
        for io_device in io_devices:
            connection_type = classify_topology_connection(
                gpu, io_device, devices, root_numa_nodes
            )
            if connection_type not in OPTIMAL_TOPOLOGY_CONNECTIONS:
                continue

            common_bdf = common_pci_ancestor(gpu, io_device)
            if common_bdf not in devices:
                continue

            peer_fabrics[common_bdf][gpu.label][connection_type].append(io_device.label)

    for common_bdf, gpu_map in peer_fabrics.items():
        device = devices[common_bdf]
        if device.kind not in {"BRIDGE", "HOST"}:
            continue

        for gpu_label, connection_map in sorted(gpu_map.items()):
            for connection_type, io_labels in sorted(connection_map.items()):
                device.notes.append(
                    f"{connection_type} peer fabric: {gpu_label} <-> {','.join(sorted(io_labels))}"
                )


def device_schematic_lines(device):
    """
    Returns compact box text for one PCI device.
    """
    if device.kind == "GPU":
        first_line = f"{device.label} GPU"
    elif device.kind == "NIC":
        first_line = f"{device.label} NIC"
    elif device.kind == "NVME":
        first_line = f"{device.label} NVMe"
    elif device.kind == "DISK":
        first_line = f"{device.label} Disk"
    elif device.kind == "BRIDGE":
        if "ConnectX" in device.description:
            first_line = "ConnectX PCIe Switch"
        elif "Switch" in device.description:
            first_line = "PCIe Switch"
        else:
            first_line = "PCIe Bridge / Root Port"
    elif device.kind == "HOST":
        first_line = "Host Bridge"
    else:
        first_line = device.class_name

    lines = [first_line, device.bdf]
    if device.description:
        lines.append(device.description)

    aliases = []
    if device.net_names:
        net_aliases = [name for name in device.net_names if name != device.label]
        if net_aliases:
            aliases.append("net " + ",".join(net_aliases))
    if device.rdma_names:
        aliases.append("rdma " + ",".join(device.rdma_names))
    if device.block_names:
        aliases.append("block " + ",".join(device.block_names))
    if aliases:
        lines.append("; ".join(aliases))
    lines.extend(device.notes)
    if device.numa_node >= 0:
        lines.append(f"NUMA node {device.numa_node}")

    return lines


def schematic_node_width(kind):
    return {
        "CPU": 260,
        "HOST": 270,
        "BRIDGE": 340,
        "GPU": 360,
        "NIC": 360,
        "NVME": 360,
        "DISK": 360,
    }.get(kind, 320)


def build_schematic_forest(devices, cpu_nodes, root_numa_nodes):
    """
    Builds a visual tree: CPU node -> host bridge -> PCIe bridge/switch -> endpoint.
    """
    endpoints = topology_endpoint_devices(devices)
    included = included_topology_bdfs(devices, endpoints)
    children_by_parent = defaultdict(list)
    roots = set()

    for bdf in included:
        device = devices[bdf]
        roots.add(device.root)
        parent = device.parent_bdf if device.parent_bdf in included else device.root
        children_by_parent[parent].append(bdf)

    roots_by_numa = defaultdict(list)
    unknown_roots = []
    for root in sorted(roots):
        numa_node = root_numa_nodes.get(root, -1)
        if numa_node >= 0:
            roots_by_numa[numa_node].append(root)
        else:
            unknown_roots.append(root)

    def build_device_node(bdf, depth):
        device = devices[bdf]
        node = SchematicNode(
            key=bdf,
            lines=device_schematic_lines(device),
            kind=device.kind,
            depth=depth,
            link_label=format_pcie_link_label(device),
        )
        node.children = [
            build_device_node(child_bdf, depth + 1)
            for child_bdf in sorted(children_by_parent.get(bdf, []))
        ]
        return node

    def build_host_node(root, depth):
        node = SchematicNode(
            key=root,
            lines=["Host Bridge", root],
            kind="HOST",
            depth=depth,
            link_label="root complex",
        )
        node.children = [
            build_device_node(child_bdf, depth + 1)
            for child_bdf in sorted(children_by_parent.get(root, []))
        ]
        return node

    forest = []
    for node_id in sorted(roots_by_numa):
        cpus = cpu_nodes.get(node_id, "unknown")
        cpu_node = SchematicNode(
            key=f"cpu-node-{node_id}",
            lines=[f"CPU/SoC NODE{node_id}", f"CPUs {cpus}"],
            kind="CPU",
            depth=0,
        )
        cpu_node.children = [
            build_host_node(root, 1) for root in sorted(roots_by_numa[node_id])
        ]
        forest.append(cpu_node)

    if unknown_roots:
        cpu_node = SchematicNode(
            key="cpu-node-unknown",
            lines=["CPU/SoC NODE?", "NUMA unknown"],
            kind="CPU",
            depth=0,
        )
        cpu_node.children = [build_host_node(root, 1) for root in sorted(unknown_roots)]
        forest.append(cpu_node)

    return forest


def flatten_schematic_nodes(forest):
    rows = []

    def visit(node):
        rows.append(node)
        for child in node.children:
            visit(child)

    for root in forest:
        visit(root)

    return rows


def wrap_schematic_lines(lines, width):
    max_chars = max(18, int((width - 28) / 7.4))
    wrapped = []
    for line in lines:
        wrapped.extend(textwrap.wrap(line, width=max_chars) or [""])
    return wrapped


def layout_schematic_nodes(forest):
    """
    Assigns node boxes to a left-to-right tree layout.
    """
    rows = flatten_schematic_nodes(forest)
    margin_x = 40
    margin_y = 88
    x_step = 540
    line_height = 18
    vertical_padding = 24
    leaf_step = 140

    for node in rows:
        node.width = schematic_node_width(node.kind)
        node.render_lines = wrap_schematic_lines(node.lines, node.width)
        node.height = max(64, vertical_padding + line_height * len(node.render_lines))
        node.x = margin_x + node.depth * x_step

    next_leaf = 0

    def assign_y(node):
        nonlocal next_leaf
        if not node.children:
            node.y = margin_y + next_leaf * leaf_step
            next_leaf += 1
            return node.y + node.height / 2

        child_centers = [assign_y(child) for child in node.children]
        center = (min(child_centers) + max(child_centers)) / 2
        node.y = int(center - node.height / 2)
        return center

    for root in forest:
        assign_y(root)

    width = max((node.x + node.width for node in rows), default=640) + margin_x
    height = max((node.y + node.height for node in rows), default=360) + 40
    return rows, width, height


def schematic_edges(forest):
    edges = []

    def visit(parent):
        for child in parent.children:
            edges.append((parent, child))
            visit(child)

    for root in forest:
        visit(root)

    return edges


def schematic_colors(node):
    colors = {
        "CPU": ("#dcecff", "#5c7fa6"),
        "HOST": ("#eceff4", "#8a95a5"),
        "BRIDGE": ("#f4f0e6", "#b88a35"),
        "GPU": ("#e5f6de", "#5f9d3b"),
        "NIC": ("#e2f4f7", "#3b91a3"),
        "NVME": ("#efe7fb", "#8b65b7"),
        "DISK": ("#efe7fb", "#8b65b7"),
    }
    fill, outline = colors.get(node.kind, ("#f7f7f7", "#aaaaaa"))
    if any("peer fabric" in line for line in node.lines):
        return "#f0fdf4", "#16a34a"
    return fill, outline


def split_label_lines(label):
    return [line for line in (label or "").splitlines() if line]


def edge_label_geometry(parent, child, label_lines):
    """
    Places edge labels in the left side of the inter-box gap so multi-line labels
    do not run underneath the destination box.
    """
    x1 = parent.x + parent.width
    x2 = child.x
    y2 = child.y + child.height / 2
    label_width = 156
    label_height = 10 + len(label_lines) * 14
    gap = x2 - x1

    if gap > label_width + 32:
        label_x = x1 + max(12, (gap * 0.28) - label_width / 2)
        label_x = min(label_x, x2 - label_width - 16)
    else:
        label_x = x1 + 8

    label_y = y2 - label_height / 2
    return label_x, label_y, label_width, label_height


def draw_pil_multiline_center(draw, lines, x, y, width, font, fill):
    line_height = 14
    for index, line in enumerate(lines):
        bbox = draw.textbbox((0, 0), line, font=font)
        text_width = bbox[2] - bbox[0]
        draw.text((x + (width - text_width) / 2, y + index * line_height), line, font=font, fill=fill)


def write_schematic_png(output_path, forest, width, height):
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return False

    image = Image.new("RGB", (width, height), "#ffffff")
    draw = ImageDraw.Draw(image)

    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13)
        bold_font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 15)
        small_font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
        title_font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 22)
    except OSError:
        font = ImageFont.load_default()
        bold_font = font
        small_font = font
        title_font = font

    draw.text((40, 28), "PCIe Topology Schematic", font=title_font, fill="#1f2937")
    draw.text(
        (40, 56),
        "Root complexes show PCIe enumeration; highlighted switch boxes show PIX/PXB peer fabrics.",
        font=small_font,
        fill="#4b5563",
    )

    for parent, child in schematic_edges(forest):
        x1 = parent.x + parent.width
        y1 = parent.y + parent.height / 2
        x2 = child.x
        y2 = child.y + child.height / 2
        mid_x = (x1 + x2) / 2
        draw.line([(x1, y1), (mid_x, y1), (mid_x, y2), (x2, y2)], fill="#6b7280", width=2)

        label_lines = split_label_lines(child.link_label)
        if label_lines:
            label_x, label_y, label_width, label_height = edge_label_geometry(
                parent, child, label_lines
            )
            draw.rounded_rectangle(
                (label_x, label_y, label_x + label_width, label_y + label_height),
                radius=6,
                fill="#ffffff",
                outline="#d1d5db",
            )
            draw_pil_multiline_center(
                draw, label_lines, label_x, label_y + 5, label_width, small_font, "#374151"
            )

    for node in flatten_schematic_nodes(forest):
        fill, outline = schematic_colors(node)
        draw.rounded_rectangle(
            (node.x, node.y, node.x + node.width, node.y + node.height),
            radius=8,
            fill=fill,
            outline=outline,
            width=2,
        )
        text_y = node.y + 12
        for index, line in enumerate(node.render_lines):
            current_font = bold_font if index == 0 else font
            draw.text((node.x + 14, text_y), line, font=current_font, fill="#111827")
            text_y += 19 if index == 0 else 17

    image.save(output_path)
    return True


def svg_text(x, y, text, size=13, weight="400", fill="#111827"):
    return (
        f'<text x="{x}" y="{y}" font-family="DejaVu Sans, Arial, sans-serif" '
        f'font-size="{size}" font-weight="{weight}" fill="{fill}">'
        f"{html.escape(text)}</text>"
    )


def write_schematic_svg(output_path, forest, width, height):
    elements = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        svg_text(40, 45, "PCIe Topology Schematic", size=22, weight="700", fill="#1f2937"),
        svg_text(
            40,
            68,
            "Root complexes show PCIe enumeration; highlighted switch boxes show PIX/PXB peer fabrics.",
            size=11,
            fill="#4b5563",
        ),
    ]

    for parent, child in schematic_edges(forest):
        x1 = parent.x + parent.width
        y1 = parent.y + parent.height / 2
        x2 = child.x
        y2 = child.y + child.height / 2
        mid_x = (x1 + x2) / 2
        points = f"{x1},{y1} {mid_x},{y1} {mid_x},{y2} {x2},{y2}"
        elements.append(
            f'<polyline points="{points}" fill="none" stroke="#6b7280" stroke-width="2"/>'
        )

        label_lines = split_label_lines(child.link_label)
        if label_lines:
            label_x, label_y, label_width, label_height = edge_label_geometry(
                parent, child, label_lines
            )
            elements.append(
                f'<rect x="{label_x}" y="{label_y}" width="{label_width}" '
                f'height="{label_height}" rx="6" fill="#ffffff" stroke="#d1d5db"/>'
            )
            for index, line in enumerate(label_lines):
                elements.append(
                    svg_text(
                        label_x + label_width / 2,
                        label_y + 16 + index * 14,
                        line,
                        size=11,
                        fill="#374151",
                    ).replace("<text ", '<text text-anchor="middle" ')
                )

    for node in flatten_schematic_nodes(forest):
        fill, outline = schematic_colors(node)
        elements.append(
            f'<rect x="{node.x}" y="{node.y}" width="{node.width}" height="{node.height}" '
            f'rx="8" fill="{fill}" stroke="{outline}" stroke-width="2"/>'
        )
        text_y = node.y + 25
        for index, line in enumerate(node.render_lines):
            elements.append(
                svg_text(
                    node.x + 14,
                    text_y,
                    line,
                    size=15 if index == 0 else 13,
                    weight="700" if index == 0 else "400",
                )
            )
            text_y += 19 if index == 0 else 17

    elements.append("</svg>")
    with open(output_path, "w") as file:
        file.write("\n".join(elements))


def write_schematic_image(output_path, forest):
    rows, width, height = layout_schematic_nodes(forest)
    if not rows:
        logging.warning("No PCIe devices found for schematic output.")
        return None

    output_path = os.path.abspath(output_path)
    output_dir = os.path.dirname(output_path)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    extension = os.path.splitext(output_path)[1].lower()
    if extension == ".svg":
        write_schematic_svg(output_path, forest, width, height)
        return output_path

    if extension != ".png":
        logging.warning(
            f"Unsupported schematic extension '{extension}'. Writing SVG instead."
        )
        output_path = os.path.splitext(output_path)[0] + ".svg"
        write_schematic_svg(output_path, forest, width, height)
        return output_path

    if write_schematic_png(output_path, forest, width, height):
        return output_path

    fallback_path = os.path.splitext(output_path)[0] + ".svg"
    logging.warning("Pillow is not installed; writing SVG schematic instead of PNG.")
    write_schematic_svg(fallback_path, forest, width, height)
    return fallback_path


def check_topology_schematic(output_path="pcie_schematic.png"):
    """
    Writes a pictorial PCIe topology schematic and classifies GPU-to-I/O paths.
    """
    devices = collect_pci_devices()
    cpu_nodes = collect_cpu_nodes()
    root_numa_nodes = infer_root_numa_nodes(devices, cpu_nodes)
    annotate_optimal_peer_fabrics(devices, root_numa_nodes)
    forest = build_schematic_forest(devices, cpu_nodes, root_numa_nodes)
    schematic_path = write_schematic_image(output_path, forest)
    if schematic_path:
        logging.info(f"Wrote PCIe topology schematic to {schematic_path}")

    print("\n".join(render_topology_connection_summary(devices, root_numa_nodes)))


def check_topology_connections():
    """
    Executes `nvidia-smi topo -m`, parses its output, and ensures that every GPU has at least one PIX or PXB connection to a NIC.
    If not, logs an error specifying the GPU, NIC, and the actual connection type.
    """
    if is_any_integrated_gpu():
        logging.info(
            "Skipping PIX/PXB topology requirement: integrated GPU detected. "
            "On single-SoC unified-memory platforms (e.g. GB10 / DGX Spark) there is "
            "no separable PCIe path GPU↔NIC to optimize."
        )
        return
    try:
        # Run nvidia-smi topo -m to get topology information
        result = subprocess.run(
            ["nvidia-smi", "topo", "-m"], capture_output=True, text=True, check=True
        )

        # Parse the output of nvidia-smi topo -m
        topo_output = result.stdout.splitlines()

        ansi_escape = re.compile(r'\x1b\[[0-9;]*m')

        # Find the header line (contains GPU/NIC labels)
        header_index = 0

        if header_index is None:
            logging.error("Could not find topology table in the nvidia-smi output.")
            return

        # Extract labels (e.g., GPU0, NIC0, etc.), stripping ANSI codes first
        header = ansi_escape.sub('', topo_output[header_index]).strip()
        labels = []
        for label in header.split():
            if re.match(r'^(GPU|NIC)\d+$', label):
                labels.append(label)

        # Parse the topology table rows
        gpu_to_nic_connections = {}
        for row_idx, row in enumerate(topo_output[header_index + 1 :]):
            row = ansi_escape.sub('', row).strip()
            if not row:
                continue  # Skip empty lines

            # Split row into columns
            columns = row.split()
            device_label = columns[0]  # First column is the device label (e.g., GPU0)

            # Check connections for GPUs only
            if "GPU" in device_label:
                # We need to align the columns with the labels
                # The first column after the device label corresponds to the first label
                for label_idx, label in enumerate(labels):
                    if "NIC" in label:
                        # label_idx + 1 because columns[0] is the device label
                        connection_type = columns[label_idx + 1]

                        if device_label not in gpu_to_nic_connections:
                            gpu_to_nic_connections[device_label] = []
                        gpu_to_nic_connections[device_label].append((label, connection_type))

        # Verify that each GPU has at least one PIX or PXB connection to a NIC
        for gpu, connections in gpu_to_nic_connections.items():
            has_valid_connection = False
            for nic, connection_type in connections:
                if connection_type in {"PIX", "PXB"}:
                    logging.info(f"{gpu} has a {connection_type} connection to {nic}")
                    has_valid_connection = True
                    break

            if not has_valid_connection:
                for nic, connection_type in connections:
                    logging.warning(
                        f"{gpu} does not have a PIX or PXB connection to {nic}. "
                        f"Current connection type: {connection_type}."
                    )

    except FileNotFoundError:
        logging.error("nvidia-smi command not found. Ensure NVIDIA drivers are installed.")
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while executing nvidia-smi topo -m: {e}")
    except Exception as e:
        logging.error(f"An unexpected error occurred: {e}")


def check_kernel_cmdline():
    """
    Checks if the words "isolcpus", "rcu_nocbs", and "irqaffinity" appear in /proc/cmdline.
    Logs a separate warning for each word that is missing.
    """
    try:
        # Read the contents of /proc/cmdline
        with open("/proc/cmdline", "r") as file:
            cmdline = file.read().strip()

        # Check for each required word
        required_keywords = ["isolcpus", "rcu_nocbs", "irqaffinity"]
        for keyword in required_keywords:
            if keyword not in cmdline:
                logging.warning(
                    f"The kernel command line is missing '{keyword}'. Please ensure it is configured."
                )
            else:
                logging.info(f"{keyword} found in kernel boot line")

    except FileNotFoundError:
        logging.error("/proc/cmdline not found. Are you sure you're running on Linux?")
    except Exception as e:
        logging.error(f"An unexpected error occurred while checking /proc/cmdline: {e}")


def check_mtu_size():
    """
    Checks the MTU size of each NVIDIA NIC using the sysfs interface and prints a warning if it's not over 1500 bytes.
    """
    try:
        nic_info = get_nic_info()
        for intf in nic_info:
            iface = intf[0]

            # Check MTU size for each NVIDIA NIC using sysfs
            mtu_path = f"/sys/class/net/{iface}/mtu"
            if os.path.exists(mtu_path):
                with open(mtu_path, "r") as f:
                    mtu_value = int(f.read().strip())
                    if mtu_value <= 1518:
                        logging.warning(
                            f"Interface {iface} has an MTU of {mtu_value} bytes. "
                            "If possible use larger frame sizes ( > 1518B) for better performance"
                        )
                    else:
                        logging.info(
                            f"Interface {iface} has an acceptable MTU of {mtu_value} bytes."
                        )
            else:
                logging.error(f"MTU file for interface {iface} does not exist.")

    except FileNotFoundError:
        logging.error(
            "The ibdev2netdev command is not found. Ensure that it is installed and available in your PATH."
        )
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while executing a command: {e}")
    except Exception as e:
        logging.error(f"An unexpected error occurred: {e}")


def update_mrrs_for_nvidia_devices():
    """
    Updates the PCIe Maximum Read Request Size (MRRS) to 4096 for all Mellanox devices,
    preserving the lower 12 bits of the current setting. Reads back after the write
    so a silently-failing setpci (e.g. Secure Boot lockdown) is reported as an error
    rather than misreported as a success.
    """
    try:
        nic_info = get_nic_info()
        for intf in nic_info:
            pci_address = intf[1]

            try:
                # Read the current MRRS value
                read_result = subprocess.run(
                    ["setpci", "-s", pci_address, "68.w"],
                    capture_output=True,
                    text=True,
                    check=True,
                )

                current_value_hex = read_result.stdout.strip()
                current_value = int(current_value_hex, 16)

                # Calculate new value: keep lower 12 bits, set upper 4 bits to 5 (for 4096 bytes)
                new_value = (current_value & 0x0FFF) | (0x5 << 12)

                # Write the new MRRS value back
                subprocess.run(["setpci", "-s", pci_address, f"68.w={new_value:04x}"], check=True)

                # Read back to verify the write actually landed.
                verify_result = subprocess.run(
                    ["setpci", "-s", pci_address, "68.w"],
                    capture_output=True,
                    text=True,
                    check=True,
                )
                verified_value = int(verify_result.stdout.strip(), 16)
                if (verified_value & 0xF000) >> 12 == 5:
                    logging.info(
                        f"Successfully updated MRRS to 4096 for device at PCIe address {pci_address}={hex(verified_value)}."
                    )
                else:
                    logging.error(
                        f"MRRS write to {pci_address} did not take effect (read back {hex(verified_value)}). "
                        "Most common cause: kernel lockdown from Secure Boot blocks setpci writes "
                        "silently. Disable Secure Boot in firmware and retry."
                    )
            except subprocess.CalledProcessError as e:
                logging.error(
                    f"Failed to update MRRS for device at PCIe address {pci_address}: {e}"
                )

    except FileNotFoundError:
        logging.error(
            "The ibdev2netdev or setpci command is not found. Ensure that they are installed and available in your PATH."
        )
    except subprocess.CalledProcessError as e:
        logging.error(f"Error while executing a command: {e}")
    except Exception as e:
        logging.error(f"An unexpected error occurred: {e}")


def main():
    setup_logging()
    args = parse_args()

    if args.check is not None:
        if args.check == "all" or args.check == "cpu-freq":
            check_cpu_governor()
        if args.check == "all" or args.check == "mrrs":
            check_mrrs()
        if args.check == "all" or args.check == "mps":
            check_max_payload_size()
        if args.check == "all" or args.check == "hugepages":
            check_hugepages()
        if args.check == "all" or args.check == "gpu-clocks":
            check_nvidia_gpu_clocks()
        if args.check == "all" or args.check == "bar1-size":
            check_bar1_size()
        if args.check == "all" or args.check == "topo":
            check_topology_connections()
        if args.check == "all" or args.check == "schematic":
            check_topology_schematic(args.schematic_output)
        if args.check == "all" or args.check == "cmdline":
            check_kernel_cmdline()
        if args.check == "all" or args.check == "mtu":
            check_mtu_size()
        if args.check == "all" or args.check == "gpudirect":
            check_gpudirect_support()
        if args.check == "all" or args.check == "peermem":
            check_peermem_kernel()
    elif args.set is not None:
        if args.set == "mrrs":
            update_mrrs_for_nvidia_devices()


if __name__ == "__main__":
    if os.geteuid() != 0 and not any(arg in {"-h", "--help"} for arg in sys.argv[1:]):
        sys.exit("This script must be run as root! Please use 'sudo' to execute it.")

    main()
