# Onboarding Flow

## Intake

Start with a small set of questions unless the user already provided the answers:

1. What are you trying to prove first: working install, GPUDirect raw path, cabled throughput, hardware-loopback throughput, UDP/TCP baseline, or RoCE/RDMA?
2. Are you on the host, inside the DAQIRI container, or connecting to a remote system?
3. Is a ConnectX NIC visible, and is there a cabled loopback or peer host available?
4. Is an NVIDIA data-center/RTX GPU visible to the runtime?
5. Has DAQIRI already been built, and if so is it installed under `/opt/daqiri` or built under `./build*`?

If the user asks to "run benchmark" and does not know the answers, run local discovery commands where possible.

## Stage Detection

- Repo missing: guide clone and submodule setup.
- Repo present, no container image/build: build the container first unless the user needs bare metal.
- Container image/build present, no shell with hardware access: launch the privileged container.
- Binaries present, config has placeholders: discover PCIe addresses, netdevs, MACs, IPs, GPU UUIDs, CPU cores, and hugepage state; then update or generate YAML.
- Config present, no route/counter proof: verify the physical or namespace path.
- Prerequisites satisfied: run benchmark and interpret output.

## Discovery Commands

Use these as applicable. Some require host/root privileges.

```bash
git submodule status
docker images | grep daqiri
find . -maxdepth 3 -type f -path '*examples/daqiri_bench_*' -perm -111
ls -l /opt/daqiri/bin 2>/dev/null
nvidia-smi --query-gpu=index,name,uuid --format=csv
ibdev2netdev -v
lspci -nn | grep -Ei 'mellanox|nvidia|network|ethernet'
grep Huge /proc/meminfo
ip -br link
ip -br addr
```

## Build Defaults

Recommend the container-first build for most users:

```bash
BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
```

Use `BASE_IMAGE=torch` only when TensorRT/Torch example applications are required. Use bare-metal CMake only when the user explicitly needs a host install or cannot use the container.

## Container Defaults

For raw/RDMA benchmark execution, use a privileged hardware-visible container. Build as the current user; run benchmark setup/execution as root when needed.

Raw/installed benchmark shell:

```bash
docker run --rm -it --privileged \
  --runtime=nvidia \
  --network=host \
  -v /dev/hugepages:/dev/hugepages \
  daqiri:local bash
```

Source-mounted shell:

```bash
docker run --rm -it --privileged --network=host --pid=host --ipc=host \
  --gpus all \
  -v "$PWD:/work" \
  -v /dev/hugepages:/dev/hugepages \
  -v /tmp:/tmp \
  -w /work daqiri:local bash
```

## Engine and Stream Decision

- ConnectX plus cable/peer plus GPU: raw Ethernet GPUDirect first.
- ConnectX plus no cable/peer: raw Ethernet hardware loopback if supported.
- Need Linux TCP/UDP behavior: socket benchmark.
- Need RDMA/RoCE: `daqiri_bench_rdma` with `roce://` endpoints and `ibverbs` engine built.
- No usable NIC: software loopback smoke only.

## Completion Criteria

Onboarding is complete when the user has:

- a build or installed container/image path,
- a selected benchmark matching their goal and hardware,
- a concrete YAML config with placeholders replaced or generated,
- a successful smoke run or a clearly diagnosed prerequisite failure,
- performance interpretation rules for their selected path.
