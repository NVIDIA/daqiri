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
- The hardware-reorder config and its INT8 ONNX/engine and sequence-at-336 PCAP
  are prepared, but the hardware path is **untested**: the Thor RX HCA needs
  programmable flex parsing enabled and a cold reboot before it can initialize.
- Cross-host and cabled benchmark configs explicitly select the ibverbs raw engine;
  the runners persist engine, packet/burst, and inference summaries.
- `model_catalog.json` drives ResNet-18/34/50/101/152 and MobileNetV3 Small/Large
  export, TensorRT cache selection, and application feature dimensions.

## Remaining planned updates

1. **IGX Thor publishable benchmark pass** *(deferred until the device receives traffic from a peer)*

   - Use cabled loopback or cross-host hardware paths only; do not use software
     loopback for publishable numbers.
   - Record exact host, GPU, NIC, driver/container, engine, pacing, packet counts,
     drops, latency percentiles, and stable throughput windows.
   - Separate correctness smoke numbers from publishable performance numbers.

2. **Complexity cleanup from reviewer pass**
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
