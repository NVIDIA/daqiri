# ResNet Application Roadmap

This file captures follow-up work from the native reorder migration so future
sessions can continue without relying on chat history.

## Current state

- The app reads DAQIRI-native `daqiri.cfg.interfaces[].rx.reorder_configs` as
  the RX reorder source of truth.
- The app-level `packetization:` block now carries only image framing facts and
  TX-only packet geometry.
- `resnet50_wire_loopback.yaml` is retained as an optional single-host p0↔p1
  convenience config with conservative `pacing_mbps: 50`.
- The hardware-reorder config uses a cyclic sequence field at bit 336. The NIC
  direct-places INT8, and the shared consumer conversion supplies FP16 to the
  catalog TensorRT engines. The path is validated on ConnectX-7 after enabling
  programmable flex parsing and cold-rebooting the host.
- Cross-host and cabled benchmark configs explicitly select the ibverbs raw engine;
  the runners persist engine, packet/burst, and inference summaries.
- `model_catalog.json` drives ResNet-18/34/50/101/152 and MobileNetV3 Small/Large
  export, TensorRT cache selection, and application feature dimensions.

## Remaining planned updates

1. **Complete the IGX Thor end-to-end publication sweep**

   - Run all seven catalog models for three 30-second repetitions over the
     Spark-to-Thor hardware-reorder path.
   - Record exact host, GPU, NIC, driver/container, engine, pacing, packet counts,
     drops, latency percentiles, and stable throughput windows.
   - Separate correctness smoke numbers from publishable performance numbers.

2. **Complexity cleanup from reviewer pass**
   - If MobileNet lands, rename generic pipeline pieces that are ResNet-specific
     only by history.
   - Deduplicate Python/tool constants for packet geometry where possible.

## Validation after each major update

Run these before committing substantial app/config changes:

1. Parse all app YAML configs.
2. Rebuild `daqiri_resnet50_inference` inside the TensorRT container.
3. Run an applicable hardware path for the target deployment.
4. Require `RX reorder ready`, `TrtRunner ready`, expected image count, zero
   dropped bursts, zero partial bursts, and matching TX/RX packet counters.
