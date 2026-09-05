# DAQIRI → CUDA → UCX GPU egress

This directory contains an opt-in example that receives sequenced raw packets
with DAQIRI, reorders them into 16-image CUDA batches, applies a CUDA transform,
and sends one UCP rendezvous message per batch.

For container setup, configuration, launch order, pass criteria, and performance
reporting requirements, follow the [DAQIRI + UCX GPU Egress tutorial](../../docs/tutorials/daqiri-ucx-gpu-egress.md).
That tutorial is the canonical operator runbook.

## Source map

| Path | Responsibility |
|---|---|
| `ucx_transport.{h,cpp}` | Move-only producer and receiver tokens, UCP endpoint lifecycle, batch credits, DATA transfer, EOS, and failure cleanup. |
| `protocol.{h,cpp}` | Serialized Active Message control and DATA headers. |
| `image_geometry.h` | Fixed 128 KiB image and 16-image/2 MiB batch geometry. |
| `external_batch_policy.{h,cpp}` | Fixed-run sequence accounting and drop-before-submit policy. |
| `pipeline_spsc_queue.h` | Fixed-capacity producer handoff and free-slot queues. |
| `raw_pipeline_bench.cu` | DAQIRI RX/reorder, CUDA processing, and optional UCX egress composition. |
| `raw_tx_bench.cpp` | Bounded raw-packet source for the tutorial. |
| `processing/` | In-place scale/offset CUDA kernel and its golden-value test. |
| `cuda_image.{h,cu}` | Standalone transport-benchmark payload generation and receiver validation. |
| `configs/` | Host A source and Host B pipeline templates. |
| `*_test.cpp`, `processing/test_*.cu` | Protocol, policy, queue, and CUDA component tests. |

The raw source places a network-order 32-bit sequence at frame byte 42 and an
8,192-byte fragment at byte 80. `raw_processor.yaml` sends groups of 256 packets
through DAQIRI's `seq_packets_per_batch` reorder path. There is no
application-local raw header or assembler.

## Ownership contract

- A producer `BatchLease` represents one generation of a registered batch slot.
  Exactly one terminal operation consumes it: `submit_after(stream)` or
  `discard()`.
- DAQIRI reorder output and UCX slots are separate pools. The application holds
  the DAQIRI burst until an event proves that the single 2 MiB asynchronous copy
  into the lease has completed.
- `submit_after(stream)` records a CUDA event. UCP cannot read the batch before
  the event completes, and the producer cannot reuse it before UCP send
  completion.
- One DATA Active Message carries up to sixteen contiguous 128 KiB images. Credit
  is counted in receiver batch slots, so a batch is never partially admitted.
- A receiver `ReceivedBatch` becomes visible after UCP receive completion.
  `release()` returns it immediately; `release_after(stream)` waits for
  downstream CUDA work before returning the slot and credit.
- The receiver has no progress thread. The thread calling its public operations
  owns UCP progress and CUDA release-event polling.
- Sequence and generation checks reject stale handoffs. Failure cleanup
  quarantines storage if UCX or CUDA may still hold a reference.
- `EOS_ACK` is sent only after every admitted batch has reached receive
  completion and the application has returned its receiver slot. With
  `release_after(stream)`, that includes completion of the supplied CUDA work;
  it does not acknowledge any work outside that ownership boundary.

`host_pinned_mapped` is the validated DGX Spark allocation. `cuda_device` is kept
for later discrete-GPU validation and must not be described as GPUDirect RDMA
without UCX protocol and NIC/PCIe evidence.

## Build switches

Enable the application with:

```text
-DDAQIRI_BUILD_APPLICATIONS=ON
-DDAQIRI_BUILD_UCX_GPU_EGRESS=ON
```

Its component tests require `-DBUILD_TESTING=ON`. The normal application build
leaves tests off; the ibverbs reorder tracker test is owned by the engine rather
than this application.
