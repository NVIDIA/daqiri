# ResNet Application Roadmap

This file captures follow-up work from the native reorder migration so future
sessions can continue without relying on chat history.

## Current state

- The app reads DAQIRI-native `daqiri.cfg.interfaces[].rx.reorder_configs` as
  the RX reorder source of truth.
- The app-level `packetization:` block now carries only image framing facts and
  TX-only packet geometry.
- The commit smoke path is cabled Thor p0↔p1 using `resnet50_wire_loopback.yaml`,
  the ibverbs raw engine, and conservative `pacing_mbps: 50`.

## Remaining planned updates

1. **NIC hardware reorder path**
   - Add a hardware-reorder ResNet RX config using `reorder_engine: "hw"`,
     `cyclic_sequence: true`, and the required packet sizing fields.
   - Keep software reorder configs as correctness/perf comparison baselines.
   - Verify the hardware path uses INT8 TensorRT input because hardware reorder
     does placement only; it does not perform app-level int8→fp16 conversion.

2. **Ibverbs backend benchmark coverage**
   - Add or extend benchmark configs/scripts so ResNet smoke and publish runs can
     exercise ibverbs raw explicitly, not only DPDK raw.
   - Capture packet counters, drop counters, burst counters, and inference
     summaries in machine-readable output where practical.

3. **MobileNetV3 small and large benchmarks**
   - Add export/build/run coverage for MobileNetV3 Small and Large using the
     same DAQIRI packetization path.
   - Avoid duplicating model metadata in multiple places; prefer a small model
     catalog for ONNX path, engine-cache path, feature dimension, and input type.
   - Update tutorial/performance docs once cabled benchmark numbers are stable.

4. **IGX Thor publishable benchmark pass**
   - Use cabled loopback or cross-host hardware paths only; do not use software
     loopback for publishable numbers.
   - Record exact host, GPU, NIC, driver/container, engine, pacing, packet counts,
     drops, latency percentiles, and stable throughput windows.
   - Separate correctness smoke numbers from publishable performance numbers.

5. **Complexity cleanup from reviewer pass**
   - Consider a model catalog to remove scattered `--model` conditionals.
   - Add machine-readable final summaries to reduce grep/sed parsing in runners.
   - If MobileNet lands, rename generic pipeline pieces that are ResNet-specific
     only by history.
   - Deduplicate Python/tool constants for packet geometry where possible.

## Smoke gate after each major update

Run these before committing substantial app/config changes:

1. Parse all app YAML configs.
2. Rebuild `daqiri_resnet50_inference` inside the TensorRT container.
3. Run Thor cabled p0↔p1 smoke from `resnet50_wire_loopback.yaml` with host PCIe
   addresses and RX MAC filled in.
4. Require `RX reorder ready`, `TrtRunner ready`, expected image count, zero
   dropped bursts, zero partial bursts, and matching TX/RX packet counters.
