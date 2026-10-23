#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
"""Measure the TensorRT-only ceiling and optionally append campaign CSV rows."""

import argparse
import ctypes
import csv
import json
import os
import time
from pathlib import Path

import tensorrt as trt
import torch

CATALOG = Path(__file__).resolve().parents[1] / "model_catalog.json"
TRT_TO_TORCH = {
    trt.DataType.FLOAT: torch.float32,
    trt.DataType.HALF: torch.float16,
    trt.DataType.INT8: torch.int8,
    trt.DataType.INT32: torch.int32,
}


def torch_dtype(dtype):
    if dtype not in TRT_TO_TORCH:
        raise SystemExit(f"unsupported TensorRT dtype {dtype!r}; campaign engines must use supported fp16 inputs")
    return TRT_TO_TORCH[dtype]


def benchmark(engine, context, batch, iterations, warmup, duration):
    names = [engine.get_tensor_name(i) for i in range(engine.num_io_tensors)]
    inputs = [name for name in names if engine.get_tensor_mode(name) == trt.TensorIOMode.INPUT]
    outputs = [name for name in names if engine.get_tensor_mode(name) == trt.TensorIOMode.OUTPUT]
    buffers = {}
    for name in inputs:
        shape = (batch,) + tuple(engine.get_tensor_shape(name))[1:]
        context.set_input_shape(name, shape)
        buffers[name] = torch.zeros(shape, dtype=torch_dtype(engine.get_tensor_dtype(name)), device="cuda")
    for name in outputs:
        shape = tuple(context.get_tensor_shape(name))
        buffers[name] = torch.zeros(shape, dtype=torch_dtype(engine.get_tensor_dtype(name)), device="cuda")
    for name, buffer in buffers.items():
        context.set_tensor_address(name, buffer.data_ptr())

    stream = torch.cuda.Stream()
    with torch.cuda.stream(stream):
        for _ in range(warmup):
            context.execute_async_v3(stream.cuda_stream)
    stream.synchronize()

    completed = 0
    start = time.perf_counter()
    if duration > 0:
        while time.perf_counter() - start < duration:
            with torch.cuda.stream(stream):
                for _ in range(100):
                    context.execute_async_v3(stream.cuda_stream)
            stream.synchronize()
            completed += 100
    else:
        with torch.cuda.stream(stream):
            for _ in range(iterations):
                context.execute_async_v3(stream.cuda_stream)
        stream.synchronize()
        completed = iterations
    elapsed = time.perf_counter() - start

    latencies = []
    with torch.cuda.stream(stream):
        for _ in range(min(completed, 200)):
            start = time.perf_counter()
            context.execute_async_v3(stream.cuda_stream)
            stream.synchronize()
            latencies.append((time.perf_counter() - start) * 1e3)
    latencies.sort()
    return {
        "img_s": batch * completed / elapsed,
        "elapsed": elapsed,
        "batches": completed,
        "p50": latencies[len(latencies) // 2],
        "p99": latencies[int(len(latencies) * 0.99) - 1],
    }


def main():
    with CATALOG.open() as catalog_file:
        catalog = json.load(catalog_file)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=sorted(catalog), default="resnet50")
    parser.add_argument("--engine", help="override catalog engine_path")
    parser.add_argument("--batches", default="32")
    parser.add_argument("--iters", type=int, default=500)
    parser.add_argument("--seconds", type=float, default=0,
                        help="run each batch size for at least this duration; overrides --iters")
    parser.add_argument("--warmup", type=int, default=50)
    parser.add_argument("--csv", help="append campaign-format rows")
    parser.add_argument("--rep", default="1")
    args = parser.parse_args()

    profile = catalog[args.model]
    engine_path = args.engine or str(profile["engine_path"])
    # On IGX, TensorRT can otherwise attach to the integrated GPU before
    # PyTorch establishes the CDI-selected discrete-GPU context.
    # TensorRT and PyTorch can load distinct CUDA-runtime instances. Select
    # device 0 through the shared runtime TensorRT uses, then initialize
    # PyTorch on the same CDI-exposed device.
    cudart = ctypes.CDLL("libcudart.so")
    if cudart.cudaSetDevice(0) != 0:
        raise SystemExit("cudaSetDevice(0) failed")
    torch.cuda.set_device(0)
    torch.cuda.init()
    with open(engine_path, "rb") as engine_file, trt.Runtime(trt.Logger(trt.Logger.WARNING)) as runtime:
        engine = runtime.deserialize_cuda_engine(engine_file.read())
    if engine is None:
        raise SystemExit(f"failed to deserialize {engine_path}")
    context = engine.create_execution_context()
    outputs = [engine.get_tensor_name(i) for i in range(engine.num_io_tensors)
               if engine.get_tensor_mode(engine.get_tensor_name(i)) == trt.TensorIOMode.OUTPUT]
    if len(outputs) != 1 or engine.get_tensor_shape(outputs[0])[-1] != profile["feature_dim"]:
        raise SystemExit(f"engine feature dimension does not match catalog for {args.model}")

    rows = []
    print(f"model={args.model} engine={engine_path} feature_dim={profile['feature_dim']}")
    for batch in (int(value) for value in args.batches.split(",")):
        result = benchmark(engine, context, batch, args.iters, args.warmup, args.seconds)
        print(f"batch={batch} img_s={result['img_s']:.2f} p50_ms={result['p50']:.3f} p99_ms={result['p99']:.3f}")
        rows.append([
            args.model, profile["feature_dim"], "trt_only", batch,
            f"{result['elapsed']:.6f}", args.rep, f"{result['img_s']:.2f}", result["batches"],
            f"{result['p50']:.3f}", f"{result['p99']:.3f}", "NA", "NA", "NA", "NA",
            "NA", "NA", "NA", "NA", "NA", "NA", "NA", "NA", "NA", "NA", "NA",
            "0", "0", f"trt_only-{args.model}-b{batch}-r{args.rep}",
        ])

    if args.csv:
        header = [
            "model", "feature_dim", "mode", "images_per_batch", "seconds", "rep", "img_s",
            "inference_batches", "lat_p50_ms", "lat_p99_ms", "tx_rc", "rx_rc", "tx_sent_frames",
            "rx_packets_phy", "remainder_dropped", "partial_bursts", "queue_full",
            "non_reordered_bursts", "rx_discards_phy", "wire_gbps", "wire_pps", "clock_sync_ok",
            "tx_to_first_delivery_ms", "tx_to_first_result_ms", "delivery_to_result_p50_ms",
            "reorder_gpu_kernel_total_us", "reorder_gpu_kernel_share_pct", "run_id",
        ]
        exists = os.path.exists(args.csv)
        with open(args.csv, "a", newline="") as output_file:
            writer = csv.writer(output_file)
            if not exists:
                writer.writerow(header)
            writer.writerows(rows)
        print(f"appended {len(rows)} row(s) to {args.csv}")


if __name__ == "__main__":
    main()
