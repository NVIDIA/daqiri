# DAQIRI → TensorRT ResNet inference

End-to-end example: raw Ethernet RX with **config-based GPU reorder** (int8→fp16),
**SPSC-decoupled** RX producer / inference consumer, and TensorRT FP16 tensor-core
feature extraction.

## Primary setup: Spark-to-Spark (xhost)

| Role | Host | Config |
|------|------|--------|
| TX | `spark-stacked-01` (ncg-spark-0177) | `configs/resnet50_tx_spark_xhost.yaml` |
| RX+inference | `spark-stacked-02` (ncg-spark-7013) | `configs/resnet50_rx_spark_xhost.yaml` |

**One cable** p0↔p0 (a second cable is not used). Net prep:
`scripts/setup_spark_xhost_net.sh` on both hosts.

New here? Run [software loopback smoke](#software-loopback-smoke-no-nic) first
(no NIC required), then come back to xhost for reported numbers.

## Pipeline

```
TX host:  pcap / synthetic → pcap_tx_worker → wire (int8 samples + header seq)
RX host:  DPDK RX + reorder_configs (int8→fp16, packets_per_batch=4096)
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
seq at bit_offset 128, width 12; signed int8 = pixel − 128.

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

Expect: `kHALF` input binding, `set_reorder_cuda_stream OK`, `pc1=`/`pc2=` lines on
stdout every `inference.pca_every_n_batches` batches (default 8), per-class mean
features with CIFAR-10 class names, clean shutdown.

## Software loopback smoke (no NIC)

```bash
sudo ./build/applications/resnet50_inference/daqiri_resnet50_inference \
  ./build/applications/resnet50_inference/configs/resnet50_sw_loopback.yaml \
  --replay-once --dataset data/cifar10_resnet.pcap --expected-images 256
```

Uses `loopback: "sw"` / `address: "loopback"` (same pattern as
`daqiri_bench_raw_sw_loopback.yaml`). No PCIe placeholders or cable required.

## Config keys (app `reorder:`)

| Key | Default | Meaning |
|-----|---------|---------|
| `reorder_name` | — | Must match `rx.reorder_configs[].name` |
| `out_payload_len` | 1176 | Wire int8 bytes/packet |
| `output_slot_stride` | 2352 | fp16 bytes/slot |
| `packets_per_image` | 128 | Power of two |
| `packets_per_batch` | 4096 | = images_per_batch × packets_per_image |
| `images_per_batch` | 32 | Power-of-two divisor |
| `seq_bit_offset` / `seq_bit_width` | 128 / 12 | Header seq field |

## Queue sizing vs. reorder batches

A reorder batch only reaches inference if it is delivered whole: a partially
flushed batch is truncated to whole images, so the image straddling the flush
boundary is lost. Three settings matter:

- `rx.queues[].timeout_us: 0` — disables partial flushes. The dataset is padded
  to whole batches, so a flush can only lose data, and any finite timeout loses
  the race when the RX poller stalls mid-batch (inference backpressure, cold
  start, CPU contention). Trade-off: a batch missing a packet is never
  delivered, costing 32 images instead of 31.
- `rx.queues[].batch_size` — a multiple of `packets_per_batch` (8192 for 4096),
  so a batch never straddles an RX burst boundary.
- `tx.queues[].batch_size` / `bench_tx.batch_size` — aligned to
  `packets_per_batch` (4096), so a gap between TX bursts falls on a batch
  boundary.

With those set, an xhost `--replay-once` run of 256 images delivers 8 whole
batches: `pushed 256 images (reordered_bursts=8 partial=0 dropped=0)`.

See `docs/tutorials/daqiri-resnet-inference.md`.
