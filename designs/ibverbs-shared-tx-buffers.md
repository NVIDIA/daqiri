# Shared TX buffers for raw ibverbs

Status: implemented on top of PR #356; tracking issue #358. Hardware-free tests
and seven real ConnectX-7 loopback lifetime cases pass, alongside the 27 existing
multi-region cases. The [C++ API reference](../docs/api-reference/cpp.md#shared-tx-segments)
and [configuration reference](../docs/api-reference/configuration.md) describe usage.

## Recommendation

Separate **buffer storage** from **a packet's send descriptor**. Give each
storage allocation a reference count, and let multiple send descriptors refer
to it. Each descriptor owns its private header segments, lengths, scheduling
information, and one reference to each storage allocation it uses. Submission
transfers these references to the engine; TX completion releases them. Return storage to its
pool only when the last reference disappears.

For the common two-destination case:

```text
descriptor A: private header A ──┐
                               ├── payload P, references = 2
descriptor B: private header B ──┘

A completes: release header A; P references = 1
B completes: release header B; P references = 0; recycle P
```

The payload is stored once and read by the NIC for each transmission. This
saves payload allocation and copying, but each packet still consumes send
queue capacity and network bandwidth.

This follows the storage ownership principle used by
[DPDK indirect buffers](https://doc.dpdk.org/guides/prog_guide/mbuf_lib.html#direct-and-indirect-buffers),
without introducing a DPDK dependency into ibverbs. The release boundary is
local TX completion, which establishes that the NIC no longer needs the
gathered bytes; it does not acknowledge delivery to the remote application.
The [rdma-core send contract](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_post_send.3)
likewise requires completion before reusing non-inline send buffers.

## Why the cyclic counters are insufficient

The default cyclic allocator couples allocations and transmissions:

- `IbvTxQueue::alloc_head` and `completed_tail` describe cyclic packet slots.
- `get_tx_packet_burst()` allocates the same slot index from every segment MR.
- Posting increments `slots_posted` once per packet. CQ processing advances
  `completed_tail` using `wqe_slot_cum`.
- `send_tx_burst()` and the unsent free path cancel allocations by subtracting
  from `alloc_head`, assuming the allocation is the most recent one.
- Burst metadata returns to its pool immediately after WQE construction,
  before the NIC finishes reading packet bytes.

These mechanisms are in
[`daqiri_ibverbs_engine.h`](../src/engines/ibverbs/daqiri_ibverbs_engine.h) and
[`daqiri_ibverbs_engine.cpp`](../src/engines/ibverbs/daqiri_ibverbs_engine.cpp).
Sending two WQEs from one slot would advance transmission progress twice for
one allocation. Copying packet pointers or retaining `BurstParams` metadata
does not fix storage reclamation.

The named-endpoint path already provides the basic wire layout: it copies
each endpoint's 42-byte Ethernet/IPv4/UDP header into its SEND WQE and gathers
the payload through a data segment. Extend it to gather multiple segments for
endpoint fan-out. The ordinary raw SEND path gathers explicit packet headers
and payloads from their segment buffers.

PR #356 supplies one-through-four-segment ordinary and endpoint TX, including
multi-block WQE sizing, SQ wraparound, and aggregate endpoint lengths. The shared
storage layer builds on those builders and completion mappings.

## Initial scope and configuration

Support raw ibverbs TX allocations, on a single **indirect** TX queue, with
any number of clones subject to bounded resource capacity. Support one through
`MAX_NUM_SEGS` data segments per packet; the current public limit is four.
Sharing is selected independently for each segment, not hard-coded to HDS or
the last segment. Cover these layouts:

| Packet layout | Private state per send | Shared storage |
| --- | --- | --- |
| Named endpoint, one segment | Endpoint header copied into each WQE | Segment 0 payload |
| Header/data split, two segments | CPU header in segment 0 | CPU or GPU payload in segment 1 |
| Three segments | Network header in segment 0, message header in segment 1 | Final payload in segment 2 |
| Four segments | Network header, message header, and metadata in segments 0–2 | Final payload in segment 3 |
| Named endpoint, multiple segments | Inline network header plus private application header segments | Selected payload segment(s) |

For the requested three-segment case, two sends have this layout:

```text
send A: [Ethernet/IP/UDP A] [message header A] [payload P]
send B: [Ethernet/IP/UDP B] [message header B] [payload P]
          private segment 0  private segment 1  shared segment 2
```

Each row is one wire packet. The NIC gathers segments in their configured
order, producing the same bytes as their concatenation, without a contiguous
packet copy. Add another private header segment before P for the four-segment
case. Every private segment has its own allocation per send; only P shares an
allocation and reference count. Header bytes and header lengths may differ
between sends, within each segment's capacity and the total frame limit.

Opt in per TX queue with `buffer_mode: shared`; omission retains
`buffer_mode: cyclic` and the existing allocation/completion fast path. Reject
shared mode for other engines and direct polling during semantic validation.
Select the mode at queue creation; do not switch an active queue between
allocators.

Add `max_inflight_sends` for shared queues. It bounds accepted packets across
the handoff ring, worker-pending burst, and SQ, independently of payload
buffer counts. Default to twice the old minimum per-region `num_bufs`, with
overflow and hardware-capacity validation. This accommodates two sends per
payload without requiring twice as many payload slots. Larger fan-out can
use more credits or encounter normal backpressure. Reject bursts larger than
this fixed limit before consuming ownership.

Initially exclude RX-to-TX references, cross-queue/cross-NIC sharing, arbitrary
external pointers, byte-range slicing, and private GPU-segment copying. These
need additional lifetime, registration, or asynchronous-copy contracts.
Existing externally bound TX MRs can participate: DAQIRI owns slot reuse, and
the external allocation must remain valid for the MR's registered lifetime.

## Public API

Add one flat helper through `daqiri/daqiri.h` (declared in `common.h`):

```cpp
Status clone_tx_burst(const BurstParams* source,
                      uint32_t shared_segment_mask,
                      BurstParams** clone);
```

The helper duplicates a whole batch, preserving its packet count and queue.
For each packet, it retains the segments selected by the mask and allocates
and copies the remaining segments. Bit `s` selects segment `s`:

- Named endpoints: mask `1u << 0`; allocate metadata only, retain payloads.
- HDS: mask `1u << 1`; allocate/copy CPU headers, retain payloads. No second
  payload allocation occurs.
- Three segments: mask `1u << 2`; allocate/copy segments 0 and 1, retain
  segment 2. Edit both private header segments independently for each send.
- Four segments: mask `1u << 3`; allocate/copy segments 0–2, retain segment 3.
  Masks may also select several shared segments when needed.

Cloning a clone retains the same underlying storage controls; it never forms
a chain of parent bursts. The source metadata can disappear independently.
Non-selected segments must be CPU-accessible in the initial implementation;
return `NOT_SUPPORTED` rather than introducing an implicit GPU copy or sync.

The source must be an allocated, application-owned, complete TX burst from a
shared-mode queue. Clone before submitting or freeing it, on the queue's
existing single producer thread. Reject zero/out-of-range masks, incomplete
or released segments, RX bursts, and invalid pointers/lengths. Each clone has
independent pointer/length arrays and private headers. Copy packet lengths,
but initialize fresh engine state and clear scheduling timestamps; callers
set each clone's transmission time explicitly. Do not shallow-copy CUDA
events, `custom_pkt_data`, or engine-private ownership fields.

The operation is transactional: on failure, `*clone` is null, source ownership
is unchanged, and all tentative allocations and retains are unwound. Use
existing metadata/buffer exhaustion statuses. Reject reference-count overflow
without wrapping the count. Existing send and free helpers remain the way to
submit or cancel each resulting burst.

Example flow, with allocation/fill/error handling abbreviated:

```cpp
// original contains payload-only packets acquired from a shared TX queue.
// Finish producing the payload, including any required GPU synchronization.
BurstParams* second = nullptr;
Status status = clone_tx_burst(original, 1u << 0, &second);
if (status != Status::SUCCESS) {
  free_all_packets_and_burst_tx(original);
  return status;
}

// Each send patches its own inline endpoint header and gathers the same bytes.
Status first_status = send_tx_burst(endpoint_a, queue_id, original);
Status second_status = send_tx_burst(endpoint_b, queue_id, second);

// For EACH result: SUCCESS and NO_SPACE_AVAILABLE consume that burst.
// Other failures leave it owned by the caller, who must retry or free it.
```

For HDS, clone with `1u << 1`, edit each private segment-0 header using the
existing helpers, then submit each burst normally. A one-segment packet that
already contains its header cannot share its entire segment while changing
that header; use HDS or named endpoints for that case.

For a three-segment burst, the corresponding clone call is:

```cpp
// original: [network header][message header][payload].
// Populate all segments and lengths before cloning.
Status status = clone_tx_burst(original, 1u << 2, &second);
// On success, second has private copies of segments 0 and 1, and shares 2.
// Edit segment 0 with the network-header helpers and segment 1 through
// get_segment_packet_ptr(second, 1, packet_index), then send each burst.
```

Each burst carries its own length for every segment. Submit each with
`send_tx_burst(burst)` when its network header is in segment 0, or with the
endpoint overload when the endpoint supplies an additional inline network
header. Neither path may silently ignore intermediate header segments.

Shared bytes become immutable when cloning succeeds. Prepare the payload
before cloning and do not modify it while any descriptor or send owns a
reference. Header helpers may modify private segments and reject shared segment-0
writes. Raw pointer access and Python packet-copy helpers require caller
discipline. Reference counting provides lifetime management, not CPU/GPU/NIC
data visibility or copy-on-write.

## Internal storage and allocation

In shared mode, create independent fixed-slot pools for each configured TX
segment. Allocate CPU headers and payloads independently; they no longer
require matching slot indices or equal pool sizes. Preallocate NUMA-local
host control arrays and bounded free-index rings at queue creation. Keep the
existing registered host/GPU packet storage and strict HUGE allocation rules.

Each slot has a host control record with an atomic reference count, its pool
and slot identity, and immutable address/capacity/registration information.
The control record, rather than a pointer to the original burst, is the owner.
Extend the private pooled burst layout with per-packet/per-segment storage
handles and a shared-mode flag; do not add ownership fields to `BurstParams`.
Do not use `custom_pkt_data` or one heap `shared_ptr` per packet.

An allocated segment starts at one reference. Cloning retains it. Submission
**transfers** its reference; it does not add another one. Completion or unsent
cancellation releases exactly that reference. Use relaxed atomic increments
while a valid source reference is held, and release decrements with an
acquire fence on the final release. Publish returned indices with release /
acquire synchronization. The allocator must handle final releases from both
the application and worker threads; an SPSC free ring is insufficient.

Pool lookup must not permit independently allocating overlapping slots from
one MR. For this first version, reject an MR assigned to a shared queue if it
is also assigned to another active RX/TX queue or repeated segment. A future
MR-wide allocator can remove that restriction.

## Send admission, WQEs, and completion

Keep three distinct accounting systems:

1. **Storage:** per-slot reference counts and per-segment free pools.
2. **Send descriptors:** bounded accepted packets, including queued packets.
3. **SQ:** WQEBB producer/completion counters, including WAIT and NOP WQEs.

At submission, validate all packets before transferring ownership. Reserve
send credits for the whole burst before publishing it to the handoff ring.
If credits or handoff space are exhausted, release the burst's references
and metadata and return `NO_SPACE_AVAILABLE`, preserving the existing
ownership-consuming contract. Never use `alloc_head` as a storage allocation index in shared mode. It counts
accepted sends; only a tentative admission rejected by the handoff ring rolls it back.
Validation failures leave every reference with the caller.

The worker waits for enough SQ capacity for the whole burst, as today.
Before ringing the doorbell, transfer each packet's storage handles into a
preallocated, ordered completion ledger. One ledger entry represents one
wire packet and all of its segment references. Once handles are transferred,
the metadata can immediately return to `tx_meta_pool_`.

Track monotonic `sends_posted` and `sends_completed` separately from storage
allocations. Map each WQE's first WQEBB to both cumulative send progress and
cumulative WQEBB progress. On a successful signaled SEND CQE, retire **every**
ledger entry covered since the previous completion, releasing its segment
references and send credits. Updating just the CQE's own entry would leak
references for unsignaled sends. WAIT and drain NOP WQEs carry no storage
references; retain their cumulative mappings without inventing packet slots.
Do not overwrite ledger or WQE mappings until their previous use is retired.

Retain the existing indirect policy of signaling the last SEND in each burst,
so a lone burst makes progress without later traffic. Size and validate SQ,
CQ, and ledger capacities using `max_inflight_sends` and the worst supported
WQE footprint for the queue's segment count, header mode, and optional WAIT.

Disable eMPW for shared-mode queues. It can be restored by mapping
each packed WQE to all of its packet ledger entries. Keep pacing, checksum
offload, scheduling, and endpoint-header construction on their existing paths.
Use the storage handle's validated registration/address when constructing
data segments; a user-edited `pkts` pointer is not proof of ownership.

### Multi-segment WQE construction

For ordinary raw SEND, emit one data segment per configured packet segment,
in order, using that segment's address, length, and lkey. The existing layout
has a 16-byte control segment and a 16-byte Ethernet segment, so its size is:

```text
DS = 2 + num_segments
WQE bytes = 16 * DS
WQEBBs = ceil(WQE bytes / 64)
```

| Data segments | WQE bytes | WQEBBs per SEND |
| --- | --- | --- |
| 1 | 48 | 1 |
| 2 | 64 | 1 |
| 3 | 80 | 2 |
| 4 | 96 | 2 |

Use a common size helper in admission, posting, and queue sizing. Stage the
whole WQE, zero its padding, and copy its WQEBBs with
`copy_wqe_to_cyclic_sq()` so crossing the end of the SQ wraps each block
correctly. Advance `sq_pi` by the actual block count, but advance
`sends_posted` once. Store the cumulative completion mapping at the first
block, covering the entire WQE. A scheduled packet adds a separate WAIT
block. This block-based accounting matches the
[mlx5 provider's SEND construction](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/providers/mlx5/qp.c),
which rounds the encoded data-segment size up to WQEBBs.

For named endpoints, remove the current one-segment checks in the proposed
shared-mode path and gather all configured segments after the inline network
header. Keep the current 80-byte prefix before the first data segment:

```text
DS = 5 + num_segments
WQE bytes = 80 + 16 * num_segments
WQEBBs = ceil(WQE bytes / 64)
```

This uses two WQEBBs for one through three data segments and three for four;
an optional WAIT adds one. Size the staging buffer and SQ reservation for
this maximum rather than the existing fixed two-block endpoint WQE. The
inline network header is separate from the application's data segments and
has no pooled storage reference. Endpoint UDP payload length is the sum of
**all** gathered segment lengths, including private application headers.

Request `max_send_sge` for the full segment count when creating the QP, check
the granted capabilities and mapped SQ geometry, and reject unsupported
configurations before starting the queue. Provision the descriptor size for
the **combined inline-header plus gather-list footprint**: requesting a
42-byte inline limit and four SGEs independently does not by itself establish
that their combination fits. The
[mlx5 QP sizing code](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/providers/mlx5/verbs.c)
uses the larger of its inline and gather size calculations. One conservative
strategy is to request inline capacity at least as large as the unpadded
largest mixed WQE, then verify the resulting descriptor budget and total SQ
capacity; optimize that reservation only with an explicit sizing calculation.
Keep admission accounting based on actual WQEBBs emitted, even if provider
reservation is larger. Reject counts outside 1–4; never silently truncate the
segment list. PR #356 already enables all four segments for cyclic queues as well.

### Packet lengths and header handling

For ordinary raw SEND, retain each segment length and compute total frame
length with a checked sum across all segments. For named endpoints, add the
inline network-header size to that sum. Validate each segment against its
own allocation capacity and validate the total against MTU and IPv4/UDP
length limits. Use the aggregate length for queue MTU provisioning and TX
byte accounting, rather than only `pkt_lens[0]`. The current
`get_packet_length()` returns segment 0's length in this engine; do not use
it as the aggregate length or change its contract incidentally.

Keep the Ethernet/IPv4/UDP header together in private segment 0 for ordinary
raw sends using the existing header helpers. Additional private header
segments are UDP payload bytes, written with segment accessors; splitting
the outer network header across allocations is outside this initial scope.
Network length fields must include every following segment. For example,
with a 42-byte segment 0, a message header of H bytes, and payload P bytes:

```text
UDP length = 8 + H + P
IPv4 total length = 20 + UDP length
L2 frame length = 42 + H + P
```

Recompute these values independently for each send when its private header
length changes. Keep existing outer IPv4/UDP checksum offload and verify it
over the full gathered packet. Arbitrary inner headers remain application
data; sharing does not add automatic inner-header parsing or checksums.

Completion remains per wire packet, not per segment. Its ledger entry owns
all segment handles. For two three-segment sends, A's completion releases
A's two private headers and decrements P from two references to one; B's
completion releases B's headers and decrements P to zero. Cancellation uses
the same per-segment release operation. Releasing the original burst's
metadata must not invalidate any of the clone's handles.

## Cancellation, errors, and teardown

| Event | Reference action |
| --- | --- |
| Clone succeeds | Retain selected source segments; allocate private segments |
| Clone fails | Undo all tentative work; source unchanged |
| Successful enqueue | Transfer burst references to the engine |
| Admission/handoff full | Release this burst only; return consuming `NO_SPACE_AVAILABLE` |
| Validation failure | No transfer; caller retains the burst |
| Cancel any unsent burst | Release its references, regardless of allocation order |
| Successful completion | Release every newly covered ledger entry once |
| Original completes before a clone is submitted | Release original's references; clone still pins payload |

Route the shared TX branches of `free_packet`, segment frees,
`free_all_packets`, and cancellation through the same release primitive.
Clear each released handle so aggregate cleanup cannot release it twice.
For shared queues, metadata-only free also releases any remaining unsent
handles. Aggregate cleanup clears handles first, so the two operations compose.
Never resubmit or access a burst after an ownership-consuming result.

A CQ error must stop new submissions and enter queue failure handling. Do
not treat the error CQE's cumulative index as successful completion of all
outstanding sends. Release unposted accepted bursts immediately; keep
possibly DMA-visible references pinned until completion/flush or successful
QP destruction establishes that hardware access has ended. If quiescence
cannot be established, retain the storage and registrations. Reconcile the
ledger exactly once after quiescence, including sends with no successful CQE.

Runtime queue deletion enters DRAINING, rejects new allocations/clones/sends,
and permits releases of application-owned bursts. Complete deletion only
after active API users leave, queued/pending work drains, the ledger is empty,
and **all storage references are zero**. An unsent clone must keep deletion
pending. Keep MR registrations until then; do not infer safety from
`completed_tail == alloc_head`. Shutdown uses the same DMA-quiescence ordering
and reports remaining application-owned references before final cleanup.

## Implementation and validation

Implement in this order:

1. Extract hardware-free storage-pool/reference/ledger primitives with bounded
   allocation and transactional rollback.
2. Add configuration validation, shared queue initialization, private burst
   handles, and `clone_tx_burst` dispatch. Validate segment counts through
   `MAX_NUM_SEGS` and device capabilities. Other engines return `NOT_SUPPORTED`.
3. Integrate shared-mode acquisition, cancellation, admission, regular and
   endpoint WQE posting for all segment counts, completion, failure, and
   resource deletion. Use the same WQE size calculations throughout.
4. Provide a fan-out call pattern and the affected C++ API, configuration, ownership,
   Python binding, and raw benchmarking documentation. Keep current docs
   describing implemented behavior until that change lands.

Keep default cyclic queues on their current path. Changing public
`TxQueueConfig` layout and adding an `Engine` virtual method changes the binary interface. The implementation bumps `DAQIRI_ABI_VERSION`
from 2 to 3 while retaining the independent CalVer package version. Keeping `BurstParams`
unchanged alone does not establish ABI compatibility.

Hardware-free C++ tests should prove:

- Two or N views reuse a payload only after all completions/cancellations.
- A clone remains valid after original metadata is recycled, and a clone of
  a clone holds the underlying storage directly.
- Private headers recycle independently; retaining one old payload does not
  block reuse of unrelated slots. No unused payload slot is allocated for a clone.
- Three- and four-segment clones allocate every private header independently
  and retain only the selected shared segments. Modifying either send's
  headers leaves the other send's headers and the payload unchanged. Include
  masks selecting multiple shared segments and per-send header length changes.
- Encoded SEND data segments have the expected order, lengths, addresses,
  and lkeys. Ordinary SEND uses 1/1/2/2 WQEBBs for segment counts 1/2/3/4;
  endpoint SEND uses 2/2/2/3. Force every multi-block case across SQ wraparound,
  with and without WAIT, and verify the next WQE is not overwritten.
- Aggregate frame, IPv4, and UDP lengths include all header/payload segments;
  excessive aggregate lengths fail even when every individual segment fits.
  Reject more than four segments and insufficient granted QP capabilities.
- Arbitrary cancellation order, clone rollback at each allocation failure,
  refcount overflow, pool exhaustion, and consuming versus non-consuming errors.
- A later signaled CQE retires multiple unsignaled sends; WAIT/NOP mappings,
  SQ/ledger wraparound, idle-tail progress, and error cleanup release once.
- Deletion waits for unsent clones; concurrent worker completion and app
  cancellation cannot recycle storage while another owner still exists.

On mlx5 hardware, send identical CPU and GPU payloads through two distinct
endpoint headers, and repeat with two, three, and four gathered segments.
For three/four segments, give each send distinct IP/UDP destinations and
distinct message/metadata header bytes, but the same final payload storage.
Verify complete received byte sequences, all header fields, lengths, and
checksums for both ordinary and endpoint sends. Use two slots in each private
header pool and one in the shared payload pool to prove that both packets
can be outstanding without a second payload allocation.

Force repeated reuse and SQ wraparound, cancel one view, delay one send,
exhaust handoff/send credits, and delete the queue while a clone remains
held. Check counters for leaks, drops, and premature reuse. Test GPU producer
synchronization explicitly.

Measure shared-mode CPU cost and throughput against payload copying at
fan-out 1, 2, and larger values, and verify default cyclic performance remains
stable. Follow the repository's `mlnx_perf` methodology; label hardware
loopback separately from cabled throughput.

## Follow-on work

Cross-queue sharing needs allocator ownership independent of either queue,
an MR registration/lkey valid in each destination protection domain, and
deletion dependencies across queues. Cross-NIC sharing may require separate
registrations for the same bytes. Retaining a pointer or reusing an lkey from
the source queue is insufficient.

RX forwarding needs owners that retain the actual RX stride, regular-RQ
packet, or reorder output until the final TX reference disappears, then use
that owner's existing release mechanism. Such an adapter must retain source
queue lifetime and preserve GPU readiness. Do not route RX buffers through a
TX-slot free list.

Direct TX support needs a new admission contract: clones allow multiple
application-owned views, whereas current direct mode allows exactly one
pending allocation. Reserve SQ capacity at submission, extend completion
progress/drain behavior, and revise the owner-thread rules together rather
than bypassing `direct_pending` checks.
