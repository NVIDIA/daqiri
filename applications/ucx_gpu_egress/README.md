# DAQIRI → CUDA → UCX GPU egress

End-to-end example: DAQIRI receives packetized 256×256 `uint16` images over raw
Ethernet, a CUDA ordering kernel assembles sixteen images per batch, CUDA applies
an in-place scale/offset transform, and UCP Active Messages send the processed
images to a bounded GPU-accessible receive pool on another host.

The example is intentionally small and application-local. It does not add another
DAQIRI engine, multi-engine support, or a general reorder API.

## Primary setup: Spark-to-Spark

| Role | Host | Link 1: raw Ethernet | Link 2: UCX/RoCEv2 |
|---|---|---|---|
| Source + receiver | Host A (`.104`) | `det1`, BDF `0000:01:00.0` | `det4`, BDF `0002:01:00.1`, `10.55.1.1` |
| Processing + egress | Host B (`.103`) | `det1`, BDF `0000:01:00.0` | `det4`, BDF `0002:01:00.1`, `10.55.1.2` |

Use ports with different PCIe prefixes for the two simultaneous links. On this
rig, `det1`/`det4` is the proven pair; `det1`/`det3` shares a PCIe path.

## Pipeline

```text
Host A                                             Host B

DQRI raw source -- det1 / Link 1 ----------------> DAQIRI ibverbs raw RX
                                                       |
                                                RX + CUDA placement
                                                       |
                                                16-image ring slot
                                                       |
                                                CUDA scale/offset
                                                       |
GPU validation  <-- det4 / Link 2 / UCP AM -------- UCX progress
```

Each image is 131,072 bytes: sixteen 8,192-byte fragments. A 38-byte DQRI
application header carries the source epoch, batch ID, packet sequence, and
fragment slot. The local assembler accepts reordering within one 256-fragment
batch and rejects the whole batch on missing, duplicate, malformed, timed-out, or
cross-batch input. This avoids relying on DAQIRI's arrival-count reorder window.

The transform is *saturating*, not a claim that it saturates the GPU: each pixel
is computed with `fmaf(scale, pixel, offset)`, clamped to `[0, 65535]`, and rounded
to nearest-even before conversion back to `uint16_t`.

## Build

Build the same container image on both hosts:

```bash
BASE_TARGET=ucx DAQIRI_ENGINE=ibverbs DAQIRI_BUILD_APPLICATIONS=ON \
  DAQIRI_BUILD_RESNET50_INFERENCE=OFF DAQIRI_BUILD_UCX_GPU_EGRESS=ON \
  DAQIRI_BUILD_EXAMPLES=OFF IMAGE_TAG=daqiri:ucx-gpu-egress \
  scripts/build-container.sh
```

The `ucx` target builds UCX 1.20 with UCP, verbs, mlx5, RDMA-CM, and CUDA-memory
support under `/opt/ucx`. It inherits the repository's RDMA/DPDK dependency stage,
although `DAQIRI_ENGINE=ibverbs` does not select DPDK at runtime.

Run containers as root with `--privileged`, `--gpus all`, host networking,
unlimited memlock, and the host hugepage mount. Copy `configs/raw_source.yaml` and
`configs/raw_processor.yaml` to a writable directory and replace every
`<PLACEHOLDER>` first.

## Prove UCX first

Run the shared producer/receiver transport without DAQIRI. This exercises the same
batch-slot producer used by the composed pipeline.

```bash
# Host B: listener/producer
UCX_NET_DEVICES=mlx5_3:1 UCX_IB_GID_INDEX=11 UCX_PROTO_INFO=y \
daqiri_ucx_gpu_transport_bench --mode producer --listen 10.55.1.2:13341 \
  --images 1000000 --queue-depth 256 --batch-slots 16 --gpu-id 0 \
  --cpu-core 16 --memory-kind host_pinned_mapped --credit-mode wait

# Host A: connecting receiver
UCX_NET_DEVICES=roceP2p1s0f1:1 UCX_IB_GID_INDEX=3 UCX_PROTO_INFO=y \
daqiri_ucx_gpu_transport_bench --mode receiver \
  --connect 10.55.1.2:13341 --local 10.55.1.1:0 \
  --images 1000000 --queue-depth 256 --gpu-id 0 --cpu-core 18 \
  --memory-kind host_pinned_mapped
```

`host_pinned_mapped` is the verified Spark mode. `cuda_device` remains supported
for later testing on a system where protocol traces and NIC/PCIe counters can prove
GPUDirect RDMA; accepting a CUDA pointer is not that proof.

## Run the composed pipeline

1. Start Host B and wait for `listener_ready`:

   ```bash
   UCX_NET_DEVICES=mlx5_3:1 UCX_IB_GID_INDEX=11 \
   daqiri_ucx_raw_pipeline /configs/raw_processor.yaml \
     --stage egress --batches 100000
   ```

2. Start the Host A receiver with `--images 1600000`, `--queue-depth 256`, and
   `--validation raw-transform --scale 1.25 --offset -32`.

3. After Host B prints `receiver_ready` and then `ingress_ready`, start Link 1:

   ```bash
   daqiri_ucx_raw_source /configs/raw_source.yaml --batches 100000
   ```

The three Host B application threads own the ring in sequence:

```text
RX/placement --placement_done--> processing --producer event--> UCX/retirement --> RX
```

The move-only `BatchLease` is consumed by submit, cancel, or unused-slot release.
The producer may reuse a slot only after every admitted UCP send completes and the
RX owner observes its generation-checked retirement. On Host A, receive completion
delivers a move-only `ReceivedImage`; `release_after(stream)` returns its credit only
after downstream CUDA work on that stream finishes.

## Useful modes and expected output

- `daqiri_ucx_raw_pipeline --stage assemble`: raw RX + ordering/placement + GPU
  validation.
- `--stage process`: adds the batched in-place transform.
- `--stage egress`: adds UCP and uses the remote receiver for validation.
- `daqiri_ucx_gpu_transport_bench`: UCX-only dummy-image transfer.

At `pacing_mbps: 95000` and receive depth 256, the lab delivered and GPU-validated
1.6 million processed images at 94.94 Gbit/s payload with zero drops or gaps. An
unrestricted source overloads Link 2 because the raw framing requires about
98.7 Gbit/s of image payload before RoCE/UCP overhead; expected no-credit drops are
reported as receiver sequence gaps.

Use `mlnx_perf -i <netdev> -t 1` on both links for at least ten seconds. Discard
startup/shutdown samples and report stable one-second PHY rates separately from the
application's DATA-completion interval. The short 4,096-image smoke result is not
directly comparable to sustained runs.

## Scope and limitations

- One producer, one receiver, one stream, and a fixed image count per run.
- Drop newest before UCP submission when no receiver credit is available; no replay.
- Fail-stop on disconnect; restart both endpoints for a new connection epoch.
- One lost raw fragment rejects sixteen images in the simple batch agreement.
- The deterministic test payload repeats most pixels every sixteen images; the CPU
  DQRI batch/fragment checks protect cross-batch identity, while GPU validation checks
  placement offsets and transform results.
- UCP/RoCE and header CRCs provide neither authentication nor encryption.

For the full walkthrough, configuration fields, ownership states, and validation
sequence, see [DAQIRI + UCX GPU Egress](../../docs/tutorials/daqiri-ucx-gpu-egress.md).
