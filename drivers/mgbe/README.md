# Tegra MGBE userspace driver

This experimental driver gives DAQIRI exclusive ownership of selected Tegra MGBE
ports. NVIDIA's `nvethernet` kernel driver retains clock, reset, PHY, MAC and
IOMMU setup. An opt-in control shim allocates coherent host-memory DMA pools
and maps descriptor rings, packet buffers and DMA doorbells into userspace.
The userspace driver polls those rings with ARM DMA/MMIO barriers; data
interrupts and the kernel packet path are disabled for the acquired port.

The DAQIRI adapter is selected with `stream_type: raw` and `engine: mgbe`.
It is independent of DPDK, libibverbs and AF_XDP. Other raw-engine defaults
remain unchanged; select MGBE explicitly.

## Supported configuration

- Linux on IGX Thor/Tegra264 with **4-KiB system pages**, matching kernel/OOT
  headers and the pinned Canonical `tegra-oot` source below. The first source
  port targets IGX 2.0's `nvethernet` source fingerprint
  `ACEB0B6366A461508EE22A8` and Linux
  `6.8.0-202601282310-nvidia-tegra-ppadev-rt`. Other releases require a matching
  source port and renewed DMA/lifecycle validation; the builder rejects them.
- One through four exclusive interfaces, with one through four RX/TX queue
  pairs on each. Queue IDs must be unique and contiguous from zero in both
  directions. `address` is the Linux netdev name (`mgbe0_0` through `mgbe3_0`),
  not a PCI address. The kernel advertises the ordinary DT DMA channels it can
  expose and rejects larger requests; camera/VM channels are excluded.
- Each direction supports `poll_mode: direct` or `poll_mode: indirect`, including
  mixed modes. One application thread owns each direct queue direction; separate
  queues and directions may use separate threads. Pin application threads to
  their intended CPUs. Direct queues omit `cpu_core`, `batch_size`
  and `timeout_us` and create no worker for that direction.
- Indirect queues require `cpu_core` and `batch_size` (1..64). Pinned pollers
  service DMA, with bounded preallocated handoff rings between pollers and
  application threads. All indirect queues assigned to the same core share one
  worker, including queues on different interfaces; different cores use
  separate workers. Each queue has its own polling budget and bounded backlog. Applications may pass distinct bursts between threads;
  each burst must have only one application owner at a time. Shutdown requires
  application threads to stop using the engine first.
- Two distinct `kind: host`, DAQIRI-owned memory regions per queue pair: one
  per direction, never shared between queues or ports. All regions on a port
  must have equal `num_bufs` and `buf_size`; different ports may differ. Pools are allocated by the kernel shim,
  rather than by the generic userspace host allocator. External bindings,
  GPU/host-pinned/huge memory and physical header/data splits are rejected.
- `num_bufs` sets the hardware ring size: a power of two from 64 through 4096.
  `buf_size` is the maximum L2 frame excluding FCS, from 82 through 9014 bytes;
  the configured IP MTU is `buf_size - 14`. The sample uses standard MTU 1500.
- Direct TX reserves one packet at a time; direct RX returns up to 64 packets.
  Indirect TX accepts 1..`batch_size` packets per submission, supports multiple
  unsubmitted reservations and batches DMA doorbells. Indirect RX publishes
  full batches, or partial batches after `timeout_us` from their first packet;
  zero disables the timeout and waits for a full batch.
- RX data is read directly from mapped DMA buffers in both modes. TX writes
  directly into mapped TX slots, with no worker payload copy. RX buffers remain
  application-owned until freed. Successful TX submission transfers burst
  ownership to the engine; failed submission leaves ownership with the caller.
  Hardware-owned TX slots are recycled only after completion. Metadata and
  payload exhaustion return the usual no-free-buffer statuses. A held RX buffer
  can block ring rearming; an exhausted RX handoff leaves packets on the bounded
  hardware ring, which can eventually drop new arrivals.
- No flow steering, flow isolation, reorder, pacing, accurate send, hardware
  timestamps, offloads or internal-loopback mode through this engine. All
  IPv4 UDP/TCP flows are distributed across the enabled RX queues by hardware
  RSS, with a per-port key and a balanced 128-entry table. Packets without an
  applicable hash enter queue zero. One flow stays on one queue; several flows
  are needed to spread receive work over cores. Applications filter traffic.
  Unsupported configuration fails the native validator before hardware opens.

Raw Ethernet frames are exposed directly, with no IP address assigned to Linux.
The Ethernet/IPv4/UDP header helpers fill the source MAC, calculate the IPv4
checksum in software and use a legal zero UDP checksum. Applications may also
construct complete raw frames themselves. Hardware timestamps and optional
runtime flow/resource operations return `NOT_SUPPORTED`.

## Selecting indirect mode

[`config/daqiri_mgbe_indirect.yaml`](config/daqiri_mgbe_indirect.yaml) uses one
worker on CPU 8 for both directions, batches of 32 and a 100-us partial RX
batch timeout. Choose an available CPU for your host and pin application work
on a separate core. Set different RX/TX `cpu_core` values for separate pollers.
A failed worker affinity request aborts initialization and releases the port.
Workers inherit the creating thread's scheduling policy; the engine sets their
CPU affinity. Isolate worker CPUs through host configuration if required.

```yaml
rx:
  queues:
    - name: RX
      id: 0
      poll_mode: indirect
      cpu_core: 8
      batch_size: 32
      timeout_us: 100
      memory_regions: [RX]
tx:
  queues:
    - name: TX
      id: 0
      poll_mode: indirect
      cpu_core: 8
      batch_size: 32
      memory_regions: [TX]
```

Use the normal DAQIRI burst API. Set the TX header's packet count to the desired
burst size before `get_tx_packet_burst()`, then fill and set lengths for every
packet before `send_tx_burst()`. `wait_for_tx_idle()` drains queued and posted
sends; it returns `RESOURCE_IN_USE` while the application holds unsubmitted TX
buffers. Link/session faults fail operations and require shutdown/restart.

## Multiple queues and interfaces

[`config/daqiri_mgbe_4q.yaml`](config/daqiri_mgbe_4q.yaml) enables RX/TX queue
pairs 0..3 on `mgbe0_0`, with workers on CPUs 8..11. Add interface blocks for
other MGBE ports, using distinct memory regions for every queue direction.
Interfaces receive DAQIRI port IDs in configuration order. Set the desired
port and queue in the TX burst header before reserving packet buffers, and
poll RX using that same `(port_id, queue_id)` API addressing. RX metadata
identifies the actual interface and queue. A burst bound to one queue cannot
be submitted on another by changing its header.

`rx_meta_buffers` sizes each RX queue's metadata pool; TX metadata is a shared
pool of `tx_meta_buffers` leases, assigned to a queue when packets are reserved.
Hardware rings, packet slots, RX handoffs, TX pending lists and completion
recycling are independent per queue. Held RX buffers or TX exhaustion on one
queue do not consume another queue's DMA slots. `wait_for_tx_idle()` drains all
queues; stop new submissions first. A link or DMA fault stops every DMA channel
on that port and invalidates the DAQIRI session, which requires shutdown/restart.

The control shim and userspace driver must both use **control ABI v2**. Rebuild
and reload the matching module and rebuild applications using the low-level
C++ driver. ABI v1 configure requests are rejected before DMA allocation. The
low-level `Device` accepts a queue count and exposes `queue(id)`; RSS key/table
queries and updates are available before start. DAQIRI uses the default hardware
RSS distribution; user-supplied flow rules remain unsupported.

To expose multiple ports, pass a comma-separated netdev list or `all` to the
module manager's `--interface` option. Acquisition remains exclusive per port,
and the existing guards still refuse module replacement while any MGBE port is
live/configured or a dependency is active. For example, after quiescing ports:

```bash
drivers/mgbe/container.sh --root python3 drivers/mgbe/manage_module.py load \
  --module "$PWD/build/mgbe-kernel-v2/nvethernet/nvethernet.ko" \
  --interface all --state "$PWD/build/mgbe-module.json" --unload-idle-coe
```

Multiple queues share their port's physical bandwidth. Throughput gains depend
on frame size, batching, flow distribution and CPU placement and must be measured
on the target system.

## Build and install DAQIRI

Use the repository's project container as described in the main build guide.
CUDA remains a DAQIRI build dependency even though this engine uses CPU memory.
Build as the normal user; root access is needed only for module management and
hardware runtime. The optional container wrapper supplies the project's normal
privileged NVIDIA build environment and exposes all GPUs.

```bash
# Build daqiri:local first using scripts/build-container.sh if needed.
docker build --network host -f drivers/mgbe/Dockerfile -t daqiri:mgbe .
drivers/mgbe/container.sh cmake -S . -B build/mgbe \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DDAQIRI_ENGINE="mgbe" -DDAQIRI_BUILD_PYTHON=OFF
drivers/mgbe/container.sh cmake --build build/mgbe -j8
drivers/mgbe/container.sh ctest --test-dir build/mgbe --output-on-failure

# Installation does not replace or load a kernel module.
sudo cmake --install build/mgbe --prefix /opt/daqiri
```

To retain ConnectX engines as well, use `-DDAQIRI_ENGINE="dpdk ibverbs mgbe"`.
The default build remains `dpdk ibverbs`. Python builds expose
`daqiri.EngineType.MGBE` and use the same YAML and packet API. CMake consumers
link `daqiri::daqiri`; the optional engine is installed as `libdaqiri_mgbe`.
Driver sources, configuration and management scripts are installed below
`share/daqiri/drivers/mgbe`.
For a non-system prefix, add its `lib` directory to `LD_LIBRARY_PATH`; Python
also needs its `lib/python` directory on `PYTHONPATH`.

## Build and install the kernel control shim

The shim patches a **copy** of the pinned NVIDIA/Canonical driver; it does not
modify the checked-out vendor source or `/lib/modules`. Preserve the GPL and
MIT notices in [NOTICE](NOTICE). The generated `vendor.patch` includes the
complete kernel integration and UAPI.

```bash
git clone --depth 1 --branch v2.4.0-1_igx-2.0 \
  https://git.launchpad.net/~ubuntu-tegra/+git/tegra-oot /tmp/tegra-oot
git -C /tmp/tegra-oot rev-parse HEAD
# Must be 85b5dd4a7b14c7311c651b4fd9e60515a813da0f.

# Install the running kernel's headers and IGX OOT headers first. The builder
# uses /lib/modules/$(uname -r)/build and the matching OOT Module.symvers.
drivers/mgbe/container.sh bash drivers/mgbe/build-kernel.sh \
  /tmp/tegra-oot build/mgbe-kernel-v2 patched

sudo install -d /opt/daqiri/lib/modules/"$(uname -r)"
sudo install -m 0644 build/mgbe-kernel-v2/nvethernet/nvethernet.ko \
  /opt/daqiri/lib/modules/"$(uname -r)"/nvethernet.ko
```

Use a new kernel build directory each time. The package stays outside the
kernel module search path, so `modprobe nvethernet` remains the stock rollback
path. No `depmod`, boot-time module replacement, device-tree change or firmware
flash is required. The board's configured link speed is unchanged by this driver.

## Acquire, run and restore

Module replacement temporarily removes **all four MGBE netdevs**. Use a
separate management interface. Record the original address/UP/MTU settings and
NetworkManager state before quiescing the data ports:

```bash
drivers/mgbe/container.sh --root python3 drivers/mgbe/manage_module.py inspect \
  > build/mgbe-before.json
```

Stop traffic and network management on these dedicated data ports, remove their
addresses, and bring them down before loading. The loader refuses every live
or configured MGBE port. It also refuses active camera/CoE users; the optional
flag below removes only verified idle `tegra_capture_coe`. The module journal
records the state **at load time**, after preparation, so restore any earlier
network-manager and UP/address changes from the pre-quiesce record yourself.

```bash
drivers/mgbe/container.sh --root python3 drivers/mgbe/manage_module.py load \
  --module /opt/daqiri/lib/modules/"$(uname -r)"/nvethernet.ko \
  --interface mgbe0_0 --state "$PWD/build/mgbe-module.json" --unload-idle-coe
```

If using the container, bind-mount `/opt/daqiri` to load the installed artifact,
or use `--module "$PWD/build/mgbe-kernel-v2/nvethernet/nvethernet.ko"` directly.
The control node is `/dev/mgbe-pmd-mgbe0_0`, mode 0600. Leave the selected
kernel netdev down. Configure a compatible cabled peer and establish carrier;
the application waits briefly for PHY training and refuses to start without it.

```bash
drivers/mgbe/container.sh build/mgbe/tools/daqiri_config_validate \
  drivers/mgbe/config/daqiri_mgbe.yaml
# The same validator accepts config/daqiri_mgbe_indirect.yaml.
drivers/mgbe/container.sh --root taskset -c 4 \
  build/mgbe/examples/daqiri_mgbe_raw_echo drivers/mgbe/config/daqiri_mgbe.yaml
```

The example returns unicast, unfragmented IPv4/UDP frames addressed to the port's
MAC, swaps the Ethernet/IP/UDP endpoints and preserves the payload. Stop it
with SIGINT or SIGTERM. Other applications use the normal DAQIRI lifecycle:
initialize, retrieve RX bursts/reserve TX, construct/send frames, explicitly free
RX bursts or canceled TX reservations, then shut down. A successful send consumes
the TX burst; an error retains it for explicit release. Free all outstanding
bursts before shutdown. Link loss faults the session and requires shutdown and
reinitialization; there is no automatic packet replay or copy-mode fallback.

```bash
# Stop the application first, releasing all open files and mappings.
drivers/mgbe/container.sh --root python3 drivers/mgbe/manage_module.py restore \
  --state "$PWD/build/mgbe-module.json"
```

Restoration reloads stock `nvethernet`, restores idle CoE and journaled MTUs,
ring sizes, offloads, coalescing, NAPI settings and IRQ affinities. Reapply your
pre-quiesce UP/address/NetworkManager settings and verify management connectivity.
The durable journal supports recovery after an interrupted session. The kernel
stops DMA before freeing mappings on application exit; never force-unload or
platform-unbind a driver with active mappings. Normal installed applications
can invoke the installed management script with host `sudo python3` instead of
the source-tree container wrapper.

## Correctness checks

The MGBE-enabled CTests cover configuration rejection before device acquisition,
RX ownership and out-of-order release, TX completion ownership/exhaustion,
direct ownership, concurrent indirect submissions, batching, partial RX timeouts,
mixed modes, worker faults/affinity failure, initialization failure and cleanup.
Driver mock-ring tests cover wraparound and payload reuse without hardware.
They do not establish wire delivery; use the native echo example with a cabled
peer for an installation smoke test. Neither tests nor builds load modules.

For cable-free DMA correctness validation, explicitly load the shim and run
`build/mgbe/tests/cpp/daqiri_mgbe_dma_test config.yaml [worker_cpu]` as root
(default worker CPU: 8). This standalone
hardware test enables internal MAC loopback through a test-only port adapter,
checks changing payloads across small/standard/jumbo frames and ring wraparound
in direct, indirect and mixed modes, including 32-packet TX batches. With a
multi-queue config, it tests each TX channel against every RX queue using
controlled RSS tables,
then closes each session. It is never registered with CTest, produces no
performance measurements, and does not establish cable/peer delivery. The
production `mgbe` engine continues to require carrier.

An intermittent first-frame RX visibility failure has been observed in cabled
runs on isolated cores. Its cause remains unresolved. A passing DMA or sustained
traffic test does not establish reliable cold startup; verify first-frame payload
integrity and timeout handling for the deployment as well.

Simultaneous four-interface MAC-loopback stress has also observed an early
packet loss alongside a MAC RX CRC counter increment. The full multi-port stress
matrix did not complete without errors; its cause is unresolved. Four-queue
single-port DMA and cabled delivery checks do not qualify sustained multi-port
operation or cold-start reliability.
