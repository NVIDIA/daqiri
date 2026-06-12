---
hide:
  - navigation
---

# System Configuration

DAQIRI requires an [**NVIDIA SmartNIC**](https://www.nvidia.com/en-us/networking/ethernet-adapters/) (ConnectX-6 Dx or later) and a CUDA-capable GPU. Two reference platforms are documented in this tutorial — pick the one closest to yours below:

- **IGX Orin** with a discrete GPU (e.g. [RTX 6000 Ada](https://www.nvidia.com/en-us/design-visualization/rtx-6000/)): peermem-based GPUDirect, a separate GPU BAR1, and a discrete-PCIe path between GPU and NIC. The originally-supported reference platform.
- **DGX Spark** (Grace Blackwell **GB10** superchip): unified CPU/GPU memory via NVLink-C2C, integrated **ConnectX-7**, no peermem, and GPUDirect via `kind: host_pinned` data buffers.

<div class="platform-tabs" markdown="1">

=== "IGX Orin"

    This tab covers both the **required system setup** to get DAQIRI running on IGX Orin and **optional performance tuning** to maximize throughput and minimize latency. Complete the [System Setup for DAQIRI](#system-setup-for-daqiri) section first, then move on to [System Optimization](#system-optimization) as needed.

    ## System Setup for DAQIRI

    This section covers the essential system setup steps needed before using DAQIRI. Complete this setup before moving on to [System Optimization](#system-optimization) or [running benchmarks](../benchmarks/benchmarks.md).

    In this tutorial, we will be developing on an **NVIDIA IGX Orin platform** with [IGX SW 1.1](https://docs.nvidia.com/igx-orin/user-guide/latest/base-os.html) and an [NVIDIA RTX 6000 ADA GPU](https://www.nvidia.com/en-us/design-visualization/rtx-6000/), which is the configuration that is currently actively tested. The concepts should be applicable to other systems based on Ubuntu 22.04 as well. It should also work on other Linux distributions with a glibc version of 2.35 or higher by containerizing the dependencies and applications on top of an Ubuntu 22.04 image, but this is not actively tested at this time.

    !!! Warning "Secure boot conflict"

        If you have secure boot enabled on your system, you might need to disable it as a prerequisite to run some of the configurations below ([switching the NIC link layers to Ethernet](#switch-your-nic-link-layers-to-ethernet), [updating the MRRS of your NIC ports](#step-3-maximize-the-nics-max-read-request-size-mrrs), [updating the BAR1 size of your GPU](#step-8-maximize-gpu-bar1-size)). Secure boot can be re-enabled after the configurations are completed.

    ### Check your NIC drivers

    Ensure your NIC drivers are loaded:

    ```bash
    lsmod | grep ib_core
    ```

    ??? abstract "See an example output"

        This would be an expected output, where `ib_core` is listed on the left.

        ```bash
        ib_core               442368  8 rdma_cm,ib_ipoib,iw_cm,ib_umad,rdma_ucm,ib_uverbs,mlx5_ib,ib_cm
        mlx_compat             20480  11 rdma_cm,ib_ipoib,mlxdevm,iw_cm,ib_umad,ib_core,rdma_ucm,ib_uverbs,mlx5_ib,ib_cm,mlx5_core
        ```

    If this is empty, install the latest OFED drivers from DOCA (the DOCA APT repository should already be configured from the [DAQIRI build setup](../getting-started.md#build-the-daqiri-library)), and reboot your system:

    ```bash
    sudo apt update
    sudo apt install doca-ofed
    sudo reboot
    ```

    !!! note

        If this is not empty, you can still install the newest OFED drivers from `doca-ofed` above. If you choose to keep your current drivers, install the following utilities for convenience later on. They include tools like `ibstat`, `ibv_devinfo`, `ibdev2netdev`, `mlxconfig`:

        ```bash
        sudo apt update
        sudo apt install infiniband-diags ibverbs-utils mlnx-ofed-kernel-utils mft
        ```

        Also upgrade the user space libraries to make sure your tools have all the symbols they need:

        ```bash
        sudo apt install libibverbs1 librdmacm1 rdma-core
        ```

    Running `ibstat` or `ibv_devinfo` will confirm your NIC interfaces are recognized by your drivers.

    ### Switch your NIC Link Layers to Ethernet

    NVIDIA SmartNICs can function in two separate modes (called link layer):

    - Ethernet (ETH)
    - Infiniband (IB)

    To identify the current mode, run `ibstat` or `ibv_devinfo` and look for the `Link Layer` value.

    ```bash
    ibv_devinfo
    ```

    ??? note "Warning about `libvmw_pvrdma-rdmav34.so`"

        If you see a warning like `couldn't load driver 'libvmw_pvrdma-rdmav34.so'`, this is harmless. It refers to a VMware paravirtual RDMA driver that is not relevant on bare-metal systems and can be safely ignored.

    ??? failure "Couldn't load driver 'libmlx5-rdmav34.so'"

        If you see an error like this, you might have different versions for your OFED tools and libraries. Attempt after upgrading your user space libraries to match the version of your OFED tools like so:

        ```bash
        sudo apt update
        sudo apt install libibverbs1 librdmacm1 rdma-core
        ```

    ??? abstract "See an example output"

        In the example below, the `mlx5_0` interface is in Ethernet mode, while the `mlx5_1` interface is in Infiniband mode. Do not pay attention to the `transport` value which is always `InfiniBand`.

        ```sh hl_lines="18 37"
        hca_id: mlx5_0
                transport:                      InfiniBand (0)
                fw_ver:                         28.38.1002
                node_guid:                      48b0:2d03:00f4:07fb
                sys_image_guid:                 48b0:2d03:00f4:07fb
                vendor_id:                      0x02c9
                vendor_part_id:                 4129
                hw_ver:                         0x0
                board_id:                       NVD0000000033
                phys_port_cnt:                  1
                        port:   1
                                state:                  PORT_ACTIVE (4)
                                max_mtu:                4096 (5)
                                active_mtu:             4096 (5)
                                sm_lid:                 0
                                port_lid:               0
                                port_lmc:               0x00
                                link_layer:             Ethernet

        hca_id: mlx5_1
                transport:                      InfiniBand (0)
                fw_ver:                         28.38.1002
                node_guid:                      48b0:2d03:00f4:07fc
                sys_image_guid:                 48b0:2d03:00f4:07fb
                vendor_id:                      0x02c9
                vendor_part_id:                 4129
                hw_ver:                         0x0
                board_id:                       NVD0000000033
                phys_port_cnt:                  1
                        port:   1
                                state:                  PORT_ACTIVE (4)
                                max_mtu:                4096 (5)
                                active_mtu:             4096 (5)
                                sm_lid:                 0
                                port_lid:               0
                                port_lmc:               0x00
                                link_layer:             InfiniBand
        ```

    **For DAQIRI, we want the NIC to use the ETH link layer.** To switch the link layer mode, there are two possible options:

    1. On IGX Orin developer kits, you can switch that setting through the BIOS: [see IGX Orin documentation](https://docs.nvidia.com/igx-orin/user-guide/latest/switch-network-link.html).
    2. On any system with a NVIDIA NIC (including the IGX Orin developer kits), you can run the commands below from a terminal:

        1. Identify the PCI address of your NVIDIA NIC

            === "ibdev2netdev"

                ```bash
                nic_pci=$(sudo ibdev2netdev -v | awk '{print $1}' | head -n1)
                ```

            === "lspci"

                ```bash
                # `0200` is the PCI-SIG class code for Ethernet controllers
                # `0207` is the PCI-SIG class code for Infiniband controllers
                # `15b3` is the Vendor ID for Mellanox
                nic_pci=$(lspci -n | awk '($2 == "0200:" || $2 == "0207:") && $3 ~ /^15b3:/ {print $1; exit}')
                ```

        2. Set both link layers to Ethernet. `LINK_TYPE_P1` and `LINK_TYPE_P2` are for `mlx5_0` and `mlx5_1` respectively. You can choose to only set one of them. `ETH` or `2` is Ethernet mode, and `IB` or `1` is for InfiniBand.

            ```bash
            sudo mlxconfig -d $nic_pci set LINK_TYPE_P1=ETH LINK_TYPE_P2=ETH
            ```

            Apply with `y`.

            ??? abstract "See an example output"

                ```sh
                Device #1:
                ----------

                Device type:    ConnectX7
                Name:           P3740-B0-QSFP_Ax
                Description:    NVIDIA Prometheus P3740 ConnectX-7 VPI PCIe Switch Motherboard; 400Gb/s; dual-port QSFP; PCIe switch5.0 X8 SLOT0 ;X16 SLOT2; secure boot;
                Device:         0005:03:00.0

                Configurations:                                      Next Boot       New
                        LINK_TYPE_P1                                ETH(2)          ETH(2)
                        LINK_TYPE_P2                                IB(1)           ETH(2)

                Apply new Configuration? (y/n) [n] :
                y

                Applying... Done!
                -I- Please reboot machine to load new configurations.
                ```

                - `Next Boot` is the current value that was expected to be used at the next reboot.
                - `New` is the value you're about to set to override `Next Boot`.

            ??? failure "ERROR: write counter to semaphore: Operation not permitted"

                Disable secure boot on your system ahead of changing the link type of your NIC ports. It can be re-enabled afterwards.

        3. Reboot your system.

            ```bash
            sudo reboot
            ```

    ### Configure the IP addresses of the NIC ports

    First, we want to identify the logical names of your NIC interfaces. Connecting an SFP cable in just one of the ports of the NIC will help you identify which port is which. Run the following command once the cable is in place:

    ```bash
    ibdev2netdev
    ```

    ??? abstract "See an example output"

        In the example below, only `mlx5_1` has a cable connected (`Up`), and its logical ethernet name is `eth1`:

        ```bash
        $ ibdev2netdev
        mlx5_0 port 1 ==> eth0 (Down)
        mlx5_1 port 1 ==> eth1 (Up)
        ```

    ??? failure "ibdev2netdev does not show the NIC"

        If you have a cable connected but it does not show Up/Down in the output of `ibdev2netdev`, you can try to parse the output of `dmesg` instead. The example below shows that `0005:03:00.1` is plugged, and that it is associated with `eth1`:

        ```sh
        $ sudo dmesg | grep -w mlx5_core
        ...
        [   11.512808] mlx5_core 0005:03:00.0 eth0: Link down
        [   11.640670] mlx5_core 0005:03:00.1 eth1: Link down
        ...
        [ 3712.267103] mlx5_core 0005:03:00.1: Port module event: module 1, Cable plugged
        ```

    The next step is to set a static IP on the interface you'd like to use so you can refer to it in your DAQIRI applications. First, check if you already have any addresses configured using the ethernet interface names identified above (in our case, `eth0` and `eth1`):

    ```bash
    ip -f inet addr show eth0
    ip -f inet addr show eth1
    ```

    If nothing appears, or you'd like to change the address, you can set an IP address through the Network Manager user interface, CLI (`nmcli`), or other IP configuration tools. In the example below, we configure the `eth0` interface with an address of `1.1.1.1/24`, and the `eth1` interface with an address of `2.2.2.2/24`.

    === "One-time"

        ```bash
        sudo ip addr add 1.1.1.1/24 dev eth0
        sudo ip addr add 2.2.2.2/24 dev eth1
        ```

    === "Persistent"

        Set these variables to your desired values:

        ```bash
        if_name=eth0
        if_static_ip=1.1.1.1/24
        ```

        === "NetworkManager"

            Update the IP with `nmcli`:

            ```bash
            sudo nmcli connection modify $if_name ipv4.addresses $if_static_ip
            sudo nmcli connection up $if_name
            ```

        === "systemd-networkd"

            Create a network config file with the static IP:

            ```bash
            cat << EOF | sudo tee /etc/systemd/network/20-$if_name.network
            [Match]
            MACAddress=$(cat /sys/class/net/$if_name/address)

            [Network]
            Address=$if_static_ip
            EOF
            ```

            Apply now:

            ```bash
            sudo systemctl restart systemd-networkd
            ```

    !!! note

        If you are connecting the NIC to another NIC with an [interconnect](https://www.nvidia.com/en-us/networking/interconnect/), do the same on the other system with an IP address on the same network segment.
        For example, to communicate with `1.1.1.1/24` above (`/24` -> `255.255.255.0` submask), setup your other system with an IP between `1.1.1.2` and `1.1.1.254`, and the same `/24` submask.

    ### Enable GPUDirect

    Assuming you already have [NVIDIA drivers](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/index.html#ubuntu-installation-network) installed, check if the `nvidia_peermem` kernel module is loaded:

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check topo
        ```

        ??? abstract "See an example output"

            ```log
            2025-03-12 14:15:07 - INFO - GPU 0: NVIDIA RTX A6000 has GPUDirect support.
            2025-03-12 14:15:27 - INFO - nvidia-peermem module is loaded.
            ```

    ```bash
    lsmod | grep nvidia_peermem
    ```

    If it's not loaded, run the following command, then check again:

    === "One-time"

        ```bash
        sudo modprobe nvidia_peermem
        ```

    === "Persistent"

        ```bash
        sudo echo "nvidia-peermem" >> /etc/modules
        sudo systemctl restart systemd-modules-load.service
        ```

    ??? failure "Error loading the `nvidia-peermem` kernel module"

        If you run into an error loading the `nvidia-peermem` kernel module, follow these steps:

        1. Install the `doca-ofed` package to get the latest drivers for your NIC as [documented above](#check-your-nic-drivers).
        2. Restart your system.
        3. Rebuild your NVIDIA drivers with DKMS like so:

        ```bash
        peermem_ko=$(find /lib/modules/$(uname -r) -name "*peermem*.ko")
        nv_dkms=$(dpkg -S "$peermem_ko" | cut -d: -f1)
        sudo dpkg-reconfigure $nv_dkms
        sudo modprobe nvidia_peermem
        ```

    ??? info "What about dma-buf?"

        DAQIRI supports GPUDirect setups based on either `nvidia-peermem` or dma-buf.
        The right interface depends on your kernel, NVIDIA driver, NIC driver, and
        deployment environment. The container build uses a dmabuf-patched DPDK path,
        while some host builds and driver stacks still use `nvidia-peermem`.

    ---

    ## System Optimization

    !!! warning "Advanced"

        The section below is for advanced users looking to extract more performance out of their system. You can choose to skip this section and return to it later if performance if your application is not satisfactory.

    While the configurations above are the minimum requirements to get a NIC and a NVIDIA GPU to communicate while bypassing the OS kernel stack, performance can be further improved in most scenarios by tuning the system as described below.

    The table below summarizes all optimization steps covered in this section, along with the corresponding `tune_system.py` flags and whether each setting can be made persistent across reboots. Use it as a checklist to track your progress.

    | Step | Description | Tuning Script Flag | Persistent Option Available? |
    |------|-------------|--------------------|-------------|
    | 1 | [PCIe topology](#step-1-ensure-ideal-pcie-topology) | `--check topo` | N/A (hardware) |
    | 2 | [PCIe config (MPS/Speed)](#step-2-check-the-nics-pcie-configuration) | `--check mps` | N/A (hardware) |
    | 3 | [NIC MRRS](#step-3-maximize-the-nics-max-read-request-size-mrrs) | `--check mrrs` / `--set mrrs` | No — use a startup script |
    | 4 | [Hugepages](#step-4-enable-huge-pages) | `--check hugepages` | Yes — kernel bootline or `/etc/fstab` |
    | 5 | [CPU isolation](#step-5-isolate-cpu-cores) | `--check cmdline` | Yes — kernel bootline |
    | 6 | [CPU governor](#step-6-prevent-cpu-cores-from-going-idle) | `--check cpu-freq` | Yes — see persistent option in section |
    | 7 | [GPU clocks](#step-7-prevent-the-gpu-from-going-idle) | `--check gpu-clock` | Partial — `nvidia-smi -pm 1` persists driver; clock locks need a startup script |
    | 8 | [GPU BAR1 size](#step-8-maximize-gpu-bar1-size) | `--check bar1-size` | Yes — firmware flash |
    | 9 | [Jumbo frames (MTU)](#step-9-enable-jumbo-frames) | `--check mtu` | Yes — see persistent option in section |

    !!! tip "Plan your reboots"

        Several steps below require adding flags to the kernel bootline in `/etc/default/grub` (hugepages in [Enable Huge pages](#step-4-enable-huge-pages), CPU isolation in [Isolate CPU cores](#step-5-isolate-cpu-cores)). We recommend reading through both sections first and adding all the flags at once to avoid multiple reboots. Other items like MRRS, GPU clocks, and MTU can be applied at runtime but reset on reboot — consider scripting them or using a systemd service for persistence.

    Before diving in each of the setups below, we provide a utility script as part of the DAQIRI library which provides an overview of the configurations that potentially need to be tuned on your system. The script (`python/tune_system.py`) is run on the host from a clone of the DAQIRI repo — it touches host PCIe/sysfs and is not intended to run inside the build container.

    ??? example "Work In Progress"

        This utility script is under active development and will be updated in future releases with additional checks, more actionable recommendations, and automated tuning.

    ```bash
    sudo ./python/tune_system.py --check all
    ```

    ??? abstract "See an example output"

        Our tuned-up IGX system with A6000 can optimize most settings:

        ```log
        2025-03-12 14:16:06 - INFO - CPU 0: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 1: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 2: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 3: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 4: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 5: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 6: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 7: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 8: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 9: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 10: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - CPU 11: Governor is correctly set to 'performance'.
        2025-03-12 14:16:06 - INFO - cx7_0/0005:03:00.0: MRRS is correctly set to 4096.
        2025-03-12 14:16:06 - INFO - cx7_1/0005:03:00.1: MRRS is correctly set to 4096.
        2025-03-12 14:16:06 - WARNING - cx7_0/0005:03:00.0: PCIe Max Payload Size is not set to 256 bytes. Found: 128 bytes.
        2025-03-12 14:16:06 - WARNING - cx7_1/0005:03:00.1: PCIe Max Payload Size is not set to 256 bytes. Found: 128 bytes.
        2025-03-12 14:16:06 - INFO - HugePages_Total: 4
        2025-03-12 14:16:06 - INFO - HugePage Size: 1024.00 MB
        2025-03-12 14:16:06 - INFO - Total Allocated HugePage Memory: 4096.00 MB
        2025-03-12 14:16:06 - INFO - Hugepages are sufficiently allocated with at least 500 MB.
        2025-03-12 14:16:06 - INFO - GPU 0: SM Clock is correctly set to 1920 MHz (within 500 of the 2100 MHz theoretical Max).
        2025-03-12 14:16:06 - INFO - GPU 0: Memory Clock is correctly set to 8000 MHz.
        2025-03-12 14:16:06 - INFO - GPU 00000005:09:00.0: BAR1 size is 8192 MiB.
        2025-03-12 14:16:06 - INFO - GPU GPU0 has at least one PIX/PXB connection to a NIC
        2025-03-12 14:16:06 - INFO - isolcpus found in kernel boot line
        2025-03-12 14:16:06 - INFO - rcu_nocbs found in kernel boot line
        2025-03-12 14:16:06 - INFO - irqaffinity found in kernel boot line
        2025-03-12 14:16:06 - INFO - Interface cx7_0 has an acceptable MTU of 9000 bytes.
        2025-03-12 14:16:06 - INFO - Interface cx7_1 has an acceptable MTU of 9000 bytes.
        2025-03-12 14:16:06 - INFO - GPU 0: NVIDIA RTX A6000 has GPUDirect support.
        2025-03-12 14:16:06 - INFO - nvidia-peermem module is loaded.
        ```

    Based on the results, you can figure out which of the sections below are appropriate to update configurations on your system.

    ### Step 1: Ensure ideal PCIe topology

    Kernel bypass and GPUDirect rely on PCIe to communicate between the GPU and the NIC at high speeds. As-such, the topology of the PCIe tree on a system is critical to ensure optimal performance.

    Run the following command to check the GPUDirect communication matrix. **You are looking for a `PXB` or `PIX` connection between the GPU and the NIC interfaces to get the best performance.**

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check topo
        ```

        ??? abstract "See an example output"

            On IGX developer kits, the board's internal switch is designed to connect the GPU to the NIC interfaces with a `PXB` connection, offering great performance.

            ```log
            2025-03-06 12:07:45 - INFO - GPU GPU0 has at least one PIX/PXB connection to a NIC
            ```

    === "nvidia-smi"

        ```bash
        nvidia-smi topo -mp
        ```

        ??? abstract "See an example output"

            On IGX developer kits, the board's internal switch is designed to connect the GPU to the NIC interfaces with a `PXB` connection, offering great performance.
            ```
                    GPU0    NIC0    NIC1    CPU Affinity    NUMA Affinity   GPU NUMA ID
            GPU0     X      PXB     PXB     0-11    0               N/A
            NIC0    PXB      X      PIX
            NIC1    PXB     PIX      X

            Legend:

            X    = Self
            SYS  = Connection traversing PCIe as well as the SMP interconnect between NUMA nodes (e.g., QPI/UPI)
            NODE = Connection traversing PCIe as well as the interconnect between PCIe Host Bridges within a NUMA node
            PHB  = Connection traversing PCIe as well as a PCIe Host Bridge (typically the CPU)
            PXB  = Connection traversing multiple PCIe bridges (without traversing the PCIe Host Bridge)
            PIX  = Connection traversing at most a single PCIe bridge

            NIC Legend:

            NIC0: mlx5_0
            NIC1: mlx5_1
            ```

    If your connection is not optimal, you might be able to improve it by moving your NIC and/or GPU on a different PCIe port, so that they can share a branch and do not require going back to the Host Bridge (the CPU) to communicate. Refer to your system manufacturer for documentation, or run the following command to inspect the topology of your system:

    ```bash
    lspci -tv
    ```

    ??? abstract "See an example output"

        Here is the PCIe tree of an IGX system. Note how the ConnectX-7 and RTX A6000 are connected to the same branch.
        ``` hl_lines="2 3 5"
        -+-[0007:00]---00.0-[01-ff]----00.0  Marvell Technology Group Ltd. 88SE9235 PCIe 2.0 x2 4-port SATA 6 Gb/s Controller
        +-[0005:00]---00.0-[01-ff]----00.0-[02-09]--+-00.0-[03]--+-00.0  Mellanox Technologies MT2910 Family [ConnectX-7]
        |                                           |            \-00.1  Mellanox Technologies MT2910 Family [ConnectX-7]
        |                                           +-01.0-[04-06]----00.0-[05-06]----08.0-[06]--
        |                                           \-02.0-[07-09]----00.0-[08-09]----00.0-[09]--+-00.0  NVIDIA Corporation GA102GL [RTX A6000]
        |                                                                                        \-00.1  NVIDIA Corporation GA102 High Definition Audio Controller
        +-[0004:00]---00.0-[01-ff]----00.0  Sandisk Corp WD PC SN810 / Black SN850 NVMe SSD
        +-[0001:00]---00.0-[01-ff]----00.0-[02-fc]--+-01.0-[03-34]----00.0  Realtek Semiconductor Co., Ltd. RTL8111/8168/8411 PCI Express Gigabit Ethernet Controller
        |                                           +-02.0-[35-66]----00.0  Realtek Semiconductor Co., Ltd. RTL8111/8168/8411 PCI Express Gigabit Ethernet Controller
        |                                           +-03.0-[67-98]----00.0  Device 1c00:3450
        |                                           +-04.0-[99-ca]----00.0-[9a]--+-00.0  ASPEED Technology, Inc. ASPEED Graphics Family
        |                                           |                            \-02.0  ASPEED Technology, Inc. Device 2603
        |                                           \-05.0-[cb-fc]----00.0  Realtek Semiconductor Co., Ltd. RTL8822CE 802.11ac PCIe Wireless Network Adapter
        \-[0000:00]-
        ```

    !!! warning "x86_64 compatibility"

        Most x86_64 systems are not designed for this topology as they lack a discrete PCIe switch. In that case, the best connection they can achieve is `NODE`.

    ### Step 2: Check the NIC's PCIe configuration

    !!! quote "[Understanding PCIe Configuration for Maximum Performance - May 27, 2022](https://enterprise-support.nvidia.com/s/article/understanding-pcie-configuration-for-maximum-performance)"

        PCIe is used in any system for communication between different modules [including the NIC and the GPU]. This means that in order to process network traffic, the different devices communicating via the PCIe should be well configured. When connecting the network adapter to the PCIe, it auto-negotiates for the maximum capabilities supported between the network adapter and the CPU.

    The instructions below are meant to understand if your system is able to extract the maximum capabilities of your NIC, but they're not configurable. The two values that we are looking at here are the Max Payload Size (MPS - the maximum size of a PCIe packet) and the Speed (or PCIe generation).

    #### Max Payload Size (MPS)

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check mps
        ```

        ??? abstract "See an example output"

            The PCIe configuration on the IGX Orin developer kit is not able to leverage the max payload size of the NIC:

            ```log
            2025-03-10 16:15:54 - WARNING - cx7_0/0005:03:00.0: PCIe Max Payload Size is not set to 256 bytes. Found: 128 bytes.
            2025-03-10 16:15:54 - WARNING - cx7_1/0005:03:00.1: PCIe Max Payload Size is not set to 256 bytes. Found: 128 bytes.
            ```

    === "manual"

        Identify the PCIe address of your NVIDIA NIC:

        === "ibdev2netdev"

            ```bash
            nic_pci=$(sudo ibdev2netdev -v | awk '{print $1}' | head -n1)
            ```

        === "lspci"

            ```bash
            # `0200` is the PCI-SIG class code for NICs
            # `15b3` is the Vendor ID for Mellanox
            nic_pci=$(lspci -n | awk '$2 == "0200:" && $3 ~ /^15b3:/ {print $1}' | head -n1)
            ```

        Check current and max MPS:

        ```bash
        sudo lspci -vv -s $nic_pci | awk '/DevCap/{s=1} /DevCtl/{s=0} /MaxPayload /{match($0, /MaxPayload [0-9]+/, m); if(s){print "Max " m[0]} else{print "Current " m[0]}}'
        ```

        ??? abstract "See an example output"

            The PCIe configuration on the IGX Orin developer kit is not able to leverage the max payload size of the NIC:

            ```log
            Max MaxPayload 512
            Current MaxPayload 128
            ```

        !!! note

            While your NIC might be capable of more, 256 bytes is generally the largest supported by any switch/CPU at this time.

    ##### PCIe Speed/Generation

    Identify the PCIe address of your NVIDIA NIC:

    === "ibdev2netdev"

        ```bash
        nic_pci=$(sudo ibdev2netdev -v | awk '{print $1}' | head -n1)
        ```

    === "lspci"

        ```bash
        # `0200` is the PCI-SIG class code for NICs
        # `15b3` is the Vendor ID for Mellanox
        nic_pci=$(lspci -n | awk '$2 == "0200:" && $3 ~ /^15b3:/ {print $1}' | head -n1)
        ```

    Check current and max Speeds:

    ```bash
    sudo lspci -vv -s $nic_pci | awk '/LnkCap/{s=1} /LnkSta/{s=0} /Speed /{match($0, /Speed [0-9]+GT\/s/, m); if(s){print "Max " m[0]} else{print "Current " m[0]}}'
    ```

    ??? abstract "See an example output"

        On IGX, the switch is able to maximize the NIC speed, both being PCIe 5.0:

        ```log
        Max Speed 32GT/s
        Current Speed 32GT/s
        ```

    ### Step 3: Maximize the NIC's Max Read Request Size (MRRS)

    !!! quote "[Understanding PCIe Configuration for Maximum Performance - May 27, 2022](https://enterprise-support.nvidia.com/s/article/understanding-pcie-configuration-for-maximum-performance)"

        PCIe Max Read Request determines the maximal PCIe read request allowed. A PCIe device usually keeps track of the number of pending read requests due to having to prepare buffers for an incoming response. The size of the PCIe max read request may affect the number of pending requests (when using data fetch larger than the PCIe MTU).

    Unlike the PCIe properties queried in the previous section, the MRRS is configurable. **We recommend maxing it to 4096 bytes**. Run the following to check your current settings:

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check mrrs
        ```

    === "manual"

        Identify the PCIe address of your NVIDIA NIC:

        === "ibdev2netdev"

            ```bash
            nic_pci=$(sudo ibdev2netdev -v | awk '{print $1}' | head -n1)
            ```

        === "lspci"

            ```bash
            # `0200` is the PCI-SIG class code for NICs
            # `15b3` is the Vendor ID for Mellanox
            nic_pci=$(lspci -n | awk '$2 == "0200:" && $3 ~ /^15b3:/ {print $1}' | head -n1)
            ```

        Check current MRRS:

        ```bash
        sudo lspci -vv -s $nic_pci | grep DevCtl: -A2 | grep -oE "MaxReadReq [0-9]+"
        ```

    Update MRRS:

    ```bash
    sudo ./python/tune_system.py --set mrrs
    ```

    !!! note

        This value is reset on reboot and needs to be set every time the system boots

    ??? failure "ERROR: pcilib: sysfs_write: write failed: Operation not permitted"

        Disable secure boot on your system ahead of changing the MRRS of your NIC ports. It can be re-enabled afterwards.

    ### Step 4: Enable Huge pages

    Huge pages are a memory management feature that allows the OS to allocate large blocks of memory (typically 2MB or 1GB) instead of the default 4KB pages. This reduces the number of page table entries and the amount of memory used for translation, improving cache performance and reducing TLB (Translation Lookaside Buffer) misses, which leads to lower latencies.

    While it is naturally beneficial for CPU packets, it is also needed when routing data packets to the GPU in order to handle metadata (mbufs) on the CPU.

    === "hugeadm"

        We recommend installing the `libhugetlbfs-bin` package for the `hugeadm` utility:

        ```bash
        sudo apt update
        sudo apt install -y libhugetlbfs-bin
        ```

        Then, check your huge page pools:

        ```bash
        hugeadm --pool-list
        ```

        ??? abstract "See an example output"

            The example below shows that this system supports huge pages of 64K, 2M (default), 32M, and 1G, but that none of them are currently allocated.

            ```
                  Size  Minimum  Current  Maximum  Default
                 65536        0        0        0
               2097152        0        0        0        *
              33554432        0        0        0
            1073741824        0        0        0
            ```

        And your huge page mount points:

        ```bash
        hugeadm --list-all-mounts
        ```

        ??? abstract "See an example output"

            The default huge pages are mounted on `/dev/hugepages` with a page size of 2M.

            ```
            Mount Point          Options
            /dev/hugepages       rw,relatime,pagesize=2M
            ```

    === "vanilla"

        First, check your huge page pools:

        ```bash
        ls -1 /sys/kernel/mm/hugepages/
        grep Huge /proc/meminfo
        ```

        ??? abstract "See an example output"

            The example below shows that this system supports huge pages of 64K, 2M (default), 32M, and 1G, but that none of them are currently allocated.

            ```
            hugepages-1048576kB
            hugepages-2048kB
            hugepages-32768kB
            hugepages-64kB
            ```

            ```
            HugePages_Total:       0
            HugePages_Free:        0
            HugePages_Rsvd:        0
            HugePages_Surp:        0
            Hugepagesize:       2048 kB
            Hugetlb:               0 kB
            ```

        And your huge page mount points:

        ```bash
        mount | grep huge
        ```

        ??? abstract "See an example output"

            The default huge pages are mounted on `/dev/hugepages` with a page size of 2M.

            ```
            hugetlbfs on /dev/hugepages type hugetlbfs (rw,relatime,pagesize=2M)
            ```

    **As a rule of thumb, we recommend to start with 3 to 4 GB of total huge pages, with an individual page size of 500 MB to 1 GB** (per system availability).

    There are two ways to allocate huge pages:

    - in the kernel bootline (recommended to ensure contiguous memory allocation) or
    - dynamically at runtime (risk of fragmentation for large page sizes)

    The example below allocates 4 huge pages of 1GB each.

    === "Kernel bootline"

        Add the flags below to the `GRUB_CMDLINE_LINUX` variable in `/etc/default/grub`:

        ```bash
        default_hugepagesz=1G hugepagesz=1G hugepages=4
        ```

        ??? info "Show explanation"

            - `default_hugepagesz`: the default huge page size to use, making them available from the default mount point, `/dev/hugepages`.
            - `hugepagesz`: the size of the huge pages to allocate.
            - `hugepages`: the number of huge pages to allocate.

        Then rebuild your GRUB configuration and reboot:

        ```bash
        sudo update-grub
        sudo reboot
        ```

    === "Runtime"

        Allocate the 4x 1GB huge pages:

        === "hugeadm"

            ```bash
            sudo hugeadm --pool-pages-min 1073741824:4
            ```

        === "vanilla"

            ```bash
            echo 4 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
            ```

        Create a mount point to access the 1GB huge pages pool since that is not the default size on that system. We will name it `/mnt/huge` here.

        === "One-time"

            ```bash
            sudo mkdir -p /mnt/huge
            sudo mount -t hugetlbfs -o pagesize=1G none /mnt/huge
            ```

        === "Persistent"

            ```bash
            echo "nodev /mnt/huge hugetlbfs pagesize=1G 0 0" | sudo tee -a /etc/fstab
            sudo mount /mnt/huge
            ```

        !!! note

            If you work with containers, remember to mount this directory in your container as well with `-v /mnt/huge:/mnt/huge`.

    Rerunning the initial commands should now list 4 hugepages of 1GB each. 1GB will be the default huge page size if updated in the kernel bootline only.

    ### Step 5: Isolate CPU cores

    The CPU interacting with the NIC to route packets is sensitive to perturbations, especially with smaller packet/batch sizes requiring more frequent work. Isolating a CPU in Linux prevents unwanted user or kernel threads from running on it, reducing context switching and latency spikes from noisy neighbors.

    We recommend isolating the CPU cores you will select to interact with the NIC (defined in the `daqiri` configuration [described in the configuration reference](configuration-walkthrough.md) in this tutorial). This is done by setting additional flags on the kernel bootline.

    You can first check if any of the recommended flags were already set on the last boot:

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check cmdline
        ```

    === "manual"

        ```bash
        cat /proc/cmdline | grep -e isolcpus -e irqaffinity -e nohz_full -e rcu_nocbs -e rcu_nocb_poll
        ```

    Decide which cores to isolate based on your configuration. We recommend one core per DAQIRI queue, plus one core per benchmark application worker thread (the `bench_tx` / `bench_rx` `cpu_core` fields), as a rule of thumb — these are separate busy-poll threads and should not share a core. First, identify your core IDs:

    ```bash
    cat /proc/cpuinfo | grep processor
    ```

    ??? abstract "See an example output"

        This system has 12 cores, numbered 0 to 11:
        ```bash
        processor       # 0
        processor       # 1
        processor       # 2
        processor       # 3
        processor       # 4
        processor       # 5
        processor       # 6
        processor       # 7
        processor       # 8
        processor       # 9
        processor       # 10
        processor       # 11
        ```

    As an example, the line below will isolate cores 8, 9, 10 and 11 (enough for the two DAQIRI queues and two application worker threads in the base TX+RX config), leaving cores 0-7 free for other tasks and hardware interrupts:

    ```bash
    isolcpus=8-11 irqaffinity=0-7 nohz_full=8-11 rcu_nocbs=8-11 rcu_nocb_poll
    ```

    ??? info "Show explanation"

        | Parameter | Description |
        | --------- | ----------- |
        | `isolcpus` | Isolates specific CPU cores from the Linux scheduler, preventing regular system tasks from running on them. This ensures dedicated cores are available exclusively for your networking tasks, reducing context switches and interruptions that can cause latency spikes. |
        | `irqaffinity` | Controls which CPU cores can handle hardware interrupts. By directing network interrupts away from your isolated cores, you prevent networking tasks from being interrupted by hardware events, maintaining consistent processing time. |
        | `nohz_full` | Disables regular kernel timer ticks on specified cores when they're running user space applications. This reduces overhead and prevents periodic interruptions, allowing your networking code to run with fewer disturbances. |
        | `rcu_nocbs` | Offloads Read-Copy-Update (RCU) callback processing from specified cores. RCU is a synchronization mechanism in the Linux kernel that can cause periodic processing bursts. Moving this work away from your networking cores helps maintain consistent performance. |
        | `rcu_nocb_poll` | Works with `rcu_nocbs` to improve how RCU callbacks are processed on non-callback CPUs. This can reduce latency spikes by changing how the kernel polls for RCU work. |

        Together, these parameters create an environment where specific CPU cores can focus exclusively on network packet processing with minimal interference from the operating system, resulting in lower and more consistent latency.

    Add these flags to the `GRUB_CMDLINE_LINUX` variable in `/etc/default/grub`, then rebuild your GRUB configuration and reboot:

    ```bash
    sudo update-grub
    sudo reboot
    ```

    Verify that the flags were properly set after boot by rerunning the check commands above.

    ### Step 6: Prevent CPU cores from going idle

    When a core goes idle/to sleep, coming back online to poll the NIC can cause latency spikes and dropped packets. To prevent this, **we recommend setting the scaling governor to `performance` for these CPU cores**.

    !!! note

        Cores from a single cluster will always share the same governor.

    !!! bug

        We have witnessed instances where setting the governor to `performance` on only the isolated cores (dedicated to polling the NIC) does not lead to the performance gains expected. As such, we currently recommend setting the governor to `performance` for all cores which has shown to be reliably effective.

    Check the current governor for each of your cores:

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check cpu-freq
        ```

        ??? abstract "See an example output"

            ```
            2025-03-06 12:20:27 - WARNING - CPU 0: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 1: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 2: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 3: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 4: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 5: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 6: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 7: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 8: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 9: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 10: Governor is set to 'powersave', not 'performance'.
            2025-03-06 12:20:27 - WARNING - CPU 11: Governor is set to 'powersave', not 'performance'.
            ```

    === "manual"

        ```bash
        cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
        ```

        ??? abstract "See an example output"

            In this example, all cores were defaulted to `powersave` instead of the recommended `performance`.

            ```
            powersave
            powersave
            powersave
            powersave
            powersave
            powersave
            powersave
            powersave
            powersave
            powersave
            powersave
            powersave
            ```

    Install `cpupower` to more conveniently set the governor:

    ```bash
    sudo apt update
    sudo apt install -y linux-tools-$(uname -r)
    ```

    Set the governor to `performance` for all cores:

    === "One-time"

        ```bash
        sudo cpupower frequency-set -g performance
        ```

    === "Persistent"

        ```bash
        cat << EOF | sudo tee /etc/systemd/system/cpu-performance.service
        [Unit]
        Description=Set CPU governor to performance
        After=multi-user.target

        [Service]
        Type=oneshot
        ExecStart=/usr/bin/cpupower -c all frequency-set -g performance

        [Install]
        WantedBy=multi-user.target
        EOF
        sudo systemctl enable cpu-performance.service
        sudo systemctl start cpu-performance.service
        ```

    Running the checks above should now list `performance` as the governor for all cores. You can also run `sudo cpupower -c all frequency-info` for more details.

    ### Step 7: Prevent the GPU from going idle

    Similarly to the above, we want to maximize the GPU's clock speed and prevent it from going idle.

    Run the following command to check your current clocks and whether they're locked (persistence mode):

    ```text
    nvidia-smi -q | grep -i "Persistence Mode"
    nvidia-smi -q -d CLOCK
    ```

    ??? abstract "See an example output"

        ``` hl_lines="1 7 8 20 21"
            Persistence Mode: Enabled
        ...
        Attached GPUs                             : 1
        GPU 00000005:09:00.0
            Clocks
                Graphics                          : 420 MHz
                SM                                : 420 MHz
                Memory                            : 405 MHz
                Video                             : 1680 MHz
            Applications Clocks
                Graphics                          : 1800 MHz
                Memory                            : 8001 MHz
            Default Applications Clocks
                Graphics                          : 1800 MHz
                Memory                            : 8001 MHz
            Deferred Clocks
                Memory                            : N/A
            Max Clocks
                Graphics                          : 2100 MHz
                SM                                : 2100 MHz
                Memory                            : 8001 MHz
                Video                             : 1950 MHz
            ...
        ```

    To lock the GPU's clocks to their max values:

    === "One-time"

        ```bash
        sudo nvidia-smi -pm 1
        sudo nvidia-smi -lgc=$(nvidia-smi --query-gpu=clocks.max.sm --format=csv,noheader,nounits)
        sudo nvidia-smi -lmc=$(nvidia-smi --query-gpu=clocks.max.mem --format=csv,noheader,nounits)
        ```

    === "Persistent"

        ```bash
        cat << EOF | sudo tee /etc/systemd/system/gpu-max-clocks.service
        [Unit]
        Description=Max GPU clocks
        After=multi-user.target

        [Service]
        Type=oneshot
        ExecStart=/usr/bin/nvidia-smi -pm 1
        ExecStart=/bin/bash -c '/usr/bin/nvidia-smi --lock-gpu-clocks=$(/usr/bin/nvidia-smi --query-gpu=clocks.max.sm --format=csv,noheader,nounits)'
        ExecStart=/bin/bash -c '/usr/bin/nvidia-smi --lock-memory-clocks=$(/usr/bin/nvidia-smi --query-gpu=clocks.max.mem --format=csv,noheader,nounits)'
        RemainAfterExit=true

        [Install]
        WantedBy=multi-user.target
        EOF

        sudo systemctl enable gpu-max-clocks.service
        sudo systemctl start gpu-max-clocks.service
        ```

    ??? info "Show explanation"

        This queries the max clocks for the GPU SM (`clocks.max.sm`) and memory (`clocks.max.mem`) and sets them to the current clocks (`lock-gpu-clocks` and `lock-memory-clocks` respectively). `-pm 1` (or `--persistence-mode=1`) enables persistence mode to lock these values.

    ??? abstract "See an example output"

        ```
        GPU clocks set to "(gpuClkMin 2100, gpuClkMax 2100)" for GPU 00000005:09:00.0
        All done.
        Memory clocks set to "(memClkMin 8001, memClkMax 8001)" for GPU 00000005:09:00.0
        All done.
        ```

    You can confirm that the clocks are set to the max values by running `nvidia-smi -q -d CLOCK` again.

    !!! note

        Some max clocks might not be achievable in certain configurations, or due to boost clocks (SM) or rounding errors (Memory),  despite the lock commands indicating it worked. For example - on IGX - the max non-boot SM clock will be 1920 MHz, and the max memory clock will show 8000 MHz, which are satisfying compared to the initial mode.

    ### Step 8: Maximize GPU BAR1 size

    The GPU BAR1 memory is the primary resource consumed by `GPUDirect`. It allows other PCIe devices (like the CPU and the NIC) to access the GPU's memory space. The larger the BAR1 size, the more memory the GPU can expose to these devices in a single PCIe transaction, reducing the number of transactions needed and improving performance.

    **We recommend a BAR1 size of 1GB or above.** Check the current BAR1 size:

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check bar1-size
        ```

        ??? abstract "See an example output"

            ```
            2025-03-06 12:22:53 - INFO - GPU 00000005:09:00.0: BAR1 size is 8192 MiB.
            ```

    === "manual"

        ```bash
        nvidia-smi -q | grep -A 3 BAR1
        ```

        ??? abstract "See an example output"

            For our RTX A6000, this shows a BAR1 size of 256 MiB:

            ```
                BAR1 Memory Usage
                Total                             : 256 MiB
                Used                              : 13 MiB
                Free                              : 243 MiB
            ```

    !!! warning

        Resizing the BAR1 size requires:

        - A BIOS with resizable BAR support
        - A GPU with physical resizable BAR

        **If you attempt to go forward with the instructions below without meeting the above requirements, you might render your GPU unusable.**

    #### BIOS Resizable BAR support

    First, check if your system and BIOS support resizable BAR. Refer to your system's manufacturer documentation to access the BIOS. The Resizable BAR option is often categorized under `Advanced > PCIe` settings. Enable this feature if found.

    !!! note

        The IGX Developer kit with IGX OS 1.1+ supports resizable BAR by default.

    #### GPU Resizable BAR support

    Next, you can check if your GPU has physical resizable BAR by running the following command:

    ```bash
    sudo lspci -vv -s $(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader) | grep BAR
    ```

    ??? abstract "See an example output"

        This RTX A6000 has a resizable BAR1, currently set to 256 MiB:

        ```
        Capabilities: [bb0 v1] Physical Resizable BAR
            BAR 0: current size: 16MB, supported: 16MB
            BAR 1: current size: 256MB, supported: 64MB 128MB 256MB 512MB 1GB 2GB 4GB 8GB 16GB 32GB 64GB
            BAR 3: current size: 32MB, supported: 32MB
        ```

    If your GPU is listed [on this page](https://developer.nvidia.com/displaymodeselector), you can download the `Display Mode Selector` to resize the BAR1 to 8GB.

    1. Press `Join Now`.
    2. Once approved, download the `Display Mode Selector` archive.
    3. Unzip the archive.
    4. Access your system without an X-server running. SSH into the machine, or switch to a Virtual Console (`Alt+F1`). You do not need to physically disconnect the monitor — the requirement is that no display server (X11/Wayland) is holding a lock on the NVIDIA driver.
    5. Go down the right OS and architecture folder for your system (`linux/aarch64` or `linux/x64`).
    6. Run the `displaymodeselector` command like so:

    ```bash
    chmod +x displaymodeselector
    sudo ./displaymodeselector --gpumode physical_display_enabled_8GB_bar1
    ```

    Press `y` to confirm you'd like to continue, then `y` again to apply to all the eligible adapters.

    ??? abstract "See an example output"

        ```
        NVIDIA Display Mode Selector Utility (Version 1.67.0)
        Copyright (C) 2015-2021, NVIDIA Corporation. All Rights Reserved.

        WARNING: This operation updates the firmware on the board and could make
                the device unusable if your host system lacks the necessary support.

        Are you sure you want to continue?
        Press 'y' to confirm (any other key to abort):
        y
        Specified GPU Mode "physical_display_enabled_8GB_bar1"


        Update GPU Mode of all adapters to "physical_display_enabled_8GB_bar1"?
        Press 'y' to confirm or 'n' to choose adapters or any other key to abort:
        y

        Updating GPU Mode of all eligible adapters to "physical_display_enabled_8GB_bar1"

        Apply GPU Mode <6> corresponds to "physical_display_enabled_8GB_bar1"

        Reading EEPROM (this operation may take up to 30 seconds)

        [==================================================] 100 %
        Reading EEPROM (this operation may take up to 30 seconds)

        Successfully updated GPU mode to "physical_display_enabled_8GB_bar1" ( Mode 6 ).

        A reboot is required for the update to take effect.
        ```

    ??? failure "Error: unload the NVIDIA kernel driver first"

        If you see this error:

        ```bash
        ERROR: In order to avoid the irreparable damage to your graphics adapter it is necessary to unload the NVIDIA kernel driver first:

        rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia_peermem nvidia
        ```

        Stop the display server and then unload the NVIDIA kernel modules listed in the error message (the exact list may vary by system):

        ```bash
        sudo systemctl isolate multi-user
        sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia_peermem nvidia
        ```

        !!! tip "IGX Systems"

            IGX systems may have additional NVIDIA kernel modules (e.g. `nvidia_vrs_pseq`) that must also be unloaded. Check for remaining modules with `lsmod | grep nvidia` and `rmmod` each one before retrying.

    ??? failure "/dev/mem: Operation not permitted. Access to physical memory denied"

        Disable secure boot on your system ahead of changing your GPU's BAR1 size. It can be re-enabled afterwards.

    Reboot your system, and check the BAR1 size again to confirm the change.

    ```bash
    sudo reboot
    ```

    ### Step 9: Enable Jumbo Frames

    Jumbo frames are Ethernet frames that carry a payload larger than the standard 1500 bytes MTU (Maximum Transmission Unit). They can significantly improve network performance when transferring large amounts of data by reducing the overhead of packet headers and the number of packets that need to be processed.

    **We recommend an MTU of 9000 bytes on all interfaces involved in the data path.** You can check the current MTU of your interfaces:

    === "tune_system.py"

        ```bash
        sudo ./python/tune_system.py --check mtu
        ```

        ??? abstract "See an example output"

            ```
            2025-03-06 16:51:19 - INFO - Interface eth0 has an acceptable MTU of 9000 bytes.
            2025-03-06 16:51:19 - INFO - Interface eth1 has an acceptable MTU of 9000 bytes.
            ```

    === "manual"

        For a given `if_name` interface:

        ```bash
        if_name=eth0
        ip link show dev $if_name | grep -oE "mtu [0-9]+"
        ```

        ??? abstract "See an example output"

            ```
            mtu 1500
            ```

    You can set the MTU for each interface like so, for a given `if_name` name identified [above](#configure-the-ip-addresses-of-the-nic-ports):

    === "One-time"

        ```bash
        sudo ip link set dev $if_name mtu 9000
        ```

    === "Persistent"

        === "NetworkManager"

            ```bash
            sudo nmcli connection modify $if_name ethernet.mtu 9000
            sudo nmcli connection up $if_name
            ```

        === "systemd-networkd"

            Assuming you've set an IP address for the interface [above](#configure-the-ip-addresses-of-the-nic-ports), you can add the MTU to the interface's network configuration file like so:

            ```bash
            sudo sed -i '/\[Network\]/a MTU=9000' /etc/systemd/network/20-$if_name.network
            sudo systemctl restart systemd-networkd
            ```

    ??? info "Can I do more than 9000?"

        While your NIC might have a maximum MTU capability larger than 9000, we typically recommend setting the MTU to 9000 bytes, as that is the standard size for jumbo frames that's widely supported for compatibility with other network equipment. When using jumbo frames, all devices in the communication path must support the same MTU size. If any device in between has a smaller MTU, packets will be fragmented or dropped, potentially degrading performance.

        Example with the CX-7 NIC:

        ```bash
        $ ip -d link show dev $if_name | grep -oE "maxmtu [0-9]+"
        maxmtu 9978
        ```

    ---
    **Next:** [Benchmarking](../benchmarks/benchmarks.md) — choose and run your first DAQIRI benchmark

=== "DGX Spark"

    ## DGX Spark profile

    This tab covers a **DGX Spark** workstation: Grace Blackwell **GB10** superchip (unified CPU/GPU memory via NVLink-C2C, integrated **ConnectX-7**, ARM64), running Ubuntu 24.04 with a CUDA-13 / driver-580 stack. Several IGX optimization steps are physically inapplicable on Spark (no separate GPU BAR1, no peermem, no discrete-PCIe path between GPU and NIC). Each is called out as **N/A on Spark** in the corresponding step, with the rationale.

    ### Pre-flight: the CX-7 disappears without a cable

    !!! warning "QSFP cable is required"

        DGX Spark removes the integrated CX-7 PFs from the PCI bus when no QSFP cable is plugged in. After boot, `mlx5_core` probes 4 PFs, each port immediately emits `Cable unplugged`, and the platform service `/opt/nvidia/dgx-spark-mlnx-hotplug` removes all four CX-7 PCIe devices between roughly t=5s and t=20s. Symptoms: `lspci -d 15b3:` is empty, `ibv_devinfo` reports "No IB devices found", and `/sys/class/infiniband/` is empty even though `mlx5_core` and `mlx5_ib` are loaded.

        **Plug a cable into the chassis QSFP socket before debugging firmware, drivers, or BIOS.** The hotplug service brings the device back when a cable is detected.

    The hotplug behavior is implemented as a power/thermal management policy and coordinates with `nvidia-spark-mlnx-firmware-manager.service`. If you need the NIC alive without a cable for software-only testing, the override point is the scripts under `/opt/nvidia/dgx-spark-mlnx-hotplug` — read them before disabling.

    ### Port topology: 4 PFs, 2 ports, tied chassis sockets

    `lspci -d 15b3:` on Spark shows **four** CX-7 PFs across **two** PCIe domains. This is **one** ConnectX-7 ASIC with **two physical ports** (p0, p1); each port is dual-homed across both PCIe segments (socket-direct), which is why a single card appears as four PFs. The PCIe address is also called a BDF (bus-device-function); in the examples below, `0002:01:00.1` is PCI domain `0002`, bus `01`, device `00`, function `1`.

    ```text
    0000:01:00.0  →  mlx5_0  →  enp1s0f0np0
    0000:01:00.1  →  mlx5_1  →  enp1s0f1np1
    0002:01:00.0  →  mlx5_2  →  enP2p1s0f0np0
    0002:01:00.1  →  mlx5_3  →  enP2p1s0f1np1
    ```

    The two chassis QSFPs are bridged by a single loopback cable. Pulling **just one end** drops `carrier` to 0 on **all four** PFs simultaneously, confirming the cable is present and the loop is intact:

    ```bash
    for i in /sys/class/net/{enp1s0f0np0,enp1s0f1np1,enP2p1s0f0np0,enP2p1s0f1np1}; do
        echo "$i: $(cat $i/carrier)"
    done
    ```

    !!! important "Single-machine loopback: same physical port = on-chip test; different ports = over-the-wire test"

        This distinction matters when you run a loopback benchmark within one DGX Spark, where TX and RX are two PFs on the same integrated CX-7. For two-device tests, pick ports according to the external cabling and the peer system's topology; the on-chip shortcut described here is specific to same-machine loopback.

        Which PF pair you choose decides **what you are actually measuring**. Because each physical port is exposed over both PCIe segments, two of the four BDFs map to the *same* physical port. Identify them on your own system with `phys_port_name`:

        ```bash
        for d in /sys/class/net/*/device; do n=${d%/device}; n=${n##*/}; \
            printf '%-16s port=%s pci=%s\n' "$n" \
            "$(cat /sys/class/net/$n/phys_port_name 2>/dev/null)" \
            "$(basename "$(readlink "$d")")"; done
        ```

        Example output:

        ```text
        enp1s0f0np0      port=p0  pci=0000:01:00.0   # mlx5_0
        enp1s0f1np1      port=p1  pci=0000:01:00.1   # mlx5_1
        enP2p1s0f0np0    port=p0  pci=0002:01:00.0   # mlx5_2
        enP2p1s0f1np1    port=p1  pci=0002:01:00.1   # mlx5_3
        ```

        Here two BDFs share each physical port (`mlx5_0` and `mlx5_2` are both **p0**). The two pairings measure different things:

        - **Same physical port** (e.g. `mlx5_0` ↔ `mlx5_2`, both p0) → TX/RX loop **on-chip** through the eswitch; traffic never reaches the cable. Physical-link packet counters stay flat while the vport counters (`tx_good_packets` / `rx_good_packets`) run at line rate. This is a software-path test.
        - **Different physical ports** (e.g. `mlx5_0` p0 ↔ `mlx5_3` p1 `0002:01:00.1`, or `mlx5_0` ↔ `mlx5_1`) → TX/RX loop **over the wire**; physical-link packet counters rise to match the TX/RX counts. This is an over-the-wire test.

        Confirm which case you got from the physical-link packet counters: near zero for on-chip, matching the TX/RX packet counts for over-the-wire. These counters count packets that reached the SerDes/QSFP side of the NIC rather than packets switched internally by the eswitch. The [daqiri bench](../benchmarks/raw_benchmarking.md)'s DPDK "Extended Stats" output reports them as `tx_phy_packets` / `rx_phy_packets`; `ethtool -S` and `mlnx_perf` report the same wire counters as `tx_packets_phy` / `rx_packets_phy`.

    `ethtool -m` reports identical `Connector: 0x23 No separable connector` on all 4 PFs and is **not** useful for distinguishing them; use `phys_port_name` above (the cable-yank carrier test confirms a cable is present but does **not** distinguish ports).

    ## System Setup for DAQIRI

    ### Check your NIC drivers

    Same as IGX — verify `ib_core` is loaded:

    ```bash
    lsmod | grep ib_core
    ```

    Spark ships with the Mellanox stack pre-installed. Two Spark-specific packages are involved at boot: `nvidia-spark-mlnx-firmware-manager` (flashes CX-7 firmware when a cable is present) and `nvidia-mlnx-tools`. Don't disable them.

    ### Switch your NIC link layers to Ethernet

    Run `mlxconfig` once per PCIe domain (one command per chip). The CX-7 firmware on Spark already ships with both link types set to `ETH`, so this is usually a no-op verification:

    ```bash
    for d in 0000:01:00.0 0002:01:00.0; do
        sudo mlxconfig -d "$d" set LINK_TYPE_P1=ETH LINK_TYPE_P2=ETH
    done
    ```

    Reboot only if any flag actually changed.

    ### Configure the IP addresses of the NIC ports

    Spark uses NetworkManager. Create persistent `daqiri-tx` / `daqiri-rx` profiles that pin both the IP and the MTU (so [Step 9: Jumbo frames](#step-9-enable-jumbo-frames-already-covered) is folded in here). `daqiri-tx` is p0 (`enp1s0f0np0`) and `daqiri-rx` is p1 (`enP2p1s0f1np1`) — different physical ports, matching the over-the-wire benchmark example:

    ```bash
    sudo nmcli connection add type ethernet ifname enp1s0f0np0   con-name daqiri-tx \
        ipv4.addresses 1.1.1.1/24 ipv4.method manual ethernet.mtu 9000 \
        ipv4.gateway "" ipv6.method ignore
    sudo nmcli connection add type ethernet ifname enP2p1s0f1np1 con-name daqiri-rx \
        ipv4.addresses 2.2.2.2/24 ipv4.method manual ethernet.mtu 9000 \
        ipv4.gateway "" ipv6.method ignore
    sudo nmcli connection up daqiri-tx
    sudo nmcli connection up daqiri-rx
    ```

    Verify:

    ```bash
    ip -f inet addr show enp1s0f0np0
    ip -f inet addr show enP2p1s0f1np1
    ip link show enp1s0f0np0   | grep -oE "mtu [0-9]+"
    ip link show enP2p1s0f1np1 | grep -oE "mtu [0-9]+"
    ```

    ### Enable GPUDirect

    **No GPUDirect kernel-module setup is required on GB10.** Set `kind: "host_pinned"` in the YAML and you're done — there is no system-side step to perform. Buffers are allocated by DAQIRI via `cudaHostAlloc` (so they are CUDA-addressable) and registered with DPDK via `rte_extmem_register`. End-to-end TX↔RX over the QSFP loop with `kind: "host_pinned"`, `num_bufs: 51200`, `batch_size: 10240` reaches **~94 Gbps** unicast (verified against `main` 9ebd729, which contains [PR #41](https://github.com/nvidia/daqiri/pull/41)).

    `kind: "huge"` works as a fallback at the same rate. `kind: "device"` does **not** work on GB10.

    See the ready-to-run [`examples/daqiri_bench_raw_tx_rx_spark.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_spark.yaml) for the complete config.

    ??? info "Why peermem and DMA-BUF don't apply on GB10"

        `sudo modprobe nvidia_peermem` returns `Invalid argument` (EINVAL, exit=1) on GB10. The module file ships in `/lib/modules/$(uname -r)/kernel/nvidia-580-open/nvidia-peermem.ko`, but loading fails by design: peermem maps the NIC into a separate GPU BAR1, and GB10's NVLink-C2C unified memory has no separate BAR1.

        The Open kernel module on Grace platforms expects the standard Linux **DMA-BUF** path instead of peermem, but as of CUDA 13.1 / driver 580.142 the device-attribute query reports `flag=0`:

        ```text
        cuDeviceGetAttribute(CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, 0)         → SUCCESS, flag=0
        cuDeviceGetAttribute(CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED, 0) → SUCCESS, flag=0
        cuDeviceGetAttribute(CU_DEVICE_ATTRIBUTE_INTEGRATED, 0)                → SUCCESS, flag=1
        ```

        DAQIRI's CUDA-DMA-BUF code path is therefore unreachable on Spark; `dpdk_patches/dmabuf.patch` still ships and is mandatory for the build, but the daqiri-side dma-buf branch never fires. The `host_pinned` path above sidesteps both interfaces entirely.

    ---

    ## System Optimization

    The IGX checklist below applies on Spark with a few items skipped or reshaped. Quick map:

    | Step | Spark status | Notes |
    |------|--------------|-------|
    | 1. PCIe topology | **N/A** | Single-SoC integrated GPU; no separable PCIe path GPU↔NIC |
    | 2. PCIe MPS / Speed | unchanged | Same diagnostic commands; PCIe Gen5 native |
    | 3. NIC MRRS | reshape | Use systemd unit + `setpci CAP_EXP+8.w` (capability-relative); **disable Secure Boot** |
    | 4. Hugepages | reshape | Use a grub **drop-in** under `/etc/default/grub.d/`, not `/etc/default/grub` |
    | 5. CPU isolation | reshape | Pin to big cores 16-19 (X925 cluster 1); folds into the same grub drop-in as Step 4 |
    | 6. CPU governor | already set | Spark default is `performance` on all 20 cores |
    | 7. GPU clocks | unchanged | Same systemd-unit recipe; GB10 max SM clock is 3003 MHz |
    | 8. GPU BAR1 size | **N/A** | Unified memory; no resizable BAR1 |
    | 9. Jumbo frames | folded into setup | MTU=9000 was set in the `daqiri-tx`/`daqiri-rx` nmcli profiles above |

    `tune_system.py --check all` on Spark suppresses the WARNs that are false positives on integrated GPUs (peermem, gpudirect, topology, BAR1) — see the source comments in [`python/tune_system.py`](https://github.com/nvidia/daqiri/blob/main/python/tune_system.py).

    ### Step 1: PCIe topology — N/A on Spark

    `nvidia-smi topo -m` reports the integrated GPU as `SYS`-connected to the NIC PFs. This is structural, not tunable: there is no separable PCIe path GPU↔NIC on a single-SoC integrated GPU. `tune_system.py --check topo` recognizes integrated GPUs and reports INFO instead of WARNING for this case.

    ### Step 2: Check the NIC's PCIe configuration

    Same diagnostic commands as IGX — for each PF, query `MaxPayload` (DevCap vs DevCtl) and PCIe `Speed` (LnkCap vs LnkSta). Spark's CX-7 PFs negotiate PCIe Gen5 at the chip's native MPS; nothing to set.

    ```bash
    for d in 0000:01:00.0 0000:01:00.1 0002:01:00.0 0002:01:00.1; do
        echo "=== $d ==="
        sudo lspci -vv -s "$d" | awk '/DevCap/{s=1} /DevCtl/{s=0} /MaxPayload /{match($0, /MaxPayload [0-9]+/, m); if(s){print "Max " m[0]} else{print "Current " m[0]}}'
        sudo lspci -vv -s "$d" | awk '/LnkCap/{s=1} /LnkSta/{s=0} /Speed /{match($0, /Speed [0-9]+GT\/s/, m); if(s){print "Max " m[0]} else{print "Current " m[0]}}'
    done
    ```

    ### Step 3: Maximize the NIC's MRRS via a systemd unit

    `tune_system.py --set mrrs` writes `0x68.w`. On Spark, write to `CAP_EXP+8.w` (capability-relative) so the change is robust to capability-layout differences and easy to do in a unit file. **Secure Boot must be disabled** for `setpci` writes to succeed (otherwise the kernel's lockdown policy returns EPERM).

    ```bash
    cat << 'EOF' | sudo tee /etc/systemd/system/nic-mrrs.service
    [Unit]
    Description=Set CX-7 PFs MRRS to 4096 (capability-relative)
    After=multi-user.target

    [Service]
    Type=oneshot
    ExecStart=/usr/bin/bash -c 'for d in 0000:01:00.0 0000:01:00.1 0002:01:00.0 0002:01:00.1; do setpci -s "$d" CAP_EXP+8.w=0x5000:0xf000; done'
    RemainAfterExit=true

    [Install]
    WantedBy=multi-user.target
    EOF
    sudo systemctl daemon-reload
    sudo systemctl enable --now nic-mrrs.service
    ```

    Verify:

    ```bash
    for d in 0000:01:00.0 0000:01:00.1 0002:01:00.0 0002:01:00.1; do
        sudo setpci -s "$d" CAP_EXP+8.w
    done
    # Each line should print 5xxx (high nibble 5 = 4096-byte MRRS).
    ```

    ### Step 4: Enable Huge pages — grub drop-in pattern

    Spark composes its `GRUB_CMDLINE_LINUX` from drop-ins under `/etc/default/grub.d/`. Edit a new file rather than `/etc/default/grub` directly so Spark platform updates don't fight your changes. The shipped `daqiri_bench_raw_tx_rx_spark.yaml` needs ~4 GiB of hugepages (kind: HUGE dummy queues + DPDK per-pool overhead); 4 × 1 GiB pages is enough:

    ```bash
    cat << 'EOF' | sudo tee /etc/default/grub.d/daqiri-tuning.cfg
    GRUB_CMDLINE_LINUX="${GRUB_CMDLINE_LINUX} default_hugepagesz=1G hugepagesz=1G hugepages=4 isolcpus=16-19 nohz_full=16-19 rcu_nocbs=16-19 rcu_nocb_poll irqaffinity=0-15"
    EOF
    sudo update-grub
    ```

    Add the 1G hugepage mount and pre-create the directory:

    ```bash
    echo "nodev /mnt/huge hugetlbfs pagesize=1G 0 0" | sudo tee -a /etc/fstab
    sudo mkdir -p /mnt/huge
    ```

    Reboot once — Steps 4 and 5 land together.

    ```bash
    sudo reboot
    ```

    After reboot, verify:

    ```bash
    grep Huge /proc/meminfo
    mount | grep huge
    ```

    ### Step 5: Isolate CPU cores

    Spark has 20 cores arranged big.LITTLE-style: cluster 0 is 10 Cortex-A725 LITTLE cores (IDs 0-9 + part of 10-15), cluster 1 is the big Cortex-X925 cores (IDs 16-19 are the four highest-frequency big cores). Pin both the DAQIRI TX/RX/processing threads and the benchmark application's `bench_tx.cpu_core` / `bench_rx.cpu_core` worker threads onto **16, 17, 18, 19**; otherwise the application side of the benchmark can land on a lower-power core and become the measurement bottleneck. The rest of the system continues working normally on cores 0-15.

    The grub drop-in created in [Step 4](#step-4-enable-huge-pages-grub-drop-in-pattern) above already includes:

    ```text
    isolcpus=16-19 nohz_full=16-19 rcu_nocbs=16-19 rcu_nocb_poll irqaffinity=0-15
    ```

    Verify after reboot:

    ```bash
    cat /proc/cmdline | grep -oE "isolcpus=[^ ]+|nohz_full=[^ ]+|rcu_nocbs=[^ ]+|irqaffinity=[^ ]+"
    ```

    ### Step 6: CPU governor — already `performance` on Spark

    Spark ships with `performance` on all 20 cores. Verify:

    ```bash
    cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
    ```

    If you want a belt-and-suspenders unit anyway, the IGX `cpu-performance.service` recipe works unchanged.

    ### Step 7: Prevent the GPU from going idle

    The IGX [`gpu-max-clocks.service`](#step-7-prevent-the-gpu-from-going-idle) systemd-unit recipe works on Spark with one item dropped: GB10's unified CPU/GPU memory has no separate VRAM clock domain, so `clocks.max.mem` reports `N/A` and `nvidia-smi -lmc` fails. Lock only the SM clock at runtime:

    ```bash
    sudo nvidia-smi -pm 1
    sudo nvidia-smi -lgc=$(nvidia-smi --query-gpu=clocks.max.sm --format=csv,noheader,nounits)
    ```

    On a production GB10 the locked SM clock is 3003 MHz. If `nvidia-smi -pm 1` reports persistence mode is unsupported on this platform, the lock-clocks call still takes effect for the current driver session — fold it into a unit (omitting the `--lock-memory-clocks` line from the IGX recipe) and start it after reboot.

    ### Step 8: GPU BAR1 size — N/A on Spark

    GB10 has unified CPU/GPU memory (NVLink-C2C coherent) — there is no resizable BAR1 to enlarge, so the entire IGX displaymodeselector / firmware-flash flow does not apply. `nvidia-smi -q | grep -A 3 BAR1` may print numbers but they are not actionable. `tune_system.py --check bar1-size` reports INFO instead of WARNING when an integrated GPU is detected.

    ### Step 9: Enable Jumbo frames — already covered

    The `daqiri-tx` / `daqiri-rx` nmcli profiles created in [Configure the IP addresses](#configure-the-ip-addresses-of-the-nic-ports_1) already pin `ethernet.mtu 9000`, so this step is a no-op on Spark. Verify:

    ```bash
    ip link show enp1s0f0np0   | grep -oE "mtu [0-9]+"
    ip link show enP2p1s0f1np1 | grep -oE "mtu [0-9]+"
    ```

    ---
    **Next:** [Benchmarking](../benchmarks/benchmarks.md) — choose and run your first DAQIRI benchmark

</div>
