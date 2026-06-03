# DAQIRI + Holoscan Integration

DAQIRI can be used as the IO layer inside an NVIDIA Holoscan application. The
recommended pattern is to initialize DAQIRI once during application startup, let a
Holoscan source operator poll DAQIRI RX queues, aggregate packets into
application-owned tensor storage, and emit a `holoscan::TensorMap` to downstream
operators.

This tutorial shows a minimal C++ integration skeleton. It is not a complete
runnable app in this repository. The exact tensor and DLPack wrapping code depends
on the Holoscan SDK version and should live in a runnable Holohub example.

Before integrating with Holoscan, make sure the DAQIRI YAML works with the
standalone benchmarks in [Benchmarking Examples](benchmarking_examples.md), and
review the [configuration walkthrough](configuration-walkthrough.md).

## Application Lifecycle

The Holoscan application owns the graph. DAQIRI owns NIC setup, packet memory,
and RX burst lifetimes.

```cpp
#include <daqiri/daqiri.h>
#include <holoscan/holoscan.hpp>

#include <filesystem>

class DaqiriHoloscanApp : public holoscan::Application {
 public:
  void compose() override {
    auto rx = make_operator<DaqiriRxOp>("daqiri_rx", from_config("bench_rx"));
    auto sink = make_operator<TensorSinkOp>("tensor_sink");

    add_flow(rx, sink, {{"out", "in"}});
  }
};

int main(int argc, char** argv) {
  if (argc != 2) {
    return 1;
  }

  const std::filesystem::path config_path{argv[1]};

  auto status = daqiri::daqiri_init(config_path.string());
  if (status != daqiri::Status::SUCCESS) {
    return 1;
  }

  auto app = holoscan::make_application<DaqiriHoloscanApp>();
  app->config(config_path.string());
  app->run();

  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
```

The shared YAML file can contain both Holoscan parameters and the `daqiri` block
passed to `daqiri_init(...)`. DAQIRI ignores Holoscan-only keys, and Holoscan
operators read their own parameter blocks with `from_config(...)`.

```yaml
scheduler:
  type: greedy

daqiri:
  cfg:
    version: 1
    stream_type: "raw"
    master_core: 3
    log_level: "info"
    memory_regions:
      - name: rx_payload
        kind: device
        affinity: 0
        num_bufs: 32768
        buf_size: 4096
    interfaces:
      - name: rx_port
        address: "<0000:00:00.0>"
        rx:
          queues:
            - name: rx_q0
              id: 0
              cpu_core: 9
              batch_size: 64
              memory_regions: [rx_payload]

bench_rx:
  interface_name: rx_port
  batch_size: 64
  max_packet_size: 4096
  header_size: 64
  gpu_direct: true
  split_boundary: 0
```

Replace placeholder PCIe addresses, CPU cores, queue fields, and memory regions
with values for your system. RX flow rules are omitted from this compact excerpt;
real raw Ethernet RX configs need a `flows:` block to steer packets to a queue.
See the [configuration walkthrough](configuration-walkthrough.md) and
[Configuration YAML Reference](../api-reference/configuration.md).

## RX Operator Skeleton

The source operator resolves the configured DAQIRI interface once, allocates any
CUDA resources it needs, polls all RX queues on each `compute()` call, and emits a
Holoscan tensor batch after aggregation.

```cpp
#include <daqiri/daqiri.h>
#include <holoscan/holoscan.hpp>

#include <cuda_runtime.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

class DaqiriRxOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(DaqiriRxOp)

  ~DaqiriRxOp() override {
    if (batch_ready_event_ != nullptr) {
      cudaEventDestroy(batch_ready_event_);
    }
    if (stream_ != nullptr) {
      cudaStreamDestroy(stream_);
    }
  }

  void setup(holoscan::OperatorSpec& spec) override {
    spec.param(interface_name_, "interface_name", "Interface name",
               "Name of the DAQIRI interface to receive from");
    spec.param(batch_size_, "batch_size", "Batch size",
               "Number of packets to aggregate before emitting");
    spec.param(max_packet_size_, "max_packet_size", "Max packet size",
               "Maximum packet bytes copied into one output slot");
    spec.param(header_size_, "header_size", "Header size",
               "Header bytes used when HDS is enabled");
    spec.param(gpu_direct_, "gpu_direct", "GPU direct",
               "Whether RX payloads are expected in CUDA-addressable memory");
    spec.param(split_boundary_, "split_boundary", "Split boundary",
               "Payload segment index for header-data split; 0 for single segment");

    spec.output<holoscan::TensorMap>("out");
  }

  void initialize() override {
    holoscan::Operator::initialize();

    port_id_ = daqiri::get_port_id(interface_name_.get());
    if (port_id_ < 0) {
      throw std::runtime_error("DAQIRI interface was not found");
    }

    cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
    cudaEventCreateWithFlags(&batch_ready_event_, cudaEventDisableTiming);

    allocate_batch_buffers(batch_size_.get(), max_packet_size_.get());
  }

  void compute(holoscan::InputContext&,
               holoscan::OutputContext& op_output,
               holoscan::ExecutionContext&) override {
    const auto queue_count = daqiri::get_num_rx_queues(port_id_);

    for (uint16_t queue_id = 0; queue_id < queue_count; ++queue_id) {
      daqiri::BurstParams* burst = nullptr;
      const auto status = daqiri::get_rx_burst(&burst, port_id_, queue_id);
      if (status != daqiri::Status::SUCCESS || burst == nullptr) {
        continue;
      }

      consume_burst_into_batch(burst);
      daqiri::free_all_packets_and_burst_rx(burst);
    }

    if (!batch_is_ready()) {
      return;
    }

    cudaEventRecord(batch_ready_event_, stream_);
    cudaEventSynchronize(batch_ready_event_);

    holoscan::TensorMap out_message;
    out_message["rx_tensor"] = wrap_current_batch_as_tensor();
    op_output.emit(out_message, "out");

    reset_batch();
  }

 private:
  void consume_burst_into_batch(daqiri::BurstParams* burst) {
    const int packet_count = daqiri::get_num_packets(burst);

    for (int i = 0; i < packet_count; ++i) {
      if (split_boundary_.get() == 0) {
        void* packet = daqiri::get_packet_ptr(burst, i);
        uint32_t packet_len = daqiri::get_packet_length(burst, i);
        append_single_segment_packet(packet, packet_len, stream_);
      } else {
        void* header = daqiri::get_segment_packet_ptr(burst, 0, i);
        uint32_t header_len = daqiri::get_segment_packet_length(burst, 0, i);

        const int payload_segment = split_boundary_.get();
        void* payload = daqiri::get_segment_packet_ptr(burst, payload_segment, i);
        uint32_t payload_len =
            daqiri::get_segment_packet_length(burst, payload_segment, i);

        append_hds_packet(header, header_len, payload, payload_len, stream_);
      }
    }
  }

  void allocate_batch_buffers(int batch_size, int max_packet_size);
  bool batch_is_ready() const;
  void reset_batch();

  void append_single_segment_packet(void* packet, uint32_t packet_len,
                                    cudaStream_t stream);
  void append_hds_packet(void* header, uint32_t header_len,
                         void* payload, uint32_t payload_len,
                         cudaStream_t stream);

  std::shared_ptr<holoscan::Tensor> wrap_current_batch_as_tensor();

  holoscan::Parameter<std::string> interface_name_;
  holoscan::Parameter<int> batch_size_;
  holoscan::Parameter<int> max_packet_size_;
  holoscan::Parameter<int> header_size_;
  holoscan::Parameter<bool> gpu_direct_;
  holoscan::Parameter<int> split_boundary_;

  int port_id_ = -1;
  cudaStream_t stream_ = nullptr;
  cudaEvent_t batch_ready_event_ = nullptr;
};
```

For a single-segment GPU RX configuration, `daqiri::get_packet_ptr(...)` returns
the packet pointer and `daqiri::get_packet_length(...)` returns its length. For
header-data split, segment `0` is commonly the CPU header segment and the payload
segment is retrieved with `daqiri::get_segment_packet_ptr(...)` and
`daqiri::get_segment_packet_length(...)`.

The skeleton above copies or aggregates packet contents into operator-owned batch
storage before emitting. That means it can return the RX buffers to DAQIRI
immediately with `daqiri::free_all_packets_and_burst_rx(burst)`.

If your operator wraps DAQIRI-owned packet buffers directly in a tensor, the tensor
lifetime must call the matching DAQIRI cleanup function when downstream consumers
are finished. For normal RX bursts, use
`daqiri::free_all_packets_and_burst_rx(burst)`. For HDS or segmented ownership
where only one segment is transferred to downstream ownership, use
`daqiri::free_segment_packets_and_burst(burst, segment_id)` for the segment being
released. Freeing only the burst metadata is insufficient when RX packet buffers
are still owned by the RX path; missed packet-buffer frees eventually drain the
pool and cause RX drops.

See [C++ API Usage](../api-reference/cpp.md#receiving-packets) for packet access
helpers and [RX Step 3 - Free buffers](../api-reference/cpp.md#rx-step-3-free-buffers)
for the complete cleanup API list.

## Tensor Emission

The source operator emits a `holoscan::TensorMap` on port `out`. The example uses
a single entry named `rx_tensor`:

```cpp
holoscan::TensorMap out_message;
out_message["rx_tensor"] = wrap_current_batch_as_tensor();
op_output.emit(out_message, "out");
```

The tensor can represent any layout your downstream pipeline expects, such as
`[batch, bytes]`, `[batch, header_bytes]` plus `[batch, payload_bytes]`, or a
metadata tensor paired with a payload tensor. The important boundary is ownership:
either the operator emits tensors backed by its own batch storage, or it emits
zero-copy wrappers with a release callback that frees the DAQIRI packet buffers.

The complete SDK-version-specific tensor and DLPack wrapping belongs in the
Holohub runnable example, where it can be compiled and tested against a concrete
Holoscan SDK release.

## Sink Operator Skeleton

A downstream sink receives the same `TensorMap` and can log, forward, or pass the
tensor metadata into additional processing operators.

```cpp
#include <holoscan/holoscan.hpp>

class TensorSinkOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(TensorSinkOp)

  void setup(holoscan::OperatorSpec& spec) override {
    spec.input<holoscan::TensorMap>("in");
  }

  void compute(holoscan::InputContext& op_input,
               holoscan::OutputContext&,
               holoscan::ExecutionContext&) override {
    auto maybe_message = op_input.receive<holoscan::TensorMap>("in");
    if (!maybe_message) {
      return;
    }

    const auto& tensor_map = maybe_message.value();
    auto it = tensor_map.find("rx_tensor");
    if (it == tensor_map.end()) {
      return;
    }

    const auto& tensor = it->second;
    HOLOSCAN_LOG_INFO("Received rx_tensor at {}", static_cast<const void*>(tensor.get()));
  }
};
```

## References

- [Holohub PR #1553](https://github.com/nvidia-holoscan/holohub/pull/1553) is the current reference while the Holohub example is under review.
- After merge, the expected Holohub app path is `applications/daqiri_raw_ethernet_bench`.
- DAQIRI [configuration walkthrough](configuration-walkthrough.md) and [C++ API Usage](../api-reference/cpp.md) cover the DAQIRI-side configuration, packet access, and cleanup calls.
