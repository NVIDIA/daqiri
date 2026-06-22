---
hide:
  - navigation
---

# DAQIRI + Holoscan Integration

This tutorial demonstrates how the DAQIRI library can be integrated into an NVIDIA
Holoscan application. The [Holoscan SDK](https://developer.nvidia.com/holoscan-sdk)
is NVIDIA's sensor-processing platform optimized for real-time GPU processing and
AI inferencing, with applications in scientific computing, healthcare, medical
robotics, and more.

This tutorial shows a minimal C++ example demonstrating the main principles of
using DAQIRI in a Holoscan application: it ingests a raw Ethernet stream, uses
DAQIRI's GPU reorder/quantize plan to turn it into sequence-ordered, quantized
batches, and emits each batch as a GPU tensor into a Holoscan pipeline. The snippets
below link against both `libdaqiri` and `libholoscan`; a complete, build-tested
project with a working CMake setup lives in Holohub (see [References](#references)).

To achieve peak IO performance, be sure to review the
[system configuration](system_configuration.md) tutorial. It is also a good idea to
confirm the DAQIRI YAML works with the standalone benchmarks in
[Raw Ethernet Benchmarking](../benchmarks/raw_benchmarking.md) and to review the
[configuration walkthrough](configuration-walkthrough.md) before wiring DAQIRI into
Holoscan.

## Application Lifecycle

A Holoscan application is a directed acyclic graph of operators — the fundamental
units of work — connected by data flows and run by a scheduler. DAQIRI owns NIC
setup, packet memory, and RX burst lifetimes; the operators in the graph pull
received data from DAQIRI and pass it downstream as tensors.

The recommended Holoscan pattern is to pass one YAML file into the application
with `app->config(...)`, read Holoscan operator parameters with `from_config(...)`,
and initialize DAQIRI from the same YAML during graph composition before any
DAQIRI-backed operators run. DAQIRI handles any configured GPU reorder/quantize
plan internally; the Holoscan source operator is just the adapter that polls DAQIRI
and emits completed batches to downstream operators.

```cpp
#include <daqiri/daqiri.h>
#include <holoscan/holoscan.hpp>

#include <filesystem>
#include <stdexcept>
#include <utility>

class DaqiriHoloscanApp : public holoscan::Application {
 public:
  void set_config_path(std::filesystem::path config_path) {
    config_path_ = std::move(config_path);
  }

  // compose() wires up the operator graph. Holoscan calls it once when the
  // application starts, before the scheduler begins running operators.
  void compose() override {
    // Initialize DAQIRI from the same YAML file passed to app->config(...). This
    // sets up the NIC, packet memory pools, RX queues, and any reorder/quantize
    // plan described in the `daqiri` block before DAQIRI-backed operators run.
    auto status = daqiri::daqiri_init(config_path_.string());
    if (status != daqiri::Status::SUCCESS) {
      throw std::runtime_error("DAQIRI initialization failed");
    }

    // Drive the graph with a multithreaded scheduler. High-bandwidth DAQIRI
    // pipelines must overlap IO with processing: the RX source operator has to keep
    // polling the NIC while downstream operators work on earlier batches. A
    // single-threaded (greedy) scheduler would serialize them. Size the worker
    // count to the number of operators that can run at once / available cores.
    scheduler(make_scheduler<holoscan::MultiThreadScheduler>(
        "multithread", from_config("scheduler")));

    // Source operator: polls DAQIRI for completed reordered+quantized batches. Its
    // parameters come from the `daqiri_rx` block of the YAML below.
    auto rx = make_operator<DaqiriRxOp>("daqiri_rx", from_config("daqiri_rx"));
    // Downstream operator that consumes the emitted tensors.
    auto sink = make_operator<TensorSinkOp>("tensor_sink");

    // Connect rx's "out" port to sink's "in" port, forming a two-node graph.
    add_flow(rx, sink, {{"out", "in"}});
  }

 private:
  std::filesystem::path config_path_;
};

int main(int argc, char** argv) {
  // Expect a single argument: the path to the shared YAML config file.
  if (argc != 2) {
    return 1;
  }

  const std::filesystem::path config_path{argv[1]};

  // Build the Holoscan application and hand it the same YAML file. Holoscan reads
  // its own keys (scheduler, operator parameter blocks) and ignores the `daqiri`
  // block.
  auto app = holoscan::make_application<DaqiriHoloscanApp>();
  app->set_config_path(config_path);
  app->config(config_path.string());
  // Runs the operator graph until the application is stopped. See "Running and
  // stopping" below for how a polling RX source terminates.
  app->run();

  // Print DAQIRI's RX/TX counters, then tear down the NIC and free packet memory.
  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
```

The shared YAML file carries Holoscan parameters (the `scheduler` block and each
operator's parameter block) alongside the `daqiri` block passed to
`daqiri_init(...)`. DAQIRI ignores Holoscan-only keys, and Holoscan operators read
their own parameter blocks with `from_config(...)`.

```yaml
# Multithreaded scheduler: runs the IO source operator and downstream processing on
# separate worker threads so ingestion and compute overlap.
scheduler:
  worker_thread_number: 4        # size to the operators that can run concurrently
  stop_on_deadlock: true         # return from run() once the graph goes idle
  stop_on_deadlock_timeout: 500  # ms to wait before declaring the graph idle

daqiri:
  cfg:
    version: 1
    stream_type: "raw"
    master_core: 3
    log_level: "info"
    memory_regions:
      - name: rx_gpu             # raw RX packet buffers the NIC DMAs into
        kind: device
        affinity: 0
        num_bufs: 16384
        buf_size: 2048
      - name: reorder_gpu        # holds one reordered + quantized output batch each
        kind: device
        affinity: 0
        num_bufs: 128
        buf_size: 4063232        # >= packets_per_batch * per-packet output bytes
    interfaces:
      - name: rx_port
        address: "<0000:00:00.0>"   # put the correct PCIe address of the NIC here
        rx:
          flow_isolation: true
          queues:
            - name: rx_q0
              id: 0
              cpu_core: 9
              batch_size: 256
              timeout_us: 20000
              memory_regions:
                - rx_gpu
          flows:
            - name: flow_0          # steer matching UDP packets to queue 0
              id: 201
              action:
                type: queue
                id: 0
              match:
                udp_src: 5000
                udp_dst: 5000
          # GPU reorder + quantize plan. DAQIRI reorders packets by their in-payload
          # sequence/batch number into a contiguous, gap-filled buffer and converts
          # the payload from int4 to fp32 on the GPU, writing one batch into
          # reorder_gpu. The operator just consumes the finished batch.
          reorder_configs:
            - name: rx_reorder_quantize
              reorder_type: gpu
              memory_region: reorder_gpu
              payload_byte_offset: 64     # header bytes before the payload
              flow_ids:
                - 201
              data_types:
                input_type: int4          # on-the-wire payload format
                output_type: fp32         # quantized output dtype
                endianness: host
              method:
                seq_batch_number:
                  sequence_number:        # bits locating the per-packet sequence no.
                    bit_offset: 128
                    bit_width: 10
                  batch_number:           # bits locating the batch no.
                    bit_offset: 144
                    bit_width: 2

# Parameters for the Holoscan DAQIRI source adapter, read via from_config("daqiri_rx").
daqiri_rx:
  interface_name: rx_port
  reorder_name: rx_reorder_quantize   # must match a reorder_configs[].name above
```

Replace the placeholder PCIe address, CPU cores, flow match fields, and bit-field
offsets with values for your stream. The DAQIRI reorder plan does the heavy lifting;
the operator's own parameter block (`daqiri_rx`) is just the interface name and the
name of the reorder config whose CUDA stream it drives. See the
[configuration walkthrough](configuration-walkthrough.md) and
[Configuration YAML Reference](../api-reference/configuration.md) for the full
reorder/quantize schema.

### Running and stopping

The RX operator is a *source*: it has no input ports, so under the multithreaded
scheduler its `compute()` is invoked continuously on a worker thread, polling the
NIC while other workers run downstream operators. With `stop_on_deadlock: true` the
scheduler returns from `app->run()` once no operator can make progress for
`stop_on_deadlock_timeout` ms (e.g. the stream ends); `SIGINT` (Ctrl-C) also stops
it. Either way control returns to `main()` and the teardown prints stats and shuts
DAQIRI down. To bound a run explicitly — for a smoke test — attach a stop condition
such as `make_condition<holoscan::CountCondition>(num_batches)` to the operator in
`compose()`.

## RX Operator Skeleton

The source operator resolves the configured DAQIRI interface, binds its CUDA stream
to the named reorder plan, and on each `compute()` call dequeues one RX burst. When
the burst is a completed reorder batch, it fences on the burst's CUDA event, reads
the batch metadata, and emits the reordered+quantized device buffer as a tensor.

```cpp
#include <daqiri/daqiri.h>
#include <holoscan/holoscan.hpp>

#include <cuda_runtime.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

// Emits each completed, sequence-ordered, quantized batch from DAQIRI's GPU reorder
// plan as a Holoscan tensor.
class DaqiriRxOp : public holoscan::Operator {
 public:
  // Boilerplate that lets Holoscan construct the operator from the YAML config.
  HOLOSCAN_OPERATOR_FORWARD_ARGS(DaqiriRxOp)

  // Release the CUDA stream created in initialize().
  ~DaqiriRxOp() override {
    if (stream_ != nullptr) {
      cudaStreamDestroy(stream_);
    }
  }

  // setup() declares the operator's parameters (read from the `daqiri_rx` block)
  // and its output port. It runs before initialize().
  void setup(holoscan::OperatorSpec& spec) override {
    spec.param(interface_name_, "interface_name", "Interface name",
               "Name of the DAQIRI interface to receive from");
    spec.param(reorder_name_, "reorder_name", "Reorder config name",
               "reorder_configs[].name whose CUDA stream this operator drives");

    // Single output port named "out" carrying a map of named tensors.
    spec.output<holoscan::TensorMap>("out");
  }

  // initialize() runs once after graph composition. DAQIRI was initialized from
  // the shared YAML in compose(), so here we resolve the interface and attach
  // our CUDA stream to the reorder plan.
  void initialize() override {
    holoscan::Operator::initialize();

    // Resolve the configured interface name to a DAQIRI port id. A negative id
    // means the name was not found.
    port_id_ = daqiri::get_port_id(interface_name_.get());
    if (port_id_ < 0) {
      throw std::runtime_error("DAQIRI interface was not found");
    }

    // The GPU reorder+quantize kernel runs on a CUDA stream we own. Binding our
    // stream to the named reorder plan means every reordered burst we dequeue was
    // produced on this stream, and its completion event is recorded on it.
    cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
    if (daqiri::set_reorder_cuda_stream(interface_name_.get(), reorder_name_.get(),
                                        stream_) != daqiri::Status::SUCCESS) {
      throw std::runtime_error("set_reorder_cuda_stream failed");
    }
  }

  // compute() is invoked continuously by the scheduler. Each call drains at most one
  // burst and, if it is a finished reorder batch, emits it as a tensor.
  void compute(holoscan::InputContext&,
               holoscan::OutputContext& op_output,
               holoscan::ExecutionContext&) override {
    // Dequeue one RX burst from queue 0. With a reorder plan active, completed
    // reorder batches are delivered as bursts flagged DAQIRI_BURST_FLAG_REORDERED.
    daqiri::BurstParams* burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id_, 0) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      return;  // Nothing ready this tick.
    }
    auto burst_deleter = [](daqiri::BurstParams* owned_burst) {
      daqiri::free_all_packets_and_burst_rx(owned_burst);
    };
    std::unique_ptr<daqiri::BurstParams, decltype(burst_deleter)> burst_guard(
        burst, burst_deleter);

    // Skip anything that is not a finished reorder batch (e.g. raw passthrough
    // bursts). The flag lives in the burst header.
    const bool reordered =
        (burst->hdr.hdr.burst_flags & daqiri::DAQIRI_BURST_FLAG_REORDERED) != 0U;
    if (!reordered) {
      return;
    }

    // The reorder/quantize kernel filled the batch asynchronously on our stream; the
    // burst carries the completion event. Fence on it before reading the data.
    if (burst->event != nullptr) {
      cudaEventSynchronize(burst->event);
    }

    // Batch metadata: how many sequence slots, the per-slot output length, and the
    // total reordered buffer size.
    daqiri::ReorderBurstInfo info{};
    if (daqiri::get_reorder_burst_info(burst, &info) != daqiri::Status::SUCCESS) {
      return;
    }

    // The reordered + quantized batch is one contiguous *device* buffer, exposed as
    // "packet 0" of the burst (its length equals info.aggregate_len).
    void* batch = daqiri::get_packet_ptr(burst, 0);

    // Wrap that device buffer as a tensor and emit it zero-copy. The tensor takes
    // ownership of the burst and frees it via the DLPack deleter once downstream is
    // done — so we do NOT free the burst here.
    auto tensor = wrap_reorder_output_as_tensor(burst_guard.get(), batch, info);
    burst_guard.release();  // Tensor's DLPack deleter now owns the burst.

    holoscan::TensorMap out_message;
    out_message["rx_tensor"] = tensor;
    op_output.emit(out_message, "out");
  }

 private:
  // Defined in the Tensor Emission section below.
  std::shared_ptr<holoscan::Tensor> wrap_reorder_output_as_tensor(
      daqiri::BurstParams* burst, void* batch,
      const daqiri::ReorderBurstInfo& info);

  // Parameters populated from the YAML `daqiri_rx` block.
  holoscan::Parameter<std::string> interface_name_;
  holoscan::Parameter<std::string> reorder_name_;

  // Runtime state owned by the operator.
  int port_id_ = -1;               // resolved DAQIRI interface id
  cudaStream_t stream_ = nullptr;  // stream the reorder/quantize kernel runs on
};
```

Because the DAQIRI reorder plan reorders and quantizes the payloads on the GPU, the
operator never touches individual packets: it dequeues a burst, checks the
`DAQIRI_BURST_FLAG_REORDERED` flag, waits on the burst's CUDA event, and reads one
contiguous output buffer. `daqiri::get_reorder_burst_info(...)` returns the batch
layout — `packets_per_batch` sequence slots, `payload_len` output bytes per slot,
`aggregate_len` total — so the operator can shape the tensor correctly.

To process the stream differently — a different reorder layout, a custom
quantization, or any other per-batch GPU transform — you can add your own CUDA kernel
instead of (or after) the built-in plan: run it on the operator's stream over the raw
RX payloads and emit its output buffer the same way. DAQIRI's reorder kernels live in
`src/kernels.cu` (build with `-DDAQIRI_REORDER_GPU_PROFILE=ON` for CUDA event timing)
and are a useful starting point.

### Buffer ownership

The tensor emitted above is a zero-copy view of DAQIRI's `reorder_gpu` buffer, so the
burst must stay alive until every downstream consumer is finished. That is why the
burst is freed in the tensor's release callback rather than in `compute()`:
`daqiri::free_all_packets_and_burst_rx(burst)` returns both the burst metadata and its
packet buffers to the pool. Freeing only the burst metadata, or freeing in `compute()`
while a downstream operator still reads the tensor, leads to drops or use-after-free.
See [RX Step 3 - Free buffers](../api-reference/cpp.md#rx-step-3-free-buffers) for the
full cleanup API.

## Tensor Emission

The source operator emits a `holoscan::TensorMap` on port `out` with a single entry
named `rx_tensor`. `holoscan::Tensor` interoperates through
[DLPack](https://dmlc.github.io/dlpack/latest/), so `wrap_reorder_output_as_tensor`
describes the reordered device buffer with a `DLManagedTensor` and constructs a tensor
from it (`DLPack` types come from `dlpack/dlpack.h`, bundled with the Holoscan SDK):

```cpp
std::shared_ptr<holoscan::Tensor> DaqiriRxOp::wrap_reorder_output_as_tensor(
    daqiri::BurstParams* burst, void* batch,
    const daqiri::ReorderBurstInfo& info) {
  // Reordered, sequence-ordered, fp32 layout: one row per sequence slot, one column
  // per payload element. shape must outlive the tensor, so the DLManagedTensor owns
  // it and the deleter frees it.
  auto shape = std::make_unique<int64_t[]>(2);
  shape[0] = info.packets_per_batch;            // number of sequence slots in the batch
  shape[1] = info.payload_len / sizeof(float);  // fp32 elements per slot

  auto dl = std::make_unique<DLManagedTensor>();
  dl->dl_tensor.data = batch;                        // DAQIRI-owned device buffer (reorder_gpu)
  dl->dl_tensor.device = DLDevice{kDLCUDA, 0};       // device memory, GPU 0
  // The DLPack device ordinal must match the DAQIRI memory-region affinity.
  dl->dl_tensor.ndim = 2;
  dl->dl_tensor.dtype = DLDataType{kDLFloat, 32, 1}; // matches output_type: fp32
  dl->dl_tensor.shape = shape.get();
  dl->dl_tensor.strides = nullptr;                   // row-major / contiguous
  dl->dl_tensor.byte_offset = 0;

  // The deleter is the ownership hook: it runs when the last downstream consumer
  // drops the tensor. Because the tensor is a zero-copy view of DAQIRI's buffer, this
  // is where the burst (and its packet buffers) is returned to DAQIRI.
  dl->manager_ctx = burst;
  dl->deleter = [](DLManagedTensor* self) {
    daqiri::free_all_packets_and_burst_rx(
        static_cast<daqiri::BurstParams*>(self->manager_ctx));
    delete[] self->dl_tensor.shape;
    delete self;
  };

  auto tensor = std::make_shared<holoscan::Tensor>(dl.get());
  // Tensor construction succeeded; its DLPack deleter now owns dl and shape.
  shape.release();
  dl.release();
  return tensor;
}
```

Downstream operators receive an fp32 `[packets_per_batch, elements_per_packet]` tensor
on the GPU, ready for inference or further processing with no host copy. The exact
DLPack and tensor-construction details track the Holoscan SDK version; a complete,
build-tested implementation lives in the Holohub example (see [References](#references)).

## Sink Operator Skeleton

A downstream sink receives the same `TensorMap` and can log, forward, or pass the
tensor into additional processing operators.

```cpp
#include <holoscan/holoscan.hpp>

class TensorSinkOp : public holoscan::Operator {
 public:
  // Boilerplate that lets Holoscan construct the operator from the YAML config.
  HOLOSCAN_OPERATOR_FORWARD_ARGS(TensorSinkOp)

  // Declare a single input port named "in"; it must match the downstream end of the
  // add_flow(rx, sink, {{"out", "in"}}) edge created in compose().
  void setup(holoscan::OperatorSpec& spec) override {
    spec.input<holoscan::TensorMap>("in");
  }

  void compute(holoscan::InputContext& op_input,
               holoscan::OutputContext&,
               holoscan::ExecutionContext&) override {
    // Try to receive a message from the upstream operator. Empty means no data is
    // available on this tick.
    auto maybe_message = op_input.receive<holoscan::TensorMap>("in");
    if (!maybe_message) {
      return;
    }

    // Look up the named tensor the RX operator emitted ("rx_tensor"). A real sink
    // would run inference or further processing here instead of just logging.
    const auto& tensor_map = maybe_message.value();
    auto it = tensor_map.find("rx_tensor");
    if (it == tensor_map.end()) {
      return;  // The expected tensor was not present in the map.
    }

    const auto& tensor = it->second;
    HOLOSCAN_LOG_INFO("Received rx_tensor at {}", static_cast<const void*>(tensor.get()));
  }
};
```

## References

- The standalone `daqiri_bench_raw_reorder_quantize` benchmark
  (`examples/raw_reorder_quantize_bench.cpp`, config
  `daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml`) exercises the same
  reorder/quantize path outside Holoscan — a good way to validate the DAQIRI config
  first. See [Raw Ethernet Benchmarking](../benchmarks/raw_benchmarking.md).
- The landed Holohub reference app is
  [`applications/daqiri_raw_ethernet_bench`](https://github.com/nvidia-holoscan/holohub/tree/main/applications/daqiri_raw_ethernet_bench).
  The merge history is in [Holohub PR #1553](https://github.com/nvidia-holoscan/holohub/pull/1553).
- DAQIRI [configuration walkthrough](configuration-walkthrough.md) and [C++ API Usage](../api-reference/cpp.md) cover the DAQIRI-side configuration, packet access, and cleanup calls.
