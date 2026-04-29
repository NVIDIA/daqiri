# Tutorials

Step-by-step guides for setting up and using DAQIRI for high-performance networking.

## Getting Started

Start here to understand the core concepts and prepare your system for DAQIRI.

### [Getting Started](getting_started.md)
System requirements at a glance, building DAQIRI from source (container or CMake), and links to the detailed setup tutorials.

### [Background](background.md)
Learn about the core technologies behind DAQIRI — Kernel Bypass (DPDK, RDMA, DOCA GPUNetIO, Rivermax) and GPUDirect.

### [System Requirements](system_requirements.md)
Hardware prerequisites and essential system setup — NIC drivers, link layers, IP configuration, and GPUDirect enablement.

## Configuration & Tuning

### [System Optimization](system_optimization.md)
Advanced performance tuning — PCIe topology, MRRS, hugepages, CPU isolation, GPU clocks, BAR1 sizing, and jumbo frames.

### [Understanding the Configuration File](configuration.md)
Deep dive into the YAML configuration used by DAQIRI benchmarks and applications, with annotated explanations of every parameter.

## Running DAQIRI

### [Benchmarking Examples](benchmarking_examples.md)
Run the DAQIRI benchmark application — container setup, loopback configuration, and troubleshooting.
