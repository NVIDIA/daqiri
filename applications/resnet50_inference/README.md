# DAQIRI → TensorRT ResNet inference

End-to-end example: raw Ethernet RX with **config-based GPU reorder** (int8→fp16),
**SPSC-decoupled** RX producer / inference consumer, and TensorRT FP16 tensor-core
feature extraction.

Follow-up work lives in `ROADMAP.md` so future sessions can resume the remaining
ResNet/MobileNet/Thor benchmark plan without chat history.

## Primary setup: Spark-to-Spark (xhost)

| Role | Host | Config |
|------|------|--------|
| TX | `spark-stacked-01` (ncg-spark-0177) | `configs/resnet50_tx_spark_xhost.yaml` |
| RX+inference | `spark-stacked-02` (ncg-spark-7013) | `configs/resnet50_rx_spark_xhost.yaml` |

**One cable** p0↔p0 (a second cable is not used). Net prep:
`scripts/setup_spark_xhost_net.sh` on both hosts.

## Pipeline

```
TX host:  pcap / synthetic → pcap_tx_worker → wire (int8 samples + header seq)
RX host:  ibverbs raw RX + GPU reorder (int8→fp16, packets_per_batch=4096)
       → rx_producer_worker → InferenceQueue (SPSC)
       → inference_consumer_worker → TrtRunner (fp16 in) → FeatureSink
```

One reordered burst is one contiguous fp16 NCHW batch `[32,3,224,224]`. TRT reads
`get_packet_ptr(burst, 0)` zero-copy.

## Build

```bash
# Container with TensorRT
BASE_IMAGE=torch BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_ENGINE="dpdk ibverbs" -DDAQIRI_BUILD_APPLICATIONS=ON
cmake --build build -j --target daqiri_resnet50_inference
```

## Models, data + ONNX

`model_catalog.json` is the single source of truth for model architecture, ONNX and
engine-cache paths, feature dimension, and input type. The default catalog includes
ResNet-18/34/50/101/152 plus MobileNetV3 Small and Large. The app and
`run_resnet_bench.sh` both resolve `--model` through it.

```bash
python3 applications/resnet50_inference/tools/export_resnet_onnx.py --list-models
python3 applications/resnet50_inference/tools/export_resnet_onnx.py \
  --model mobilenet_v3_small --check
```

## Data + ONNX

```bash
python3 applications/resnet50_inference/tools/export_resnet_onnx.py \
  --model resnet50 --output models/resnet50_features.onnx
# Delete stale engines so TRT rebuilds with FLOAT32 features (fp16 input unchanged).
rm -f models/resnet50_features.fp16in.engine models/resnet50_features.fp16in.*.engine

python3 applications/resnet50_inference/tools/prepare_cifar10_pcap.py \
  --num-images 256 --images-per-batch 32 --out data/cifar10_resnet.pcap
```

Geometry (defaults): 128 pkts/image × 1176 int8 B; batch = 32 images → 4096 pkts;
seq at bit_offset 128, width 16; signed int8 = pixel − 128.

## Run (xhost helper)

```bash
./applications/resnet50_inference/tools/run_resnet_xhost.sh --replay-once
# or sustained:
./applications/resnet50_inference/tools/run_resnet_xhost.sh --seconds 20
```

Manual:

```bash
# RX first (stacked-02)
sudo ./build/applications/resnet50_inference/daqiri_resnet50_inference \
  ./build/applications/resnet50_inference/configs/resnet50_rx_spark_xhost.yaml \
  --mode rx --replay-once --expected-images 256

# TX (stacked-01)
# p0 netdev name is det1 on these Sparks (enp1s0f0np0 elsewhere)
export ETH_DST_ADDR=$(ssh spark-stacked-02 cat /sys/class/net/det1/address)
sudo -E ./build/applications/resnet50_inference/daqiri_resnet50_inference \
  ./build/applications/resnet50_inference/configs/resnet50_tx_spark_xhost.yaml \
  --mode tx --replay-once --dataset data/cifar10_resnet.pcap
```

Expect: `kHALF` input binding, `RX reorder ready`, per-class mean
features with CIFAR-10 class names, clean shutdown.

## Ibverbs benchmark sweep

The cabled synthetic benchmark config explicitly selects the ibverbs raw engine.
It runs every catalogued model by default and writes one CSV row per model/repeat:

```bash
export ETH_DST_ADDR=$(cat /sys/class/net/<rx-iface>/address)
BUILD_DIR=./build applications/resnet50_inference/tools/run_resnet_bench.sh
```

## Optional local cabled test

`resnet50_wire_loopback.yaml` provides a convenient single-host p0↔p1 test. It
is a real cabled raw Ethernet path (`loopback: ""`), uses the ibverbs raw engine,
and caps TX with `pacing_mbps: 50` so the test checks reorder/inference
correctness rather than receiver saturation.

```bash
# Replace <tx-pcie-addr>, <rx-pcie-addr>, and <rx-port-mac> for the host.
sudo ./build/applications/resnet50_inference/daqiri_resnet50_inference \
  ./build/applications/resnet50_inference/configs/resnet50_wire_loopback.yaml \
  --replay-once --dataset data/cifar10_resnet.pcap --expected-images 512
```

Expect `RX reorder ready`, `TrtRunner ready`, `pushed_images=512`,
`dropped_bursts=0`, `partial_bursts=0`, and RX/TX packet counters at 65536.
If a cached TensorRT engine was built for a different GPU, delete it or point
`inference.engine_path` at a host-local cache so TensorRT rebuilds it.

## Config keys (native reorder + app packetization)

RX reorder behavior now lives in DAQIRI's native
`daqiri.cfg.interfaces[].rx.reorder_configs` block. The application reads that
config to discover the reorder name, sequence field, payload offset, output
memory region, conversion type, and `packets_per_batch`; it no longer
synthesizes `reorder_configs` at startup.

The small app-level `packetization:` block carries only image framing facts:

| Key | Meaning |
|-----|---------|
| `reorder_name` | Native `rx.reorder_configs[].name` used by RX setup |
| `packets_per_image` | Number of packets that make one image; power of two |
| `images_per_batch` | Images handed to TensorRT from each reorder window |

TX-only configs have no RX reorder config to inspect, so they also declare the
wire geometry needed to generate or validate packets: `out_payload_len`,
`payload_byte_offset`, `seq_bit_offset`, and `seq_bit_width`. RX configs may
include those same keys as assertions, but the native reorder config remains
authoritative.

For the shipped FP16 path, the native reorder config uses `data_types:
input_type: int8, output_type: fp16`; for hardware NIC reorder, omit
`data_types`, set `reorder_engine: "hw"`, and use an INT8 TensorRT input.
`--images-per-batch` may shrink the TensorRT batch afterward without changing
the engine reorder window.

Hardware reorder samples the UDP payload, so its sequence field is at bit 336,
not the software path's header field at bit 128. Its 16-bit cyclic sequence
requires all 16 `Reorder_RX_GPU` buffers (4096 packets each) so every exact
sampled value has a hardware destination.

## Queue sizing vs. reorder batches

A reorder batch only reaches inference if it is delivered whole: a partially
flushed batch is truncated to whole images, so the image straddling the flush
boundary is lost. Three settings matter:

- `rx.queues[].timeout_us: 2000` — must stay **finite**. The accumulator fills
  by arrival order, and DAQIRI derives the batch id as `seq / packets_per_batch`.
  Without a timeout, one lost packet lets the next batch's first packet complete
  this one, shifting every subsequent batch by one packet for the rest of the run
  and misaligning image boundaries and labels. A finite timeout resyncs at the
  batch boundary instead. Trade-off: a flush landing mid-image drops the
  straggler packets, reported as `remainder_dropped`.
- `rx.queues[].batch_size` — set equal to `packets_per_batch` (4096) for latency
  measurements. Larger multiples remain correctness-safe, but the indirect
  software RX path may wait for the larger RX burst before handing completed
  reorder batches to the application, adding whole-batch delivery latency.
- `tx.queues[].batch_size` / `bench_tx.batch_size` — aligned to
  `packets_per_batch` (4096), so a gap between TX bursts falls on a batch
  boundary.

With those set, an xhost `--replay-once` run of 128 images delivers 4 whole
batches:

```text
pcap_tx_worker: tx_start unix_ns=1792692003228611087
rx_producer_worker: batch_delivered index=0 unix_ns=1792692004052194077 images=32
rx_producer_worker: delivered_bursts=4 pushed_images=128 dropped_bursts=0 [queue_full=0 short_batch=0 info_failure=0 null_input=0]
rx_producer_worker: partial_bursts=0 partial_packets=0 early_flush_shortfall=0 remainder_dropped=0 info_failures=0 non_reordered_bursts=0 non_reordered_packets=0
batch delivery->inference submit (us): first=57.834 mean=74.998 p50=57.834 p99=81.575 n=4
batch delivery->application-visible result (ms): first=822.421 mean=628.439 p50=822.421 p99=823.251 n=4
```

`batch_delivered` is captured immediately after DAQIRI returns a complete
reordered burst and before it is pushed to the inference SPSC queue. Subtract
the `tx_start` timestamp to compare first-batch delivery across reorder engines.
The application-visible result includes the runner's one-batch-delayed output
pipeline; the separate TensorRT `inference latency` line remains CUDA-event
timing for the model execution itself.

See `docs/tutorials/daqiri-resnet-inference.md`.
