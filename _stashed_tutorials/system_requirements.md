# System Requirements

This page covers the minimum hardware and software requirements and the essential system setup steps needed before using DAQIRI. Complete this setup before moving on to system optimization or running benchmarks.

!!! note

    This solution is designed for users who want to create an application that interfaces with an external system or sensor over Ethernet.

    - For high performance communication with systems also running Holoscan, refer to the [Holoscan distributed application documentation](https://docs.nvidia.com/holoscan/sdk-user-guide/holoscan_create_distributed_app.html) instead.
    - For JESD-compliant sensors without Ethernet support, consider the [Holoscan Sensor Bridge](https://docs.nvidia.com/holoscan/sensor-bridge/latest/introduction.html) for an FPGA-based interface.

## Prerequisites

Achieving high performance networking with DAQIRI requires a system with an [**NVIDIA SmartNIC**](https://www.nvidia.com/en-us/networking/ethernet-adapters/) and a [**discrete GPU**](https://www.nvidia.com/en-us/design-visualization/desktop-graphics/). That is the case of [NVIDIA Data Center](https://www.nvidia.com/en-us/data-center/) systems, or edge systems like the [NVIDIA IGX](https://www.nvidia.com/en-us/edge-computing/products/igx/) platform and the [NVIDIA Project DIGITS](https://www.nvidia.com/en-us/project-digits/). `x86_64` systems equipped with these components are also supported, though the performance will vary greatly depending on the PCIe topology of the system (more on this in [System Optimization](system_optimization.md#ensure-ideal-pcie-topology)).

In this tutorial, we will be developing on an **NVIDIA IGX Orin platform** with [IGX SW 1.1](https://docs.nvidia.com/igx-orin/user-guide/latest/base-os.html) and an [NVIDIA RTX 6000 ADA GPU](https://www.nvidia.com/en-us/design-visualization/rtx-6000/), which is the configuration that is currently actively tested. The concepts should be applicable to other systems based on Ubuntu 22.04 as well. It should also work on other Linux distributions with a glibc version of 2.35 or higher by containerizing the dependencies and applications on top of an Ubuntu 22.04 image, but this is not actively tested at this time.

!!! Warning "Secure boot conflict"

    If you have secure boot enabled on your system, you might need to disable it as a prerequisite to run some of the configurations below ([switching the NIC link layers to Ethernet](#switch-your-nic-link-layers-to-ethernet), [updating the MRRS of your NIC ports](system_optimization.md#maximize-the-nics-max-read-request-size-mrrs), [updating the BAR1 size of your GPU](system_optimization.md#maximize-gpu-bar1-size)). Secure boot can be re-enabled after the configurations are completed.

## Check your NIC drivers

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

If this is empty, install the latest OFED drivers from DOCA (the DOCA APT repository should already be configured from the [DAQIRI build setup](../getting-started.md#building-from-source)), and reboot your system:

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

## Switch your NIC Link Layers to Ethernet

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

**For Holoscan Networking, we want the NIC to use the ETH link layer.** To switch the link layer mode, there are two possible options:

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

## Configure the IP addresses of the NIC ports

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

The next step is to set a static IP on the interface you'd like to use so you can refer to it in your Holoscan applications. First, check if you already have any addresses configured using the ethernet interface names identified above (in our case, `eth0` and `eth1`):

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

## Enable GPUDirect

Assuming you already have [NVIDIA drivers](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/index.html#ubuntu-installation-network) installed, check if the `nvidia_peermem` kernel module is loaded:

=== "tune_system.py"

    === "Debian installation"

        ```bash
        sudo /opt/nvidia/holoscan/bin/tune_system.py --check topo
        ```

    === "From source"

        ```bash
        cd holohub
        sudo ./operators/advanced_network/python/tune_system.py --check topo

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

??? info "Why peermem and not dma buf?"

    `peermem` is currently the only GPUDirect interface supported by all our [networking backends](background.md#kernel-bypass). This section will therefore provide instructions for `peermem` and not `dma buf`.

---
**Next:** [System Optimization](system_optimization.md) — tune your system for maximum performance
