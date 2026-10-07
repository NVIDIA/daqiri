---
hide:
  - navigation
---

# Getting Started

DAQIRI's first-run path is: **build the library**, optionally **tune the host**
for maximum performance, then **run a benchmark**. This page covers the basic
startup steps and links to long-form guides when you need platform-specific
setup, bare-metal packaging, or the full API reference.

## System Requirements

DAQIRI can run a plain Linux socket path on modest hardware, but the common
Raw Ethernet, RoCE, and GPUDirect paths depend on NVIDIA networking and GPU
capabilities. Start with the hardware you plan to exercise.

The built-in TCP and UDP paths bound each internal RX backlog by the smallest
memory-region `num_bufs` referenced by that queue. A sustained slow consumer
therefore cannot grow DAQIRI's process queue without limit. TCP applies
backpressure to the sender; UDP leaves overflow in the bounded kernel socket
buffer, where excess datagrams are dropped. See
[TCP receive backpressure and queue sizing](benchmarks/socket_benchmarking.md#tcp-receive-backpressure-and-queue-sizing).

### Hardware

| Component | Required for | Requirement |
|---|---|---|
| **Linux host** | All paths | Linux kernel 5.15+; Ubuntu 22.04 or 24.04 recommended |
| **NVIDIA NIC** | Raw Ethernet, RoCE, GPUDirect | ConnectX-6 Dx or later. Packet pacing, accurate timed transmission, and hardware reorder require ConnectX-7 or later. |
| **NVIDIA GPU** | GPUDirect and GPU post-processing | RTX or Data Center GPU. GeForce is not supported. |
| **Hugepages** | DPDK Raw Ethernet or `kind: huge` memory regions | Reserved hugepages on the host or in the container runtime environment. |

Supported platforms include NVIDIA Data Center systems, NVIDIA IGX, NVIDIA DGX
Spark, and `x86_64` systems with the NIC/GPU requirements above.

### Required Libraries And Tools

The container build is the recommended starting point because it bundles the
user-space libraries and builds the patched DPDK used by DAQIRI. For bare-metal
builds, install the matching packages yourself by following the
[Bare-Metal CMake Build](tutorials/bare-metal-cmake-build.md) tutorial.

| Component | Required for | Notes |
|---|---|---|
| **CUDA Toolkit 12.2+** | Build and GPU paths | The container currently ships CUDA 13.1. CUDA Toolkit 13.0+ adds the default GB10 (`sm_121`) build target. |
| **CMake 3.20+ and C++ build tools** | All source builds | `cmake`, a C++ compiler, `git`, `pkg-config`, Python, and standard build tooling. |
| **DPDK** | DPDK Raw Ethernet engine | Included in the DAQIRI container and patched for dma-buf GPUDirect, so `nvidia-peermem` is not required inside the container. |
| **libibverbs / librdmacm / mlx5 provider** | RoCE and ibverbs Raw Ethernet engine | Needed for `roce://` socket endpoints and the pure-DevX ibverbs raw engine. |
| **NIC diagnostic utilities** | System setup and benchmarks | `ibstat`, `ibv_devinfo`, `ibdev2netdev`, `mlnx_perf`, `mlxconfig`, and related tools are strongly recommended. |
| **Vendored submodules** | All source builds | `third_party/yaml-cpp` and `third_party/spdlog`; initialize submodules before configuring. |

### Optional Libraries

| Component | Enables | Notes |
|---|---|---|
| **pybind11** | Python bindings | Only needed with `-DDAQIRI_BUILD_PYTHON=ON`. |
| **cuFile / GDS** | CUDA device-memory burst file writes | Only needed with `-DDAQIRI_ENABLE_GDS=ON`; host-memory writes use POSIX APIs without GDS. |
| **AWS SDK for C++ with S3** | Raw packet writes to S3-compatible object stores | Only needed with `-DDAQIRI_ENABLE_S3=ON`; the container can build this SDK from source. |
| **OpenTelemetry C++** | Metrics instrumentation | Only needed with `-DDAQIRI_ENABLE_OTEL_METRICS=ON`; applications still configure the SDK reader/exporter. |
| **libnuma** | NUMA-aware ring, pool, and huge-memory placement | Auto-detected. DAQIRI falls back to first-touch placement when absent. |

## Build {#build-the-daqiri-library}

<span id="container-build"></span>

Choose either the container build or a bare-metal CMake build. The container is
the recommended first pass because it carries the DAQIRI source build, patched
DPDK, CUDA user-space dependencies, and RDMA libraries in one image.

=== "Container build (recommended)"

    ```bash
    git clone git@github.com:NVIDIA/daqiri.git
    cd daqiri
    BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
    ```

    IGX Thor ships CUDA 13.0. Match that version instead of the default CUDA
    13.1 image:

    ```bash
    CUDA_VERSION=13.0.0 BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" \
      scripts/build-container.sh
    ```

    Use `BASE_IMAGE=torch` when you need the Torch or TensorRT dependencies:

    ```bash
    BASE_IMAGE=torch BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
    ```

    This selects the base image only; building the opt-in TensorRT example
    applications is a separate `DAQIRI_BUILD_APPLICATIONS=ON` source-build
    workflow covered in the [TensorRT inference tutorial](tutorials/daqiri-resnet-inference.md#build).

=== "CMake build (bare-metal)"

    Bare-metal builds are supported, but the full setup depends on the host
    distribution, DOCA/CUDA repositories, and DPDK install prefix. Follow
    [Bare-Metal CMake Build](tutorials/bare-metal-cmake-build.md) for the full
    dependency list, DPDK patch workflow, installation checks, cleanup commands,
    and troubleshooting.

    ```bash
    git clone git@github.com:NVIDIA/daqiri.git
    cd daqiri
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_ENGINE="dpdk ibverbs"
    cmake --build build -j
    cmake --install build --prefix /opt/daqiri
    ```

    After installation, CMake consumers link the exported target with
    `find_package(daqiri REQUIRED)` and
    `target_link_libraries(my_app PRIVATE daqiri::daqiri)`. Pkg-config
    consumers can use `pkg-config --cflags --libs daqiri`.

    Most users can keep the defaults. Change CMake flags when enabling Python
    bindings, GDS, S3, OpenTelemetry, a smaller engine set, tests, or a GPU
    architecture not covered by the default build. See
    [CMake options reference](tutorials/bare-metal-cmake-build.md#cmake-options-reference).

## Tune The System

This step is optional, but recommended before collecting performance numbers.
The built-in host checks surface common networking, GPUDirect, hugepage, and
affinity issues before you spend time debugging benchmark output.

```bash
sudo python3 python/tune_system.py --check all
```

The script reports what it can inspect automatically. Persistent host changes
such as NIC link layer, hugepages, BAR1 size, MRRS, CPU isolation, GPU clocks,
and programmable flex parsing are covered in
[System Configuration](tutorials/system_configuration.md).

## Run A Benchmark

DAQIRI benchmarks pair an executable with a YAML configuration. If you have a
cable looped back between NIC ports on the system, start with a closed-loop Raw
Ethernet run after replacing the `<angle-bracket>` placeholders in the YAML for
your system.

=== "Container build"

    Launch the container with hardware access:

    ```bash
    docker run --rm -it --privileged \
      --runtime=nvidia \
      --network=host \
      -v /dev/hugepages:/dev/hugepages \
      daqiri:local bash
    ```

    Then run the installed benchmark inside the container:

    ```bash
    /opt/daqiri/bin/daqiri_bench_raw_gpudirect \
        /opt/daqiri/bin/daqiri_bench_raw_tx_rx.yaml \
        --seconds 10
    ```

=== "CMake build"

    ```bash
    ./build/examples/daqiri_bench_raw_gpudirect \
        ./build/examples/daqiri_bench_raw_tx_rx.yaml \
        --seconds 10
    ```

Other smoke tests exist if you do not have a cable loopback, including hardware
loopback on supported NICs and software loopback when no NIC is available. For
those paths, or for throughput and latency measurements, follow the benchmark
guide that matches your stream:

- [Benchmarking overview](benchmarks/index.md): choose a stream type and engine.
- [Raw Ethernet Benchmarking](benchmarks/raw_benchmarking.md): DPDK or ibverbs
  raw packet benchmarks, loopback setup, flow programming, hardware reorder, and
  throughput measurement with `mlnx_perf`.
- [Socket and RDMA Benchmarking](benchmarks/socket_benchmarking.md): UDP/TCP and
  RoCE examples.

## Next Steps

Keep these pages nearby as you go deeper:

1. [Concepts](concepts.md): stream types, engines, endpoint URI schemes,
   packets, bursts, segments, flows, queues, memory regions, GPUDirect, and
   zero-copy ownership.
2. [Configuration YAML Walkthrough](tutorials/configuration-walkthrough.md):
   annotated examples and a decision tree for choosing an example config.
3. [System Configuration](tutorials/system_configuration.md): NIC drivers, link
   layers, GPUDirect, hugepages, CPU isolation, GPU clocks, and performance
   tuning.
4. [Benchmarking](benchmarks/index.md): choose an engine, then run socket/RDMA
   or Raw Ethernet benchmarks.
5. [API Guide](api-reference/index.md): the DAQIRI application lifecycle and
   configuration-first model. Runtime queues, memory regions, and dynamic RX
   flows are covered in [C++ API Usage](api-reference/cpp.md) and
   [Python API Usage](api-reference/python.md).
