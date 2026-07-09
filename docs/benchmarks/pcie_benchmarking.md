---
hide:
  - navigation
---

# PCIe / GPUDirect Benchmarking

The PCIe stream moves batches directly between an FPGA and DAQIRI-owned CUDA
device memory. Select it with `stream_type: "pcie"`. PCIe is a stream type, not
an engine: do not add `engine: "pcie"`, `engine: "dmabuf"`, or
`engine: "peermem"` to the configuration. DMA-BUF is the internal GPU-memory
registration mechanism and is not an application choice.

The repository includes a software provider for development and protocol
testing. A board-specific character driver and FPGA bitstream are required for
hardware operation and are not shipped by DAQIRI.

## Build

PCIe support is independent of the optional Ethernet engines. CUDA Toolkit 12.8
or newer is required because DAQIRI requests a PCIe BAR1 DMA-BUF mapping
explicitly.

For a PCIe-only container image:

```bash
BASE_TARGET=base-deps DAQIRI_ENABLE_PCIE=ON DAQIRI_ENGINE="" \
  scripts/build-container.sh
```

The equivalent direct CMake build is:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DDAQIRI_ENABLE_PCIE=ON \
  -DDAQIRI_ENGINE=""
cmake --build build -j
```

`DAQIRI_ENABLE_PCIE=ON` builds the internal PCIe implementation and
`daqiri_bench_pcie`; it does not add a value to `DAQIRI_ENGINE` or
`EngineType`.

## Run the software loopback

The shipped loopback config needs a CUDA-capable discrete GPU but no FPGA or
PCIe driver:

```bash
./build/examples/daqiri_bench_pcie \
  ./build/examples/daqiri_bench_pcie_sw_loopback.yaml \
  --seconds 10 --mode both
```

`--mode` accepts `tx`, `rx`, or `both`. `--seconds 0` runs until Ctrl-C. The
benchmark embeds a little-endian 64-bit sequence in each packet, fills the rest
with a deterministic byte pattern, and validates the received GPU buffers. Its
synchronous CUDA copies intentionally demonstrate the application ordering
contract: TX writes finish before `send_tx_burst()`, and RX reads finish before
the burst is freed.

The loopback config also holds four validated RX bursts while later completions
arrive, revalidates their original sequence and payload before release, then
frees alternating packets individually before releasing each whole burst. This
checks that application-owned slots are not overwritten and that a mixed
individual/burst free cannot return a slot twice. Its 17-packet TX bursts
deliberately differ from the 32-packet RX batch size; `timeout_us` therefore
publishes partial RX bursts too. With 256 slots, a one-second run completes many
ring wraps and TX completion/reclamation cycles.

The benchmark removes the unused direction from the parsed configuration for
`tx` or `rx` mode. Software loopback creates RX data only by copying submitted
TX data, so use `both` for its closed-loop validation; standalone `rx` is for an
external FPGA source.

The final TX and RX lines report packets, bytes, bursts, backpressure, validation
errors, elapsed time, and application throughput. Software-loopback numbers
measure the mock provider and CUDA copies; they are protocol smoke-test results,
not FPGA or PCIe performance measurements.

### Inject completion faults

The software provider has a private, one-shot test hook for completion
validation. Each command below is expected to mark the interface unhealthy and
make the benchmark exit nonzero:

```bash
DAQIRI_PCIE_LOOPBACK_FAULT=stale_epoch \
  ./build/examples/daqiri_bench_pcie \
  ./build/examples/daqiri_bench_pcie_sw_loopback.yaml --seconds 2 --mode both

DAQIRI_PCIE_LOOPBACK_FAULT=bad_length \
  ./build/examples/daqiri_bench_pcie \
  ./build/examples/daqiri_bench_pcie_sw_loopback.yaml --seconds 2 --mode both

DAQIRI_PCIE_LOOPBACK_FAULT=device_reset \
  ./build/examples/daqiri_bench_pcie \
  ./build/examples/daqiri_bench_pcie_sw_loopback.yaml --seconds 2 --mode both
```

These hooks are for the software provider's verification path only; they are not
DAQIRI configuration or public application APIs.

## Configure hardware

Start from the loopback YAML, remove `loopback: "sw"`, and replace
`address: "loopback"` with the FPGA PCI domain/bus/device/function, for example
`0000:65:00.0`. A production interface has exactly one queue in each enabled
direction, and every queue has ID `0`:

```yaml
daqiri:
  cfg:
    version: 1
    stream_type: "pcie"
    master_core: 3

    memory_regions:
    - name: "RX_GPU"
      kind: "device"
      affinity: 0
      num_bufs: 4096
      buf_size: 1048576
    - name: "TX_GPU"
      kind: "device"
      affinity: 0
      num_bufs: 4096
      buf_size: 1048576

    interfaces:
    - name: "fpga0"
      address: "0000:65:00.0"
      rx:
        queues:
        - name: "rx0"
          id: 0
          cpu_core: 8
          batch_size: 32
          timeout_us: 100
          memory_regions: ["RX_GPU"]
      tx:
        queues:
        - name: "tx0"
          id: 0
          cpu_core: 9
          batch_size: 32
          memory_regions: ["TX_GPU"]
```

Each direction must reference one distinct, DAQIRI-owned `device` memory region.
Regions cannot be shared between RX and TX or between interfaces. PCIe v1 does
not support host memory, multiple segments/HDS, multiple queues, flow rules,
reorder plans, timestamps, offloads, pacing, accurate send, or socket/RoCE
blocks. Static use of one of those features fails initialization; corresponding
runtime-only APIs return `Status::NOT_SUPPORTED`.

## Buffer ownership and CUDA ordering

The FPGA and application must never access the same slot concurrently.

- RX ownership is free → FPGA-owned → completed → application-owned → free.
  DAQIRI returns the slot to the FPGA only after `free_packet()` or an RX
  packet/burst free helper. Finish every CUDA kernel or copy that reads an RX
  pointer before that call.
- TX ownership is free → application-owned → FPGA-owned → free. Finish every
  CUDA kernel or copy that writes a TX pointer before `send_tx_burst()`.
  `send_tx_burst()` transfers ownership on `SUCCESS`; an FPGA TX completion
  returns the slot only after every PCIe read has completed.
- Do not run a persistent kernel against slots that may be owned by the FPGA.
  CUDA stream ordering alone is not a substitute for completing work at the
  ownership boundary.

DAQIRI sets `CU_POINTER_ATTRIBUTE_SYNC_MEMOPS` on exported allocations. After an
RX completion it also applies the CUDA GPUDirect write-visibility operation when
the platform's native ordering is insufficient, before publishing the burst to
the application. Initialization fails when the platform can provide neither
native ordering nor a supported host flush.

The FPGA has a separate responsibility: an RX completion must be published only
after all payload writes have reached GPU memory, and a TX completion only after
all reads have returned. Firmware must use a real DMA fence and must not allow a
completion write to pass payload traffic through PCIe Relaxed Ordering.

## Driver and FPGA protocol

The production provider exports each CUDA allocation once as a PCIe BAR1
DMA-BUF, passes the file descriptor to the board driver, and retains the
registration for the DAQIRI lifetime. The driver is a DMA-BUF importer: it keeps
the attachment alive, maps the complete scatter/gather list for the FPGA, and
never exposes CUDA virtual addresses as bus addresses.

Capability negotiation requires both `DMABUF_PCIE` registration and
`DMA_FENCE`: the former promises import of the explicit CUDA PCIe mapping, and
the latter promises that completions have the payload-ordering guarantees
described above. `NV_P2P` is reserved for a possible legacy provider;
`nvidia-peermem` is not the custom-FPGA registration interface. The versioned
driver operations are:

- get capabilities;
- register and unregister a region;
- configure the queue rings;
- start;
- stop and acknowledge DMA quiescence;
- reset; and
- get status.

One host-coherent single-producer/single-consumer ring is used for each
ownership transition:

| Ring | Producer → consumer | Meaning |
| --- | --- | --- |
| RX available | DAQIRI → FPGA | Slots the FPGA may overwrite |
| RX completion | FPGA → DAQIRI | Completed slot, actual length, and status |
| TX submission | DAQIRI → FPGA | GPU slot and length to read |
| TX completion | FPGA → DAQIRI | All reads from the slot have completed |

The canonical C-compatible layout is
[`include/daqiri/pcie_abi.h`](https://github.com/NVIDIA/daqiri/blob/main/include/daqiri/pcie_abi.h).
The shared ABI has magic `DQPC`, version `1.0`, and little-endian 32-byte
entries containing `epoch:u64`, `sequence:u64`, `region_id:u32`, `slot_id:u32`,
`length:u32`, `status:u16`, and `flags:u16`. Ring depths are powers of two;
producer and consumer use cache-line-separated monotonic 64-bit counters. A
producer must stop when a ring is full and must never overwrite an unread entry.
Every completion echoes the submitted epoch, sequence, region ID, and slot ID.
A TX completion also echoes the submitted length exactly; an RX completion sets
`length` to the actual number of bytes written, bounded by the configured slot
payload size. A stale epoch, duplicate/unknown sequence or slot, invalid length,
DMA fault, or device reset makes the DAQIRI interface unhealthy and stops
further submission.

Stable completion status values are `OK`, `BAD_DESCRIPTOR`, `LENGTH_ERROR`,
`DMA_FAULT`, `DEVICE_RESET`, and `INTERNAL_ERROR`. `DMABUF_PCIE` and
`DMA_FENCE` are required in v1; `DEVICE_RESET` describes optional reset
support, and `NV_P2P` remains reserved. A driver must advertise only behavior
it implements.

### Character-device discovery and exclusivity

An `interfaces[].address` beginning with `/dev/` is opened directly. For a PCI
BDF such as `0000:65:00.0`, the production provider first reads
`/sys/bus/pci/devices/0000:65:00.0/daqiri_pcie/char`; its `major:minor` value is
opened through `/dev/char/<major>:<minor>`. If that attribute is absent, the
fallback node is `/dev/daqiri-pcie-0000_65_00_0` (colons and the function dot
become underscores). The installed UAPI header publishes the attribute and
node-prefix constants.

The driver must enforce exclusive open for a device session. DAQIRI also reads
status during provider open and rejects a device whose `RUNNING` flag is
already set. A change in `reset_count`, a fatal status, or an unexpected loss
of `RUNNING` while queues are active makes the interface unhealthy.

During shutdown DAQIRI stops submission, waits for the FPGA to quiesce DMA,
drains or invalidates completions, unmaps the control rings, unregisters the
DMA-BUF regions, and only then frees CUDA memory. A hardware driver must make
the quiesce acknowledgement cover every outstanding read and write.

## Hardware readiness checklist

- The GPU, FPGA, and PCIe fabric support peer-to-peer transactions and the
  topology keeps them under the same root complex where possible.
- IOMMU translation is disabled for the peer path or configured for identity /
  pass-through mappings supported by the driver.
- GPU BAR1 is large enough for all registered regions, including allocation
  alignment overhead.
- The board driver implements the published DAQIRI UAPI and safely handles the
  complete DMA-BUF scatter/gather mapping.
- The trusted FPGA bitstream implements ring backpressure, epoch/sequence
  validation, length bounds, and the required DMA fences.

See [System Configuration](../tutorials/system_configuration.md#pcie-fpga-streams)
for host checks and the
[NVIDIA GPUDirect RDMA guide](https://docs.nvidia.com/cuda/gpudirect-rdma/)
for platform ordering and BAR1 background.

**Previous:** [Benchmarking Overview](benchmarks.md)<br>
**Next:** [Socket and RDMA Benchmarking](socket_benchmarking.md)
