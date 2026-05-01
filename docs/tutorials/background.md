# Background

Understanding the core networking concepts below is helpful before diving into system setup. This page covers the two foundational technologies — Kernel Bypass and GPUDirect — that underpin all DAQIRI backends.

## Kernel Bypass

In this context, Kernel Bypass refers to bypassing the operating system's kernel to directly communicate with the network interface (NIC), greatly reducing the latency and overhead of the Linux network stack. There are multiple technologies that achieve this in different fashions. They're all Ethernet-based, but differ in their implementation and features. The goal of the DAQIRI library is to provide a common higher-level interface to all these backends:

- **RDMA**: Remote Direct Memory Access, using the open-source [`rdma-core`](https://github.com/linux-rdma/rdma-core) library. It differs from the other Ethernet-based backends with its server/client model and RoCE (RDMA over Ethernet) protocol. Given the extra cost and complexity to setup on both ends, it offers a simpler user interface, orders packets on arrival, and is the only one to offer a high reliability mode.
- **DPDK**: the Data Plane Development Kit is an open-source project part of the Linux Foundation with a strong and long-lasting community support. Its RTE Flow capability is generally considered the most flexible solution to split packets ingress and egress data.
- **DOCA GPUNetIO**: This NVIDIA proprietary technology differs from the other backends by transmitting and receiving packets from the NIC using a GPU kernel instead of CPU code, which is highly beneficial for CPU-bound applications.
- **NVIDIA Rivermax**: NVIDIA's other proprietary kernel bypass technology. For a license fee, it should offer the lowest latency and lowest resource utilization for video streaming (RTP packets).

??? example "Work In Progress"

    The DAQIRI library integration testing infrastructure is under active development. As such:

    - The **DPDK** backend is supported and distributed with the DAQIRI library, and is the only backend actively tested at this time.
    - The **DOCA GPUNetIO** backend is supported and distributed with the DAQIRI library, with testing infrastructure under development.
    - The **NVIDIA Rivermax** backend is supported for Rx only when building from source, but not yet distributed nor actively tested. Tx support is under development.
    - The **RDMA** backend is under active development and should be available soon.

Which backend is best for your use case will depend on multiple factors, such as packet size, batch size, data type, and more. The goal of the DAQIRI library is to abstract the interface to these backends, allowing developers to focus on the application logic and experiment with different configurations to identify the best technology for their use case.

## GPUDirect

`GPUDirect` allows the NIC to read and write data from/to a GPU without requiring to copy the data the system memory, decreasing CPU overheads and significantly reducing latency. An implementation of `GPUDirect` is supported by all the kernel bypass backends listed above.

!!! Warning

    `GPUDirect` is only supported on Workstation/Quadro/RTX GPUs and Data Center GPUs. It is not supported on GeForce cards.

??? info "How does that relate to peermem or dma-buf?"

    There are two interfaces to enable `GPUDirect`:

    - The [`nvidia-peermem`](https://docs.nvidia.com/cuda/gpudirect-rdma/) kernel module, distributed with the NVIDIA DKMS GPU drivers.
        - Supported on Ubuntu kernels 5.4+, deprecated starting with kernel 6.8.
        - Supported on NVIDIA optimized Linux kernels, including IGX OS and DGX OS.
        - Supported by all MOFED drivers (requires rebuilding nvidia-dkms drivers afterwards).
    - [`DMA Buf`](https://docs.kernel.org/driver-api/dma-buf.html), supported on Linux kernels 5.12+ with NVIDIA open-source drivers 515+ and CUDA toolkit 11.7+.

## Next Steps

- [System Configuration](system_configuration.md)
- [Benchmarking Examples](benchmarking_examples.md)
- [Understanding the Configuration File](configuration-walkthrough.md)
