<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
All rights reserved. SPDX-License-Identifier: Apache-2.0
-->

# DAQIRI → TensorRT ResNet inference example

End-to-end demo wiring DAQIRI packet ingestion into a GPU inference pipeline
(GitHub issue #73):

```
received packets (raw / DPDK GPUDirect)
  → GPU sequence-number reorder (image reassembly)
  → ResNet feature extraction (TensorRT, FP16)
  → per-class mean-feature stats (example mode)
```

Packets DMA straight into GPU memory, a CUDA kernel reorders each image's
packets by sequence number into a contiguous FP32 NCHW tensor, TensorRT runs a
ResNet feature extractor on a batch of those tensors, and the feature vectors
are summarized per class — all without a host bounce on the data path.

The example runs over the **DGX Spark physical p0→p1 cabled loopback** (the same
two-physical-port DPDK loopback the performance report uses), so it exercises the
real NIC RX path rather than a software shortcut. The build, CMake target, and
CUDA-arch handling are platform-agnostic (x86_64 + aarch64 TensorRT discovery),
so the same code builds on IGX and RTX Pro servers.

## Prerequisites

Build the project container with TensorRT (and torch / torchvision for the data
prep), which the `torch` base image provides:

```bash
BASE_IMAGE=torch BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
```

## Build

Enable the applications tree (off by default; it requires TensorRT):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DDAQIRI_ENGINE="dpdk ibverbs" -DDAQIRI_BUILD_APPLICATIONS=ON
cmake --build build -j
cmake --install build --prefix /opt/daqiri
```

If TensorRT is not found the application is skipped with a CMake warning (the
rest of the build is unaffected).

## Prepare the model and dataset (offline, one-time)

```bash
# 1. Export a ResNet feature extractor (final FC stripped) to ONNX.
python3 applications/resnet50_inference/tools/export_resnet_onnx.py \
  --model resnet50 --output models/resnet50_features.onnx --check

# 2. Packetize CIFAR-10 into a replayable pcap (+ a labels sidecar).
python3 applications/resnet50_inference/tools/prepare_cifar10_pcap.py \
  --num-images 256 --out data/cifar10_resnet.pcap
```

The TensorRT engine itself is built and cached on the first run
(`models/resnet50_features.fp16.engine`) and loaded from cache afterwards.

## Run (example mode — real images + per-class stats)

Bring the wire loopback's network namespaces **down** (they hide the ports from
DPDK) and export the RX port MAC, exactly as for the raw/DPDK bench:

```bash
./scripts/setup_spark_wire_loopback_netns.sh down
export ETH_DST_ADDR=$(cat /sys/class/net/<rx-iface>/address)   # RX port (p1)
```

Fill the `<tx-pcie-addr>` / `<rx-pcie-addr>` placeholders in
`configs/resnet50_wire_loopback.yaml` (TX = p0, RX = p1), then:

```bash
./build/applications/resnet50_inference/daqiri_resnet50_inference \
  ./build/applications/resnet50_inference/configs/resnet50_wire_loopback.yaml \
  --dataset data/cifar10_resnet.pcap --seconds 10
```

Expected output: the engine builds + caches on the first run (loads from cache
on the next), a couple of sample feature vectors print, and at shutdown a
per-class mean-feature summary prints. Confirm nonzero RX with `mlnx_perf`.

Example mode replays the dataset **once** by default (use `--loop` to repeat):
the whole dataset is buffered in the RX ring, so even though ResNet inference is
slower than line rate no packets drop and the in-order, drop-free image
reassembly (and label mapping) stays correct. Keep
`num_images * packets_per_image < num_bufs` (the default 256 × 84 = 21 504 fits
the 51 200-buffer ring).

## Run (benchmark mode — synthetic, throughput sweep)

```bash
export ETH_DST_ADDR=$(cat /sys/class/net/<rx-iface>/address)
BUILD_DIR=./build ./applications/resnet50_inference/tools/run_resnet_bench.sh
```

Sweeps ResNet-18/34/50/101/152 (FP16) and writes `resnet-bench.csv` (img/s per
model). No dataset is needed; the TX synthesizes frames so this measures the
full reorder + inference receive-path throughput.

## How it works

See the tutorial `docs/tutorials/daqiri-resnet-inference.md` for the CUDA-event
buffer logic (how bursts are ingested and freed without stalling the GPU) and a
flow diagram. The short version:

```
p0 --cable--> p1 -> get_rx_burst -> [reorder-scatter into image buffer (stream s)]
   -> [D2D image -> NCHW batch + input_ready] -> [TrtRunner enqueueV3(s) + release_evt]
   -> [D2H + d2h_event] -> FeatureSink -> per-class stats / counts
                              |
        cudaStreamSynchronize(s) BEFORE free_all_packets_and_burst_rx
```

One CUDA stream serializes reorder → batch-copy → inference, so no explicit sync
is needed between them; the only barrier is a `cudaStreamSynchronize` before each
burst is freed, because the reorder kernel reads the burst's device pointers.

## Files

| File | Role |
|---|---|
| `main.cpp` | arg parse, `daqiri_init`, spawn TX + RX threads |
| `app_config.{h,cpp}` | YAML parse + derived reorder geometry |
| `pcap_replayer.{h,cpp}` | pcap reader, dst-MAC patch, TX worker, synthetic frames |
| `trt_runner.{hpp,cu}` | TRT engine build/cache + FP16 dynamic-batch inference |
| `inference_pipeline.{h,cu}` | RX loop: reorder → NCHW batch → TRT |
| `feature_sink.{h,cpp}` | per-class mean-feature stats / throughput |
| `tools/export_resnet_onnx.py` | torchvision ResNet → ONNX (feature extractor) |
| `tools/prepare_cifar10_pcap.py` | CIFAR-10 → preprocessed framed pcap + labels |
| `tools/run_resnet_bench.sh` | model-size throughput sweep → CSV |
| `configs/*.yaml` | wire-loopback example + benchmark configs |
