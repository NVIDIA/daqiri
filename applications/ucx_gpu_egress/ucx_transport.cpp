// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "ucx_transport.h"

#include "cuda_image.h"
#include "external_batch_policy.h"
#include "pipeline_spsc_queue.h"

#include <cuda_runtime.h>
#include <ucp/api/ucp.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace daqiri::ucx_gpu {

ReceivedImage::ReceivedImage(void* device_data, std::size_t size, std::uint64_t sequence,
                             std::uint64_t preceding_gap, std::size_t slot,
                             std::uint64_t generation) noexcept
    : device_data_(device_data),
      size_(size),
      sequence_(sequence),
      preceding_gap_(preceding_gap),
      slot_(slot),
      generation_(generation) {}

ReceivedImage::ReceivedImage(ReceivedImage&& other) noexcept
    : device_data_(other.device_data_),
      size_(other.size_),
      sequence_(other.sequence_),
      preceding_gap_(other.preceding_gap_),
      slot_(other.slot_),
      generation_(other.generation_) {
  other.invalidate();
}

void ReceivedImage::invalidate() noexcept {
  device_data_ = nullptr;
  size_ = 0;
  sequence_ = 0;
  preceding_gap_ = 0;
  slot_ = 0;
  generation_ = 0;
}

BatchLease::BatchLease(void* ucx_data, void* device_data, std::size_t size, std::size_t slot,
                       std::uint64_t generation) noexcept
    : ucx_data_(ucx_data),
      device_data_(device_data),
      size_(size),
      slot_(slot),
      generation_(generation) {}

BatchLease::BatchLease(BatchLease&& other) noexcept
    : ucx_data_(other.ucx_data_),
      device_data_(other.device_data_),
      size_(other.size_),
      slot_(other.slot_),
      generation_(other.generation_) {
  other.invalidate();
}

void BatchLease::invalidate() noexcept {
  ucx_data_ = nullptr;
  device_data_ = nullptr;
  size_ = 0;
  slot_ = 0;
  generation_ = 0;
}

namespace {

using Clock = std::chrono::steady_clock;

const char* thread_mode_name(ucs_thread_mode_t mode) noexcept {
  switch (mode) {
    case UCS_THREAD_MODE_SINGLE:
      return "single";
    case UCS_THREAD_MODE_SERIALIZED:
      return "serialized";
    case UCS_THREAD_MODE_MULTI:
      return "multi";
    default:
      return "unknown";
  }
}

void check_cuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
  }
}

void check_ucs(ucs_status_t status, const char* what) {
  if (status != UCS_OK) {
    throw std::runtime_error(std::string(what) + ": " + ucs_status_string(status));
  }
}

ucp_worker_attr_t query_worker(ucp_worker_h worker) {
  ucp_worker_attr_t attributes{};
  attributes.field_mask = UCP_WORKER_ATTR_FIELD_THREAD_MODE | UCP_WORKER_ATTR_FIELD_MAX_AM_HEADER;
  check_ucs(ucp_worker_query(worker, &attributes), "ucp_worker_query");
  return attributes;
}

void pin_current_thread(int core) {
  if (core < 0) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  const int status = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (status != 0) {
    throw std::runtime_error("pthread_setaffinity_np(core=" + std::to_string(core) +
                             ") failed: " + std::strerror(status));
  }
}

sockaddr_in parse_ipv4_endpoint(const std::string& endpoint) {
  const auto separator = endpoint.rfind(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 >= endpoint.size()) {
    throw std::invalid_argument("endpoint must be IPv4:port: " + endpoint);
  }
  const std::string address = endpoint.substr(0, separator);
  const unsigned long port = std::stoul(endpoint.substr(separator + 1));
  if (port > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("endpoint port is out of range: " + endpoint);
  }
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(static_cast<std::uint16_t>(port));
  if (inet_pton(AF_INET, address.c_str(), &result.sin_addr) != 1) {
    throw std::invalid_argument("endpoint must contain a numeric IPv4 address: " + endpoint);
  }
  return result;
}

struct UcxEnvironmentBinding {
  std::string net_device;
  std::string gid_index;
};

UcxEnvironmentBinding require_ucx_environment_binding() {
  const char* net_devices = std::getenv("UCX_NET_DEVICES");
  const char* gid_index = std::getenv("UCX_IB_GID_INDEX");
  if (net_devices == nullptr || *net_devices == '\0' || gid_index == nullptr ||
      *gid_index == '\0') {
    throw std::runtime_error(
        "set UCX_NET_DEVICES=<Link-2-RDMA-device:port> and a numeric UCX_IB_GID_INDEX; "
        "automatic UCX device selection is disabled for this example");
  }
  const std::string device(net_devices);
  const std::string gid(gid_index);
  if (device.find(':') == std::string::npos || device.find(',') != std::string::npos) {
    throw std::runtime_error(
        "UCX_NET_DEVICES must select exactly one Link-2 RDMA device and port, for example "
        "mlx5_3:1");
  }
  std::size_t consumed = 0;
  try {
    (void)std::stoul(gid, &consumed);
  } catch (const std::exception&) {
    consumed = 0;
  }
  if (consumed != gid.size()) {
    throw std::runtime_error("UCX_IB_GID_INDEX must be a numeric RoCEv2 GID index");
  }
  return {device, gid};
}

void log_ucx_environment_binding(const UcxEnvironmentBinding& binding) {
  std::cout << " ucx_net_device=" << binding.net_device
            << " ucx_ib_gid_index=" << binding.gid_index;
  if (const char* tls = std::getenv("UCX_TLS"); tls != nullptr && *tls != '\0') {
    std::cout << " ucx_tls=" << tls;
  }
}

std::uint64_t random_nonzero_epoch() {
  std::random_device source;
  const std::uint64_t high = static_cast<std::uint64_t>(source()) << 32U;
  const std::uint64_t low = source();
  const std::uint64_t epoch =
      high ^ low ^ static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
  return epoch == 0 ? 1 : epoch;
}

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

  bool try_push(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || queue_.size() == capacity_) {
      return false;
    }
    queue_.push_back(std::move(value));
    cv_.notify_one();
    return true;
  }

  bool try_pop(T& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    value = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  bool wait_pop(T& value, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    value = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
  }

 private:
  std::size_t capacity_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<T> queue_;
  bool closed_{false};
};

template <typename T>
using SpscQueue = ucx_example::SpscQueue<T>;

struct AtomicStats {
  std::atomic<std::uint64_t> generated{0};
  std::atomic<std::uint64_t> admitted{0};
  std::atomic<std::uint64_t> dropped_no_connection{0};
  std::atomic<std::uint64_t> dropped_no_credit{0};
  std::atomic<std::uint64_t> send_completed{0};
  std::atomic<std::uint64_t> send_failed{0};
  std::atomic<std::uint64_t> outstanding{0};
  std::atomic<std::uint64_t> delivery_unknown{0};
  std::atomic<std::uint64_t> delivered{0};
  std::atomic<std::uint64_t> released{0};
  std::atomic<std::uint64_t> sequence_gaps{0};
  std::atomic<std::uint64_t> validation_failures{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> active_nanoseconds{0};

  TransportStats snapshot() const {
    TransportStats result;
    result.generated = generated.load();
    result.admitted = admitted.load();
    result.dropped_no_connection = dropped_no_connection.load();
    result.dropped_no_credit = dropped_no_credit.load();
    result.send_completed = send_completed.load();
    result.send_failed = send_failed.load();
    result.outstanding = outstanding.load();
    result.delivery_unknown = delivery_unknown.load();
    result.delivered = delivered.load();
    result.released = released.load();
    result.sequence_gaps = sequence_gaps.load();
    result.validation_failures = validation_failures.load();
    result.bytes = bytes.load();
    result.active_nanoseconds = active_nanoseconds.load();
    return result;
  }
};

class RegisteredPool {
 public:
  RegisteredPool() = default;
  RegisteredPool(const RegisteredPool&) = delete;
  RegisteredPool& operator=(const RegisteredPool&) = delete;

  void initialize(ucp_context_h context, MemoryKind kind, int gpu_id, std::size_t slots,
                  std::size_t slot_bytes = kImageBytes) {
    if (slots == 0 || slot_bytes == 0 ||
        slots > std::numeric_limits<std::size_t>::max() / slot_bytes) {
      throw std::invalid_argument("invalid slot count");
    }
    kind_ = kind;
    slot_bytes_ = slot_bytes;
    bytes_ = slots * slot_bytes_;
    check_cuda(cudaSetDevice(gpu_id), "cudaSetDevice");
    if (kind == MemoryKind::host_pinned_mapped) {
      check_cuda(cudaHostAlloc(&ucx_base_, bytes_, cudaHostAllocMapped | cudaHostAllocPortable),
                 "cudaHostAlloc(mapped pool)");
      check_cuda(cudaHostGetDevicePointer(&device_base_, ucx_base_, 0),
                 "cudaHostGetDevicePointer(pool)");
    } else {
      check_cuda(cudaMalloc(&device_base_, bytes_), "cudaMalloc(pool)");
      ucx_base_ = device_base_;
    }

    ucp_mem_map_params_t params{};
    params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH |
                        UCP_MEM_MAP_PARAM_FIELD_MEMORY_TYPE;
    params.address = ucx_base_;
    params.length = bytes_;
    params.memory_type = ucs_memory_type();
    check_ucs(ucp_mem_map(context, &params, &memh_), "ucp_mem_map(pool)");
  }

  void destroy(ucp_context_h context) noexcept {
    if (memh_ != nullptr) {
      const ucs_status_t status = ucp_mem_unmap(context, memh_);
      if (status != UCS_OK) {
        std::cerr << "ucp_mem_unmap failed: " << ucs_status_string(status) << '\n';
      }
      memh_ = nullptr;
    }
    if (device_base_ != nullptr) {
      const cudaError_t status = kind_ == MemoryKind::host_pinned_mapped ? cudaFreeHost(ucx_base_)
                                                                         : cudaFree(device_base_);
      if (status != cudaSuccess) {
        std::cerr << "CUDA pool release failed: " << cudaGetErrorString(status) << '\n';
      }
    }
    ucx_base_ = nullptr;
    device_base_ = nullptr;
    slot_bytes_ = 0;
    bytes_ = 0;
  }

  void* ucx_slot(std::size_t slot) const {
    return static_cast<std::uint8_t*>(ucx_base_) + slot * slot_bytes_;
  }
  void* device_slot(std::size_t slot) const {
    return static_cast<std::uint8_t*>(device_base_) + slot * slot_bytes_;
  }
  void* ucx_offset(std::size_t offset) const {
    return static_cast<std::uint8_t*>(ucx_base_) + offset;
  }
  ucp_mem_h memh() const noexcept {
    return memh_;
  }
  ucs_memory_type_t ucs_memory_type() const noexcept {
    return kind_ == MemoryKind::cuda_device ? UCS_MEMORY_TYPE_CUDA : UCS_MEMORY_TYPE_HOST;
  }

 private:
  MemoryKind kind_{MemoryKind::host_pinned_mapped};
  void* ucx_base_{nullptr};
  void* device_base_{nullptr};
  std::size_t slot_bytes_{0};
  std::size_t bytes_{0};
  ucp_mem_h memh_{nullptr};
};

struct AsyncOperation {
  bool in_api{true};
  bool callback_seen{false};
  bool finalized{false};
  ucs_status_t status{UCS_INPROGRESS};
  std::size_t length{0};
  void* request{nullptr};
  void* callback_request{nullptr};
  bool counted_admitted{false};
  std::function<void(AsyncOperation&)> done;
};

void finalize_operation(AsyncOperation* operation) {
  if (operation->finalized) {
    std::terminate();
  }
  operation->finalized = true;
  void* request = operation->request != nullptr ? operation->request : operation->callback_request;
  if (request != nullptr && !UCS_PTR_IS_ERR(request)) {
    ucp_request_free(request);
  }
  operation->done(*operation);
  delete operation;
}

void reconcile_nbx_result(AsyncOperation* operation, void* result) {
  operation->in_api = false;
  if (UCS_PTR_IS_ERR(result)) {
    operation->status = UCS_PTR_STATUS(result);
    operation->callback_seen = true;
  } else if (result == nullptr) {
    operation->status = UCS_OK;
    operation->callback_seen = true;
  } else {
    operation->request = result;
  }
  if (operation->callback_seen) {
    finalize_operation(operation);
  }
}

void send_callback(void* request, ucs_status_t status, void* user_data) {
  auto* operation = static_cast<AsyncOperation*>(user_data);
  operation->callback_request = request;
  operation->status = status;
  operation->callback_seen = true;
  if (!operation->in_api) {
    finalize_operation(operation);
  }
}

void receive_callback(void* request, ucs_status_t status, std::size_t length, void* user_data) {
  auto* operation = static_cast<AsyncOperation*>(user_data);
  operation->callback_request = request;
  operation->status = status;
  operation->length = length;
  operation->callback_seen = true;
  if (!operation->in_api) {
    finalize_operation(operation);
  }
}

ucp_context_h make_context(const char* name) {
  ucp_config_t* config = nullptr;
  check_ucs(ucp_config_read(nullptr, nullptr, &config), "ucp_config_read");
  ucp_params_t params{};
  params.field_mask =
      UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_NAME | UCP_PARAM_FIELD_ESTIMATED_NUM_EPS;
  params.features = UCP_FEATURE_AM;
  params.name = name;
  params.estimated_num_eps = 1;
  ucp_context_h context = nullptr;
  const ucs_status_t status = ucp_init(&params, config, &context);
  ucp_config_release(config);
  check_ucs(status, "ucp_init");
  return context;
}

ucp_worker_h make_worker(ucp_context_h context) {
  ucp_worker_params_t params{};
  params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  params.thread_mode = UCS_THREAD_MODE_SINGLE;
  ucp_worker_h worker = nullptr;
  check_ucs(ucp_worker_create(context, &params, &worker), "ucp_worker_create");
  const auto attributes = query_worker(worker);
  const std::size_t required_header = std::max(kControlWireBytes, kDataHeaderWireBytes);
  if (attributes.max_am_header < required_header) {
    ucp_worker_destroy(worker);
    throw std::runtime_error("UCP worker maximum Active Message header is " +
                             std::to_string(attributes.max_am_header) + " bytes; at least " +
                             std::to_string(required_header) + " bytes are required");
  }
  return worker;
}

struct EndpointCloseResult {
  bool quiesced{false};
  ucs_status_t status{UCS_OK};
};

EndpointCloseResult close_endpoint(ucp_worker_h worker, ucp_ep_h& endpoint, bool force,
                                   std::chrono::steady_clock::duration timeout) noexcept {
  if (endpoint == nullptr) {
    return {true, UCS_OK};
  }
  ucp_request_param_t params{};
  params.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
  params.flags = force ? UCP_EP_CLOSE_FLAG_FORCE : 0;
  void* request = ucp_ep_close_nbx(endpoint, &params);
  endpoint = nullptr;
  if (UCS_PTR_IS_ERR(request)) {
    return {false, UCS_PTR_STATUS(request)};
  }
  if (request == nullptr) {
    return {true, UCS_OK};
  }
  const Clock::time_point deadline = Clock::now() + timeout;
  ucs_status_t status = ucp_request_check_status(request);
  while (status == UCS_INPROGRESS) {
    ucp_worker_progress(worker);
    if (Clock::now() >= deadline) {
      return {false, UCS_ERR_TIMED_OUT};
    }
    status = ucp_request_check_status(request);
  }
  ucp_request_free(request);
  return {true, status};
}

}  // namespace

class ExternalBatchProducer::Impl {
 public:
  explicit Impl(ExternalBatchProducerOptions options)
      : options_(std::move(options)),
        free_slots_(options_.batch_slot_count),
        ready_batches_(options_.batch_slot_count),
        retired_batches_(options_.batch_slot_count),
        sequence_ledger_(options_.image_count) {
    if (options_.listen_endpoint.empty() || options_.image_count == 0 ||
        options_.batch_slot_count == 0 || options_.max_receiver_queue_depth == 0) {
      throw std::invalid_argument(
          "external producer endpoint, image count, batch slots, and receiver depth are required");
    }
  }

  ~Impl() {
    close();
  }

  void start() {
    bool expected = false;
    if (!start_called_.compare_exchange_strong(expected, true)) {
      throw std::logic_error("ExternalBatchProducer::start called more than once");
    }
    thread_ = std::thread([this] { progress_main(); });
    std::unique_lock<std::mutex> lock(start_mutex_);
    start_cv_.wait(lock, [this] { return started_; });
    if (!start_error_.empty()) {
      const std::string error = start_error_;
      lock.unlock();
      if (thread_.joinable()) {
        thread_.join();
      }
      finalize_storage();
      throw std::runtime_error(error);
    }
  }

  void wait_for_receiver() {
    const Clock::time_point deadline = Clock::now() + options_.timeout;
    while (!receiver_ready_.load(std::memory_order_acquire)) {
      if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        throw std::runtime_error(error_.empty() ? "external producer failed before ACCEPT"
                                                : error_);
      }
      if (!running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("external producer stopped before ACCEPT");
      }
      if (Clock::now() >= deadline) {
        throw std::runtime_error("timed out waiting for receiver ACCEPT");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::optional<ExternalBatchProducer::AcquiredSlot> try_acquire() {
    if (!running_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire) ||
        finish_requested_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    FreeSlot free;
    if (!free_slots_.try_pop(free)) {
      return std::nullopt;
    }
    if (free.slot >= options_.batch_slot_count) {
      fail("free-slot queue returned an invalid slot");
      return std::nullopt;
    }
    Slot& slot = slots_[free.slot];
    if (slot.generation.load(std::memory_order_acquire) != free.generation) {
      fail("free-slot queue returned a stale generation");
      return std::nullopt;
    }
    SlotState expected = SlotState::free;
    if (!slot.state.compare_exchange_strong(expected, SlotState::acquired,
                                            std::memory_order_acq_rel)) {
      fail("free-slot queue returned a slot that is not free");
      return std::nullopt;
    }
    if (free.generation == std::numeric_limits<std::uint64_t>::max()) {
      fail("external producer slot generation overflow");
      return std::nullopt;
    }
    const std::uint64_t generation = free.generation + 1;
    slot.generation.store(generation, std::memory_order_release);
    leases_issued_.store(true, std::memory_order_release);
    return ExternalBatchProducer::AcquiredSlot{pool_.ucx_slot(free.slot),
                                               pool_.device_slot(free.slot),
                                               detail::kExternalBatchBytes, free.slot, generation};
  }

  void submit_after(std::size_t slot_index, std::uint64_t generation, std::uint64_t first_sequence,
                    std::uint32_t image_count, cudaStream_t processing_stream) {
    if (processing_stream == nullptr || slot_index >= options_.batch_slot_count) {
      throw std::invalid_argument("submit_after requires a valid batch slot and CUDA stream");
    }
    enqueue_after(slot_index, generation, first_sequence, image_count, processing_stream, false);
  }

  void cancel(std::size_t slot_index, std::uint64_t generation, std::uint64_t first_sequence,
              std::uint32_t image_count) {
    enqueue_after(slot_index, generation, first_sequence, image_count, nullptr, true, false);
  }

  void release_unused(std::size_t slot_index, std::uint64_t generation) {
    enqueue_after(slot_index, generation, 0, 0, nullptr, false, true);
  }

  std::optional<RetiredBatch> poll_retired() {
    RetiredBatch retired;
    if (!retired_batches_.try_pop(retired)) {
      return std::nullopt;
    }
    if (retired.slot >= options_.batch_slot_count) {
      fail("retired-batch queue returned an invalid slot");
      return std::nullopt;
    }
    Slot& slot = slots_[retired.slot];
    if (slot.generation.load(std::memory_order_acquire) != retired.generation ||
        slot.state.load(std::memory_order_acquire) != SlotState::free) {
      fail("retired-batch queue returned a stale or non-free slot");
      return std::nullopt;
    }
    // Publishing the free lease here couples reacquisition to the RX owner's
    // observation of retirement. Publishing it from the progress thread would
    // let try_acquire() race ahead of the local EgressQueued -> Free transition.
    if (!free_slots_.try_push({retired.slot, retired.generation})) {
      fail("external producer free-slot queue overflow after retirement");
      return std::nullopt;
    }
    return retired;
  }

  void finish_input(std::uint64_t total_generated) {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    if (!running_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire)) {
      throw std::logic_error("finish_input called while the external producer is not running");
    }
    if (finish_requested_.load(std::memory_order_acquire)) {
      throw std::logic_error("finish_input called more than once");
    }
    if (total_generated != options_.image_count) {
      throw std::invalid_argument("finish_input total does not match the fixed HELLO image count");
    }
    stats_.generated.store(total_generated, std::memory_order_release);
    dropped_before_submit_.store(sequence_ledger_.dropped_before_submit(),
                                 std::memory_order_release);
    finish_requested_.store(true, std::memory_order_release);
  }

  ExternalBatchProducerStats stats() const {
    return {stats_.snapshot(), submitted_images_.load(std::memory_order_acquire),
            dropped_before_submit_.load(std::memory_order_acquire),
            submitted_batches_.load(std::memory_order_acquire),
            retired_batches_count_.load(std::memory_order_acquire)};
  }

  std::optional<std::string> error() const {
    if (!failed_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
  }

  void acknowledge_local_quiescence() noexcept {
    local_quiesced_.store(true, std::memory_order_release);
  }

  void close() noexcept {
    if (!start_called_.load(std::memory_order_acquire)) {
      return;
    }
    if (!finish_requested_.load(std::memory_order_acquire)) {
      stop_requested_.store(true, std::memory_order_release);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    finalize_storage();
  }

 private:
  enum class SlotState : std::uint8_t {
    free,
    acquired,
    ready,
    waiting_cuda,
    sending,
    retirement_pending,
  };

  struct Slot {
    std::atomic<SlotState> state{SlotState::free};
    std::atomic<std::uint64_t> generation{0};
    cudaEvent_t ready_event{nullptr};
    std::uint64_t first_sequence{0};
    std::uint32_t image_count{0};
    std::uint32_t admitted{0};
    std::uint32_t dropped_no_credit{0};
    std::uint32_t sends_outstanding{0};
  };

  struct FreeSlot {
    std::size_t slot{0};
    std::uint64_t generation{0};
  };

  struct ReadyBatch {
    std::size_t slot{0};
    std::uint64_t generation{0};
    std::uint64_t first_sequence{0};
    std::uint32_t image_count{0};
    cudaEvent_t processing_done{nullptr};
    bool canceled{false};
    bool release_only{false};
  };

  void enqueue_after(std::size_t slot_index, std::uint64_t generation, std::uint64_t first_sequence,
                     std::uint32_t image_count, cudaStream_t completion_stream, bool canceled,
                     bool release_only = false) {
    if (!release_only && !canceled && completion_stream == nullptr) {
      throw std::invalid_argument("external batch handoff requires a CUDA stream");
    }
    if (slot_index >= options_.batch_slot_count) {
      throw std::invalid_argument("external batch handoff slot is out of range");
    }
    if (!running_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire)) {
      throw std::logic_error("external batch handoff called while producer is not running");
    }

    std::lock_guard<std::mutex> lock(submit_mutex_);
    if (finish_requested_.load(std::memory_order_acquire)) {
      throw std::logic_error("external batch handoff called after finish_input");
    }
    Slot& slot = slots_[slot_index];
    if (slot.generation.load(std::memory_order_acquire) != generation ||
        slot.state.load(std::memory_order_acquire) != SlotState::acquired) {
      throw std::logic_error("external batch handoff received a stale or non-acquired slot");
    }
    cudaEvent_t completion_event = nullptr;
    if (!release_only && !canceled) {
      check_cuda(cudaEventRecord(slot.ready_event, completion_stream),
                 "cudaEventRecord(external batch ready)");
      completion_event = slot.ready_event;
    }
    std::string sequence_error;
    const bool sequence_valid =
        release_only ||
        (canceled ? sequence_ledger_.record_drop(first_sequence, image_count, sequence_error)
                  : sequence_ledger_.record(first_sequence, image_count, sequence_error));
    if (!sequence_valid) {
      throw std::invalid_argument(sequence_error);
    }
    slot.state.store(SlotState::ready, std::memory_order_release);
    ReadyBatch ready{slot_index,       generation, first_sequence, image_count,
                     completion_event, canceled,   release_only};
    if (!ready_batches_.try_push(ready)) {
      fail("ready-batch queue overflow");
      throw std::runtime_error("ready-batch queue overflow");
    }
    if (!canceled && !release_only) {
      submitted_images_.fetch_add(image_count, std::memory_order_relaxed);
      submitted_batches_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void progress_main() noexcept {
    try {
      pin_current_thread(options_.cpu_core);
      check_cuda(cudaSetDevice(options_.gpu_id), "external producer cudaSetDevice");
      ucx_binding_ = require_ucx_environment_binding();
      context_ = make_context("daqiri-ucx-external-batch-producer");
      worker_ = make_worker(context_);
      pool_.initialize(context_, options_.memory_kind, options_.gpu_id, options_.batch_slot_count,
                       detail::kExternalBatchBytes);
      slots_ = std::make_unique<Slot[]>(options_.batch_slot_count);
      for (std::size_t index = 0; index < options_.batch_slot_count; ++index) {
        check_cuda(cudaEventCreateWithFlags(&slots_[index].ready_event, cudaEventDisableTiming),
                   "cudaEventCreate(external batch ready)");
        if (!free_slots_.try_push({index, 0})) {
          throw std::runtime_error("could not seed external producer free-slot queue");
        }
      }
      install_handlers();
      create_listener();
      log_startup();
      running_.store(true, std::memory_order_release);
      signal_started({});

      connection_deadline_ = Clock::now() + options_.timeout;
      while (!failed_.load(std::memory_order_acquire) && !done_) {
        const unsigned progress = ucp_worker_progress(worker_);
        drain_ready_batches();
        process_ready_batches();
        flush_retirements();
        maybe_send_eos();

        if (stop_requested_.load(std::memory_order_acquire)) {
          fail("external producer closed before finish_input");
        } else if (!active_ && Clock::now() > connection_deadline_) {
          fail("timed out waiting for HELLO");
        } else if (active_ && !eos_sent_ && Clock::now() > activity_deadline_) {
          fail("external producer made no progress before the activity timeout");
        } else if (eos_sent_ && Clock::now() > eos_deadline_) {
          fail("timed out waiting for EOS_ACK");
        }
        if (progress == 0) {
          std::this_thread::yield();
        }
      }
    } catch (const std::exception& exception) {
      fail(exception.what());
      signal_started(exception.what());
    }
    running_.store(false, std::memory_order_release);
    quiesce_transport();
  }

  void signal_started(std::string error) {
    std::lock_guard<std::mutex> lock(start_mutex_);
    if (started_) {
      return;
    }
    start_error_ = std::move(error);
    started_ = true;
    start_cv_.notify_all();
  }

  void log_startup() {
    unsigned major = 0, minor = 0, release = 0;
    ucp_get_version(&major, &minor, &release);
    const auto worker_attributes = query_worker(worker_);
    std::cout << "role=external-batch-producer ucx=" << major << '.' << minor << '.' << release
              << " memory_kind=" << memory_kind_name(options_.memory_kind)
              << " gpu=" << options_.gpu_id << " core=" << options_.cpu_core
              << " batch_slots=" << options_.batch_slot_count
              << " batch_bytes=" << detail::kExternalBatchBytes
              << " max_receiver_depth=" << options_.max_receiver_queue_depth
              << " worker_thread_mode=" << thread_mode_name(worker_attributes.thread_mode)
              << " max_am_header=" << worker_attributes.max_am_header;
    log_ucx_environment_binding(ucx_binding_);
    std::cout << '\n';
  }

  void install_handlers() {
    ucp_am_handler_param_t handler{};
    handler.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID | UCP_AM_HANDLER_PARAM_FIELD_CB |
                         UCP_AM_HANDLER_PARAM_FIELD_ARG;
    handler.id = kControlAmId;
    handler.cb = &Impl::control_callback;
    handler.arg = this;
    check_ucs(ucp_worker_set_am_recv_handler(worker_, &handler),
              "ucp_worker_set_am_recv_handler(external control)");
  }

  void create_listener() {
    listen_address_ = parse_ipv4_endpoint(options_.listen_endpoint);
    ucp_listener_params_t params{};
    params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR | UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    params.sockaddr.addr = reinterpret_cast<const sockaddr*>(&listen_address_);
    params.sockaddr.addrlen = sizeof(listen_address_);
    params.conn_handler.cb = &Impl::connection_callback;
    params.conn_handler.arg = this;
    check_ucs(ucp_listener_create(worker_, &params, &listener_), "ucp_listener_create(external)");
  }

  static void connection_callback(ucp_conn_request_h request, void* argument) {
    auto* self = static_cast<Impl*>(argument);
    if (self->endpoint_ != nullptr || self->failed_.load(std::memory_order_acquire)) {
      ucp_listener_reject(self->listener_, request);
      return;
    }
    ucp_ep_params_t params{};
    params.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST | UCP_EP_PARAM_FIELD_ERR_HANDLER |
                        UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    params.conn_request = request;
    params.err_mode = UCP_ERR_HANDLING_MODE_PEER;
    params.err_handler.cb = &Impl::endpoint_error_callback;
    params.err_handler.arg = self;
    const ucs_status_t status = ucp_ep_create(self->worker_, &params, &self->endpoint_);
    if (status != UCS_OK) {
      self->fail(std::string("ucp_ep_create(external server): ") + ucs_status_string(status));
    }
  }

  static void endpoint_error_callback(void* argument, ucp_ep_h, ucs_status_t status) {
    static_cast<Impl*>(argument)->fail(std::string("external producer endpoint failure: ") +
                                       ucs_status_string(status));
  }

  static ucs_status_t control_callback(void* argument, const void* header,
                                       std::size_t header_length, void*, std::size_t,
                                       const ucp_am_recv_param_t*) {
    auto* self = static_cast<Impl*>(argument);
    ControlMessage message;
    std::string error;
    if (!decode_control(header, header_length, message, error)) {
      self->fail("invalid external producer control message: " + error);
      return UCS_OK;
    }
    self->handle_control(message);
    return UCS_OK;
  }

  void handle_control(const ControlMessage& message) {
    if (message.type == ControlType::hello) {
      if (active_ || message.connection_epoch != 0 || message.stream_id != kLogicalStreamId ||
          message.value0 != options_.image_count ||
          static_cast<std::uint32_t>(message.value2) != kImageBytes ||
          message.memory_kind != options_.memory_kind) {
        send_control({ControlType::reject, 0, kLogicalStreamId, 0, 1, 0, 0, options_.memory_kind});
        fail("HELLO does not match external producer configuration");
        return;
      }
      const std::uint64_t requested_depth = message.value2 >> 32U;
      if (requested_depth == 0 || requested_depth > options_.max_receiver_queue_depth) {
        send_control({ControlType::reject, 0, kLogicalStreamId, 0, 2, 0, 0, options_.memory_kind});
        fail("HELLO queue depth exceeds external producer limit");
        return;
      }
      connection_epoch_ = random_nonzero_epoch();
      initial_credits_ = requested_depth;
      credits_ = requested_depth;
      ControlMessage accept{ControlType::accept,
                            0,
                            kLogicalStreamId,
                            connection_epoch_,
                            options_.image_count,
                            message.value1,
                            (requested_depth << 32U) | kImageBytes,
                            options_.memory_kind};
      send_control(accept);
      active_ = true;
      active_start_ = Clock::now();
      note_activity();
      receiver_ready_.store(true, std::memory_order_release);
      return;
    }
    if (message.connection_epoch != connection_epoch_) {
      fail("external producer control message has a stale connection epoch");
      return;
    }
    if (message.type == ControlType::credit) {
      const std::uint64_t admitted = stats_.admitted.load(std::memory_order_acquire);
      if (message.value0 < released_total_ || message.value0 > admitted) {
        fail("invalid cumulative CREDIT at external producer");
        return;
      }
      released_total_ = message.value0;
      credits_ = initial_credits_ + released_total_ - admitted;
      if (credits_ > initial_credits_) {
        fail("CREDIT overcommits external producer receiver slots");
      }
      note_activity();
    } else if (message.type == ControlType::eos_ack) {
      if (!eos_sent_ || message.value0 != stats_.admitted.load(std::memory_order_acquire)) {
        fail("EOS_ACK delivered count mismatch at external producer");
      } else {
        const Clock::time_point transfer_start = first_data_time_.value_or(active_start_);
        const Clock::time_point transfer_end = last_data_time_.value_or(transfer_start);
        stats_.active_nanoseconds.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(transfer_end - transfer_start)
                .count(),
            std::memory_order_release);
        done_ = true;
      }
    } else {
      fail("unexpected control message at external producer");
    }
  }

  void send_control(ControlMessage message) {
    if (endpoint_ == nullptr) {
      fail("external producer cannot send control without an endpoint");
      return;
    }
    auto* operation = new AsyncOperation;
    operation->done = [this](AsyncOperation& completed) {
      if (completed.status != UCS_OK) {
        fail(std::string("external producer control send failed: ") +
             ucs_status_string(completed.status));
      }
    };
    const auto wire = encode_control(message);
    ucp_request_param_t params{};
    params.op_attr_mask =
        UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA | UCP_OP_ATTR_FIELD_FLAGS;
    params.cb.send = send_callback;
    params.user_data = operation;
    params.flags = UCP_AM_SEND_FLAG_COPY_HEADER;
    void* result =
        ucp_am_send_nbx(endpoint_, kControlAmId, wire.data(), wire.size(), nullptr, 0, &params);
    reconcile_nbx_result(operation, result);
  }

  void drain_ready_batches() {
    ReadyBatch ready;
    while (ready_batches_.try_pop(ready)) {
      if (ready.slot >= options_.batch_slot_count) {
        fail("ready-batch queue returned an invalid slot");
        return;
      }
      Slot& slot = slots_[ready.slot];
      if (slot.generation.load(std::memory_order_acquire) != ready.generation ||
          slot.state.load(std::memory_order_acquire) != SlotState::ready) {
        fail("ready-batch queue returned a stale slot generation");
        return;
      }
      slot.state.store(SlotState::waiting_cuda, std::memory_order_release);
      waiting_cuda_.push_back(ready);
      note_activity();
    }
  }

  void process_ready_batches() {
    if (!active_) {
      return;
    }
    while (!waiting_cuda_.empty()) {
      const ReadyBatch ready = waiting_cuda_.front();
      if (options_.wait_for_credit && !ready.canceled && !ready.release_only) {
        if (ready.image_count > initial_credits_) {
          fail("wait-for-credit batch exceeds the negotiated receiver depth");
          return;
        }
        if (credits_ < ready.image_count) {
          return;
        }
      }
      if (ready.processing_done != nullptr) {
        const cudaError_t event_status = cudaEventQuery(ready.processing_done);
        if (event_status == cudaErrorNotReady) {
          return;
        }
        if (event_status != cudaSuccess) {
          fail(std::string("external producer processing event failed: ") +
               cudaGetErrorString(event_status));
          return;
        }
      }
      waiting_cuda_.pop_front();
      if (ready.canceled || ready.release_only) {
        cancel_ready_batch(ready);
      } else {
        admit_ready_batch(ready);
      }
      if (failed_.load(std::memory_order_acquire)) {
        return;
      }
    }
  }

  void cancel_ready_batch(const ReadyBatch& ready) {
    Slot& slot = slots_[ready.slot];
    slot.first_sequence = ready.first_sequence;
    slot.image_count = ready.image_count;
    slot.admitted = 0;
    slot.dropped_no_credit = 0;
    slot.sends_outstanding = 0;
    slot.state.store(SlotState::sending, std::memory_order_release);
    note_activity();
    queue_retirement(ready.slot, ready.generation);
  }

  void admit_ready_batch(const ReadyBatch& ready) {
    Slot& slot = slots_[ready.slot];
    slot.first_sequence = ready.first_sequence;
    slot.image_count = ready.image_count;
    const detail::BatchAdmission admission =
        detail::decide_batch_admission(ready.image_count, credits_);
    slot.admitted = admission.admitted;
    slot.dropped_no_credit = admission.dropped;
    slot.sends_outstanding = admission.admitted;
    slot.state.store(SlotState::sending, std::memory_order_release);

    credits_ -= admission.admitted;
    stats_.dropped_no_credit.fetch_add(admission.dropped, std::memory_order_relaxed);
    note_activity();
    if (admission.admitted == 0) {
      queue_retirement(ready.slot, ready.generation);
      return;
    }
    for (std::uint32_t image = 0; image < admission.admitted; ++image) {
      if (!send_image(ready.slot, ready.generation, image)) {
        return;
      }
    }
  }

  bool send_image(std::size_t slot_index, std::uint64_t generation, std::uint32_t image_index) {
    Slot& slot = slots_[slot_index];
    DataHeader header;
    header.connection_epoch = connection_epoch_;
    header.sequence = slot.first_sequence + image_index;
    header.admission_ordinal = stats_.admitted.load(std::memory_order_relaxed);
    const auto wire = encode_data_header(header);

    auto* operation = new AsyncOperation;
    operation->done = [this, slot_index, generation](AsyncOperation& completed) {
      send_completed(slot_index, generation, completed.status, completed.counted_admitted);
    };
    ucp_request_param_t params{};
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA |
                          UCP_OP_ATTR_FIELD_FLAGS | UCP_OP_ATTR_FIELD_MEMORY_TYPE |
                          UCP_OP_ATTR_FIELD_MEMH;
    params.cb.send = send_callback;
    params.user_data = operation;
    params.flags = UCP_AM_SEND_FLAG_RNDV | UCP_AM_SEND_FLAG_COPY_HEADER;
    params.memory_type = pool_.ucs_memory_type();
    params.memh = pool_.memh();
    const std::size_t offset = slot_index * detail::kExternalBatchBytes +
                               static_cast<std::size_t>(image_index) * kImageBytes;
    void* result = ucp_am_send_nbx(endpoint_, kDataAmId, wire.data(), wire.size(),
                                   pool_.ucx_offset(offset), kImageBytes, &params);
    if (UCS_PTR_IS_ERR(result)) {
      reconcile_nbx_result(operation, result);
      return false;
    }
    const std::uint64_t prior_admitted = stats_.admitted.fetch_add(1, std::memory_order_relaxed);
    if (prior_admitted == 0) {
      first_data_time_ = Clock::now();
    }
    stats_.outstanding.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes.fetch_add(kImageBytes, std::memory_order_relaxed);
    operation->counted_admitted = true;
    reconcile_nbx_result(operation, result);
    return true;
  }

  void send_completed(std::size_t slot_index, std::uint64_t generation, ucs_status_t status,
                      bool counted_admitted) {
    if (slot_index >= options_.batch_slot_count ||
        slots_[slot_index].generation.load(std::memory_order_acquire) != generation) {
      fail("external DATA completion referred to a stale slot");
      return;
    }
    Slot& slot = slots_[slot_index];
    if (slot.state.load(std::memory_order_acquire) != SlotState::sending ||
        slot.sends_outstanding == 0) {
      fail("external DATA completion violated slot ownership");
      return;
    }
    --slot.sends_outstanding;
    if (counted_admitted) {
      stats_.outstanding.fetch_sub(1, std::memory_order_relaxed);
    } else {
      ++credits_;
    }
    if (status != UCS_OK) {
      stats_.send_failed.fetch_add(1, std::memory_order_relaxed);
      fail(std::string("external DATA send failed (delivery unknown): ") +
           ucs_status_string(status));
      return;
    }
    stats_.send_completed.fetch_add(1, std::memory_order_relaxed);
    last_data_time_ = Clock::now();
    note_activity();
    if (slot.sends_outstanding == 0) {
      queue_retirement(slot_index, generation);
    }
  }

  void queue_retirement(std::size_t slot_index, std::uint64_t generation) {
    Slot& slot = slots_[slot_index];
    if (slot.generation.load(std::memory_order_acquire) != generation ||
        slot.state.load(std::memory_order_acquire) != SlotState::sending) {
      fail("external batch retirement violated slot ownership");
      return;
    }
    slot.state.store(SlotState::retirement_pending, std::memory_order_release);
    pending_retirements_.push_back(slot_index);
  }

  void flush_retirements() {
    while (!pending_retirements_.empty()) {
      const std::size_t slot_index = pending_retirements_.front();
      Slot& slot = slots_[slot_index];
      const std::uint64_t generation = slot.generation.load(std::memory_order_acquire);
      RetiredBatch retired{slot_index,       generation,    slot.first_sequence,
                           slot.image_count, slot.admitted, slot.dropped_no_credit};
      slot.state.store(SlotState::free, std::memory_order_release);
      if (!retired_batches_.try_push(retired)) {
        slot.state.store(SlotState::retirement_pending, std::memory_order_release);
        return;
      }
      pending_retirements_.pop_front();
      retired_batches_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void maybe_send_eos() {
    if (eos_sent_ || !active_ || !finish_requested_.load(std::memory_order_acquire) ||
        !ready_batches_.empty() || !waiting_cuda_.empty() || !pending_retirements_.empty()) {
      return;
    }
    for (std::size_t index = 0; index < options_.batch_slot_count; ++index) {
      if (slots_[index].state.load(std::memory_order_acquire) != SlotState::free) {
        return;
      }
    }
    const std::uint64_t generated = stats_.generated.load(std::memory_order_acquire);
    const std::uint64_t admitted = stats_.admitted.load(std::memory_order_acquire);
    if (admitted > generated || stats_.send_completed.load(std::memory_order_acquire) != admitted) {
      fail("external producer EOS accounting invariant failed");
      return;
    }
    ControlMessage eos{ControlType::eos, 0,        kLogicalStreamId,     connection_epoch_,
                       generated,        admitted, generated - admitted, options_.memory_kind};
    send_control(eos);
    eos_sent_ = true;
    eos_deadline_ = Clock::now() + options_.timeout;
  }

  void fail(std::string message) noexcept {
    bool expected = false;
    if (failed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      const std::uint64_t admitted = stats_.admitted.load(std::memory_order_acquire);
      stats_.delivery_unknown.store(admitted >= released_total_ ? admitted - released_total_ : 0,
                                    std::memory_order_release);
      std::lock_guard<std::mutex> lock(error_mutex_);
      error_ = std::move(message);
    }
  }

  void note_activity() noexcept {
    activity_deadline_ = Clock::now() + options_.timeout;
  }

  void quiesce_transport() noexcept {
    const EndpointCloseResult close_result = close_endpoint(
        worker_, endpoint_, failed_.load(std::memory_order_acquire) || done_, options_.timeout);
    if (close_result.status != UCS_OK) {
      fail(std::string("external producer endpoint close failed: ") +
           ucs_status_string(close_result.status));
    }
    if (listener_ != nullptr) {
      ucp_listener_destroy(listener_);
      listener_ = nullptr;
    }
    if (close_result.quiesced && worker_ != nullptr) {
      ucp_worker_destroy(worker_);
      worker_ = nullptr;
    }
    transport_quiesced_.store(close_result.quiesced, std::memory_order_release);
  }

  void finalize_storage() noexcept {
    std::lock_guard<std::mutex> lock(cleanup_mutex_);
    if (context_ == nullptr) {
      return;
    }
    if (!transport_quiesced_.load(std::memory_order_acquire)) {
      std::cerr << "external producer UCX resources retained because endpoint close did not "
                   "quiesce\n";
      return;
    }
    if (failed_.load(std::memory_order_acquire) && leases_issued_.load(std::memory_order_acquire) &&
        !local_quiesced_.load(std::memory_order_acquire)) {
      std::cerr << "external producer pool retained until local CUDA borrowers are quiescent\n";
      return;
    }
    if (slots_ != nullptr) {
      for (std::size_t index = 0; index < options_.batch_slot_count; ++index) {
        if (slots_[index].ready_event != nullptr) {
          cudaEventDestroy(slots_[index].ready_event);
          slots_[index].ready_event = nullptr;
        }
      }
    }
    pool_.destroy(context_);
    ucp_cleanup(context_);
    context_ = nullptr;
  }

  ExternalBatchProducerOptions options_;
  UcxEnvironmentBinding ucx_binding_;
  ucp_context_h context_{nullptr};
  ucp_worker_h worker_{nullptr};
  ucp_listener_h listener_{nullptr};
  ucp_ep_h endpoint_{nullptr};
  sockaddr_in listen_address_{};
  RegisteredPool pool_;
  std::unique_ptr<Slot[]> slots_;
  SpscQueue<FreeSlot> free_slots_;
  SpscQueue<ReadyBatch> ready_batches_;
  SpscQueue<RetiredBatch> retired_batches_;
  std::deque<ReadyBatch> waiting_cuda_;
  std::deque<std::size_t> pending_retirements_;
  detail::SubmittedSequenceLedger sequence_ledger_;
  AtomicStats stats_;
  std::atomic<std::uint64_t> submitted_images_{0};
  std::atomic<std::uint64_t> dropped_before_submit_{0};
  std::atomic<std::uint64_t> submitted_batches_{0};
  std::atomic<std::uint64_t> retired_batches_count_{0};
  std::thread thread_;
  std::atomic<bool> start_called_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> finish_requested_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> receiver_ready_{false};
  std::atomic<bool> leases_issued_{false};
  std::atomic<bool> local_quiesced_{false};
  std::atomic<bool> transport_quiesced_{false};
  mutable std::mutex error_mutex_;
  std::string error_;
  std::mutex start_mutex_;
  std::condition_variable start_cv_;
  std::mutex submit_mutex_;
  std::mutex cleanup_mutex_;
  bool started_{false};
  std::string start_error_;
  bool active_{false};
  bool eos_sent_{false};
  bool done_{false};
  std::uint64_t connection_epoch_{0};
  std::uint64_t initial_credits_{0};
  std::uint64_t released_total_{0};
  std::uint64_t credits_{0};
  Clock::time_point connection_deadline_{};
  Clock::time_point activity_deadline_{};
  Clock::time_point eos_deadline_{};
  Clock::time_point active_start_{};
  std::optional<Clock::time_point> first_data_time_;
  std::optional<Clock::time_point> last_data_time_;
};

class Receiver::Impl {
 public:
  explicit Impl(ReceiverOptions options)
      : options_(std::move(options)),
        completed_(options_.queue_depth),
        releases_(options_.queue_depth) {
    if (options_.server_endpoint.empty() || options_.local_endpoint.empty() ||
        options_.image_count == 0 || options_.queue_depth == 0) {
      throw std::invalid_argument("receiver endpoints, image count, and depth are required");
    }
  }

  ~Impl() {
    close();
  }

  void start() {
    bool expected = false;
    if (!start_called_.compare_exchange_strong(expected, true)) {
      throw std::logic_error("Receiver::start called more than once");
    }
    thread_ = std::thread([this] { progress_main(); });
    std::unique_lock<std::mutex> lock(start_mutex_);
    start_cv_.wait(lock, [this] { return started_; });
    if (!start_error_.empty()) {
      lock.unlock();
      if (thread_.joinable()) {
        thread_.join();
      }
      finalize_storage();
      throw std::runtime_error(start_error_);
    }
  }

  ReceiveResult receive(std::chrono::milliseconds timeout) {
    Completed completion;
    if (completed_.wait_pop(completion, timeout)) {
      ReceivedImage image(completion.device_data, kImageBytes, completion.header.sequence,
                          completion.preceding_gap, completion.slot, completion.generation);
      return {ReceiveStatus::image, std::move(image), {}};
    }
    if (failed_.load()) {
      return {ReceiveStatus::failed, std::nullopt, error()};
    }
    if (eos_ready_.load()) {
      return {ReceiveStatus::end_of_stream, std::nullopt, {}};
    }
    return {ReceiveStatus::timeout, std::nullopt, {}};
  }

  void release(std::size_t slot, std::uint64_t generation) {
    if (!releases_.try_push({slot, generation, nullptr})) {
      fail("receiver release queue overflow or closed");
    }
  }

  void release_after(std::size_t slot, std::uint64_t generation, cudaStream_t stream) {
    if (stream == nullptr || generation == 0 || slot >= slots_.size()) {
      throw std::invalid_argument("release_after requires a valid generation and CUDA stream");
    }
    cudaEvent_t event = slots_[slot].release_event;
    const cudaError_t record_status = cudaEventRecord(event, stream);
    if (record_status != cudaSuccess) {
      check_cuda(record_status, "cudaEventRecord(receiver release)");
    }
    if (!releases_.try_push({slot, generation, event})) {
      cudaEventSynchronize(event);
      fail("receiver release queue overflow or closed");
    }
  }

  TransportStats stats() const {
    return stats_.snapshot();
  }

  std::optional<std::string> error_optional() const {
    if (!failed_.load()) {
      return std::nullopt;
    }
    return error();
  }

  void close() {
    if (!start_called_.load(std::memory_order_acquire)) {
      return;
    }
    stop_.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
    drain_releases_for_close();
    completed_.close();
    releases_.close();
    finalize_storage();
  }

 private:
  enum class SlotState { free, receiving, delivered, release_pending };
  struct Slot {
    SlotState state{SlotState::free};
    std::uint64_t generation{0};
    DataHeader header{};
    std::uint64_t preceding_gap{0};
    cudaEvent_t release_event{nullptr};
  };
  struct Completed {
    std::size_t slot{0};
    std::uint64_t generation{0};
    void* device_data{nullptr};
    DataHeader header{};
    std::uint64_t preceding_gap{0};
  };
  struct ReleaseRequest {
    std::size_t slot{0};
    std::uint64_t generation{0};
    cudaEvent_t event{nullptr};
  };

  void progress_main() noexcept {
    try {
      pin_current_thread(options_.cpu_core);
      check_cuda(cudaSetDevice(options_.gpu_id), "receiver cudaSetDevice");
      ucx_binding_ = require_ucx_environment_binding();
      context_ = make_context("daqiri-ucx-image-receiver");
      worker_ = make_worker(context_);
      pool_.initialize(context_, options_.memory_kind, options_.gpu_id, options_.queue_depth);
      slots_.resize(options_.queue_depth);
      for (Slot& slot : slots_) {
        check_cuda(cudaEventCreateWithFlags(&slot.release_event, cudaEventDisableTiming),
                   "cudaEventCreate(receiver release)");
      }
      free_slots_.reserve(options_.queue_depth);
      for (std::size_t i = options_.queue_depth; i > 0; --i) {
        free_slots_.push_back(i - 1);
      }
      install_handlers();
      create_endpoint();
      log_startup();
      signal_started({});
      send_hello();

      const auto connect_deadline = Clock::now() + options_.timeout;
      std::optional<Clock::time_point> close_deadline;
      while (!failed_.load()) {
        unsigned progress = ucp_worker_progress(worker_);
        drain_releases();
        flush_credit();
        publish_completed_in_order();
        maybe_finish_eos();
        if (stop_.load()) {
          if (stats_.released.load() == stats_.delivered.load()) {
            break;
          }
          if (!close_deadline) {
            close_deadline = Clock::now() + options_.timeout;
          } else if (Clock::now() > *close_deadline) {
            fail("timed out waiting for delivered receiver slots to be released");
          }
        }
        if (!accepted_ && Clock::now() > connect_deadline) {
          fail("timed out waiting for ACCEPT");
        }
        if (progress == 0) {
          std::this_thread::yield();
        }
      }
    } catch (const std::exception& error) {
      fail(error.what());
      signal_started(error.what());
    }
    quiesce_transport();
  }

  void signal_started(std::string error) {
    std::lock_guard<std::mutex> lock(start_mutex_);
    if (started_) {
      return;
    }
    start_error_ = std::move(error);
    started_ = true;
    start_cv_.notify_all();
  }

  void log_startup() {
    unsigned major = 0, minor = 0, release = 0;
    ucp_get_version(&major, &minor, &release);
    const auto worker_attributes = query_worker(worker_);
    std::cout << "role=receiver ucx=" << major << '.' << minor << '.' << release
              << " memory_kind=" << memory_kind_name(options_.memory_kind)
              << " gpu=" << options_.gpu_id << " core=" << options_.cpu_core
              << " queue_depth=" << options_.queue_depth << " image_bytes=" << kImageBytes
              << " worker_thread_mode=" << thread_mode_name(worker_attributes.thread_mode)
              << " max_am_header=" << worker_attributes.max_am_header;
    log_ucx_environment_binding(ucx_binding_);
    std::cout << '\n';
  }

  void install_handlers() {
    ucp_am_handler_param_t handler{};
    handler.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID | UCP_AM_HANDLER_PARAM_FIELD_CB |
                         UCP_AM_HANDLER_PARAM_FIELD_ARG;
    handler.id = kControlAmId;
    handler.cb = &Impl::control_callback;
    handler.arg = this;
    check_ucs(ucp_worker_set_am_recv_handler(worker_, &handler),
              "ucp_worker_set_am_recv_handler(control)");

    handler.id = kDataAmId;
    handler.cb = &Impl::data_callback;
    check_ucs(ucp_worker_set_am_recv_handler(worker_, &handler),
              "ucp_worker_set_am_recv_handler(DATA)");
  }

  void create_endpoint() {
    server_address_ = parse_ipv4_endpoint(options_.server_endpoint);
    local_address_ = parse_ipv4_endpoint(options_.local_endpoint);
    ucp_ep_params_t params{};
    params.field_mask = UCP_EP_PARAM_FIELD_FLAGS | UCP_EP_PARAM_FIELD_SOCK_ADDR |
                        UCP_EP_PARAM_FIELD_LOCAL_SOCK_ADDR | UCP_EP_PARAM_FIELD_ERR_HANDLER |
                        UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
    params.sockaddr.addr = reinterpret_cast<const sockaddr*>(&server_address_);
    params.sockaddr.addrlen = sizeof(server_address_);
    params.local_sockaddr.addr = reinterpret_cast<const sockaddr*>(&local_address_);
    params.local_sockaddr.addrlen = sizeof(local_address_);
    params.err_mode = UCP_ERR_HANDLING_MODE_PEER;
    params.err_handler.cb = &Impl::endpoint_error_callback;
    params.err_handler.arg = this;
    check_ucs(ucp_ep_create(worker_, &params, &endpoint_), "ucp_ep_create(client)");
  }

  static void endpoint_error_callback(void* arg, ucp_ep_h, ucs_status_t status) {
    auto* self = static_cast<Impl*>(arg);
    // The producer closes after receiving EOS_ACK. At that point all admitted DATA
    // has already reached receive completion, so the peer close is expected rather
    // than an in-run delivery failure.
    if (self->eos_ack_sent_) {
      self->peer_closed_ = true;
      return;
    }
    self->fail(std::string("receiver endpoint failure: ") + ucs_status_string(status));
  }

  static ucs_status_t control_callback(void* arg, const void* header, std::size_t header_length,
                                       void*, std::size_t, const ucp_am_recv_param_t*) {
    auto* self = static_cast<Impl*>(arg);
    ControlMessage message;
    std::string error;
    if (!decode_control(header, header_length, message, error)) {
      self->fail("invalid control message: " + error);
      return UCS_OK;
    }
    self->handle_control(message);
    return UCS_OK;
  }

  static ucs_status_t data_callback(void* arg, const void* header, std::size_t header_length,
                                    void* data, std::size_t length,
                                    const ucp_am_recv_param_t* params) {
    auto* self = static_cast<Impl*>(arg);
    if ((params->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) == 0) {
      self->fail("DATA arrived without rendezvous descriptor");
      return UCS_OK;
    }
    DataHeader decoded;
    std::string error;
    if (!decode_data_header(header, header_length, decoded, error) || length != kImageBytes ||
        decoded.connection_epoch != self->connection_epoch_ || !self->accepted_) {
      self->fail("invalid DATA header or payload: " + error);
      return UCS_OK;
    }
    if (decoded.admission_ordinal != self->next_header_ordinal_ ||
        decoded.admission_ordinal >= self->options_.image_count) {
      self->fail("DATA admission ordinal is not contiguous");
      return UCS_OK;
    }
    if (self->free_slots_.empty()) {
      self->fail("DATA arrived without a credited receive slot");
      return UCS_OK;
    }
    const std::size_t slot_index = self->free_slots_.back();
    Slot& slot = self->slots_[slot_index];
    if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
      self->fail("receiver slot generation overflow");
      return UCS_OK;
    }
    if (decoded.sequence < self->next_expected_sequence_ ||
        decoded.sequence >= self->options_.image_count) {
      self->fail("DATA sequence is outside the fixed run or regressed");
      return UCS_OK;
    }

    // Only mutate the ordinal, slot pool, and sequence accounting after every
    // header and capacity invariant has passed.
    ++self->next_header_ordinal_;
    self->free_slots_.pop_back();
    ++slot.generation;
    slot.state = SlotState::receiving;
    slot.header = decoded;
    slot.preceding_gap = decoded.sequence - self->next_expected_sequence_;
    self->stats_.sequence_gaps += slot.preceding_gap;
    self->next_expected_sequence_ = decoded.sequence + 1;

    auto* operation = new AsyncOperation;
    operation->done = [self, slot_index](AsyncOperation& completed) {
      self->receive_completed(slot_index, completed.status, completed.length);
    };
    ucp_request_param_t request_params{};
    request_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA |
                                  UCP_OP_ATTR_FIELD_MEMORY_TYPE | UCP_OP_ATTR_FIELD_MEMH;
    request_params.cb.recv_am = receive_callback;
    request_params.user_data = operation;
    request_params.memory_type = self->pool_.ucs_memory_type();
    request_params.memh = self->pool_.memh();
    void* result = ucp_am_recv_data_nbx(self->worker_, data, self->pool_.ucx_slot(slot_index),
                                        kImageBytes, &request_params);
    reconcile_nbx_result(operation, result);
    return UCS_INPROGRESS;
  }

  void handle_control(const ControlMessage& message) {
    if (message.type == ControlType::accept) {
      if (accepted_ || message.stream_id != kLogicalStreamId || message.connection_epoch == 0 ||
          message.value0 != options_.image_count || message.memory_kind != options_.memory_kind ||
          message.value1 != hello_nonce_ ||
          static_cast<std::uint32_t>(message.value2) != kImageBytes ||
          (message.value2 >> 32U) != options_.queue_depth) {
        fail("ACCEPT does not match receiver configuration");
        return;
      }
      connection_epoch_ = message.connection_epoch;
      accepted_ = true;
      active_start_ = Clock::now();
      return;
    }
    if (message.type == ControlType::reject) {
      fail("producer rejected HELLO, reason=" + std::to_string(message.value0));
      return;
    }
    if (!accepted_ || message.connection_epoch != connection_epoch_) {
      fail("control message has stale connection epoch");
      return;
    }
    if (message.type == ControlType::eos) {
      if (eos_received_ || message.value0 != options_.image_count ||
          message.value1 + message.value2 != message.value0) {
        fail("EOS accounting mismatch");
        return;
      }
      if (next_expected_sequence_ > message.value0) {
        fail("EOS generated count precedes the next expected DATA sequence");
        return;
      }
      stats_.sequence_gaps += message.value0 - next_expected_sequence_;
      next_expected_sequence_ = message.value0;
      eos_generated_ = message.value0;
      eos_admitted_ = message.value1;
      eos_dropped_ = message.value2;
      eos_received_ = true;
    } else {
      fail("unexpected control message at receiver");
    }
  }

  void send_hello() {
    hello_nonce_ = random_nonzero_epoch();
    ControlMessage hello{ControlType::hello,
                         0,
                         kLogicalStreamId,
                         0,
                         options_.image_count,
                         hello_nonce_,
                         (static_cast<std::uint64_t>(options_.queue_depth) << 32U) | kImageBytes,
                         options_.memory_kind};
    send_control(hello);
  }

  void send_control(ControlMessage message) {
    if (endpoint_ == nullptr) {
      fail("receiver cannot send control without an endpoint");
      return;
    }
    auto* operation = new AsyncOperation;
    const ControlType type = message.type;
    operation->done = [this, type](AsyncOperation& completed) {
      if (type == ControlType::credit) {
        credit_in_flight_ = false;
        credit_dirty_ = credit_sent_total_ != released_total_;
      }
      // EOS_ACK is the final receiver control operation. The producer is allowed
      // to close immediately after receiving it, which can cancel an older CREDIT
      // request that is no longer needed for this fixed run. Do not suppress an
      // EOS_ACK failure itself: without the ACK the producer cannot distinguish a
      // clean fixed-run drain from a failed connection.
      const bool obsolete_credit = type == ControlType::credit && eos_ack_sent_;
      if (completed.status != UCS_OK && !obsolete_credit) {
        fail(std::string("control send failed: ") + ucs_status_string(completed.status));
      }
    };
    const auto wire = encode_control(message);
    ucp_request_param_t params{};
    params.op_attr_mask =
        UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA | UCP_OP_ATTR_FIELD_FLAGS;
    params.cb.send = send_callback;
    params.user_data = operation;
    params.flags = UCP_AM_SEND_FLAG_COPY_HEADER;
    void* result =
        ucp_am_send_nbx(endpoint_, kControlAmId, wire.data(), wire.size(), nullptr, 0, &params);
    reconcile_nbx_result(operation, result);
  }

  void receive_completed(std::size_t slot_index, ucs_status_t status, std::size_t length) {
    if (status != UCS_OK || length != kImageBytes) {
      if (slot_index < slots_.size()) {
        slots_[slot_index].state = SlotState::free;
        free_slots_.push_back(slot_index);
      }
      fail(std::string("DATA receive failed: ") + ucs_status_string(status));
      return;
    }
    completion_scoreboard_.emplace(slots_[slot_index].header.admission_ordinal, slot_index);
    const Clock::time_point now = Clock::now();
    if (stats_.delivered.load(std::memory_order_relaxed) == 0) {
      first_data_time_ = now;
    }
    last_data_time_ = now;
    ++stats_.delivered;
    stats_.bytes += kImageBytes;
  }

  void publish_completed_in_order() {
    while (true) {
      const auto found = completion_scoreboard_.find(next_delivery_ordinal_);
      if (found == completion_scoreboard_.end()) {
        return;
      }
      const std::size_t slot_index = found->second;
      completion_scoreboard_.erase(found);
      Slot& slot = slots_[slot_index];
      slot.state = SlotState::delivered;
      Completed completed{slot_index, slot.generation, pool_.device_slot(slot_index), slot.header,
                          slot.preceding_gap};
      if (!completed_.try_push(std::move(completed))) {
        fail("completed-message queue overflow");
        return;
      }
      ++next_delivery_ordinal_;
    }
  }

  void drain_releases() {
    ReleaseRequest request;
    while (releases_.try_pop(request)) {
      if (request.slot >= slots_.size() || request.generation == 0 ||
          slots_[request.slot].generation != request.generation ||
          slots_[request.slot].state != SlotState::delivered) {
        if (request.event != nullptr) {
          cudaEventSynchronize(request.event);
        }
        fail("invalid or duplicate receiver slot release");
        return;
      }
      if (request.event != nullptr) {
        slots_[request.slot].state = SlotState::release_pending;
        pending_cuda_releases_.push_back(request);
      } else {
        complete_release(request.slot, request.generation);
      }
    }

    for (auto it = pending_cuda_releases_.begin(); it != pending_cuda_releases_.end();) {
      const cudaError_t status = cudaEventQuery(it->event);
      if (status == cudaErrorNotReady) {
        ++it;
        continue;
      }
      if (status != cudaSuccess) {
        fail(std::string("receiver release event failed: ") + cudaGetErrorString(status));
        return;
      }
      complete_release(it->slot, it->generation);
      it = pending_cuda_releases_.erase(it);
    }
  }

  void complete_release(std::size_t slot_index, std::uint64_t generation) {
    if (slot_index >= slots_.size() || slots_[slot_index].generation != generation ||
        (slots_[slot_index].state != SlotState::delivered &&
         slots_[slot_index].state != SlotState::release_pending)) {
      fail("receiver completion attempted to release a stale slot generation");
      return;
    }
    slots_[slot_index].state = SlotState::free;
    free_slots_.push_back(slot_index);
    ++released_total_;
    ++stats_.released;
    if (endpoint_ != nullptr && accepted_ && !eos_ack_sent_ && !failed_.load()) {
      credit_dirty_ = true;
    }
  }

  void flush_credit() {
    if (!credit_dirty_ || credit_in_flight_ || endpoint_ == nullptr || !accepted_ ||
        eos_ack_sent_ || failed_.load()) {
      return;
    }
    credit_dirty_ = false;
    credit_in_flight_ = true;
    credit_sent_total_ = released_total_;
    ControlMessage credit{
        ControlType::credit, 0, kLogicalStreamId, connection_epoch_, credit_sent_total_, 0, 0,
        options_.memory_kind};
    send_control(credit);
  }

  void maybe_finish_eos() {
    if (!eos_received_ || eos_ack_sent_ || stats_.delivered.load() != eos_admitted_) {
      return;
    }
    ControlMessage ack{
        ControlType::eos_ack, 0, kLogicalStreamId, connection_epoch_, stats_.delivered.load(), 0, 0,
        options_.memory_kind};
    send_control(ack);
    eos_ack_sent_ = true;
    const Clock::time_point transfer_start = first_data_time_.value_or(active_start_);
    const Clock::time_point transfer_end = last_data_time_.value_or(transfer_start);
    stats_.active_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(transfer_end - transfer_start).count();
    eos_ready_.store(true);
  }

  void fail(std::string message) {
    bool expected = false;
    if (failed_.compare_exchange_strong(expected, true)) {
      std::lock_guard<std::mutex> lock(error_mutex_);
      error_ = std::move(message);
    }
  }

  std::string error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
  }

  void quiesce_transport() noexcept {
    const EndpointCloseResult close_result =
        close_endpoint(worker_, endpoint_, failed_.load() || peer_closed_, options_.timeout);
    if (close_result.status != UCS_OK) {
      fail(std::string("receiver endpoint close failed: ") +
           ucs_status_string(close_result.status));
    }
    if (close_result.quiesced) {
      // Forced close may complete an already-transferred receive while it
      // progresses cancellation. Publish any newly contiguous completions so
      // the consumer can still release their generation-safe tokens.
      publish_completed_in_order();
    }
    if (close_result.quiesced && worker_ != nullptr) {
      ucp_worker_destroy(worker_);
      worker_ = nullptr;
    }
    transport_quiesced_.store(close_result.quiesced, std::memory_order_release);
  }

  void drain_releases_for_close() noexcept {
    drain_releases();
    for (ReleaseRequest& request : pending_cuda_releases_) {
      const cudaError_t status = cudaEventSynchronize(request.event);
      if (status != cudaSuccess) {
        fail(std::string("receiver close release event failed: ") + cudaGetErrorString(status));
        continue;
      }
      complete_release(request.slot, request.generation);
    }
    pending_cuda_releases_.clear();
  }

  void finalize_storage() noexcept {
    std::lock_guard<std::mutex> lock(cleanup_mutex_);
    if (context_ == nullptr) {
      return;
    }
    if (!transport_quiesced_.load(std::memory_order_acquire)) {
      fail("receiver endpoint close timed out; UCX resources remain quarantined");
      return;
    }
    if (stats_.released.load(std::memory_order_acquire) !=
        stats_.delivered.load(std::memory_order_acquire)) {
      fail(
          "receiver closed with delivered messages still owned by the application; storage "
          "remains quarantined");
      return;
    }
    for (const Slot& slot : slots_) {
      if (slot.state != SlotState::free) {
        fail("receiver closed with a non-free slot; storage remains quarantined");
        return;
      }
    }
    for (Slot& slot : slots_) {
      if (slot.release_event != nullptr) {
        cudaEventDestroy(slot.release_event);
        slot.release_event = nullptr;
      }
    }
    pool_.destroy(context_);
    ucp_cleanup(context_);
    context_ = nullptr;
  }

  ReceiverOptions options_;
  UcxEnvironmentBinding ucx_binding_;
  ucp_context_h context_{nullptr};
  ucp_worker_h worker_{nullptr};
  ucp_ep_h endpoint_{nullptr};
  sockaddr_in server_address_{};
  sockaddr_in local_address_{};
  RegisteredPool pool_;
  std::vector<Slot> slots_;
  std::vector<std::size_t> free_slots_;
  std::vector<ReleaseRequest> pending_cuda_releases_;
  std::map<std::uint64_t, std::size_t> completion_scoreboard_;
  BoundedQueue<Completed> completed_;
  BoundedQueue<ReleaseRequest> releases_;
  AtomicStats stats_;
  std::thread thread_;
  std::atomic<bool> start_called_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> eos_ready_{false};
  std::atomic<bool> transport_quiesced_{false};
  mutable std::mutex error_mutex_;
  std::string error_;
  std::mutex start_mutex_;
  std::condition_variable start_cv_;
  std::mutex cleanup_mutex_;
  bool started_{false};
  std::string start_error_;
  bool accepted_{false};
  bool eos_received_{false};
  bool eos_ack_sent_{false};
  bool peer_closed_{false};
  std::uint64_t connection_epoch_{0};
  std::uint64_t hello_nonce_{0};
  std::uint64_t next_header_ordinal_{0};
  std::uint64_t next_delivery_ordinal_{0};
  std::uint64_t next_expected_sequence_{0};
  std::uint64_t released_total_{0};
  std::uint64_t credit_sent_total_{0};
  std::uint64_t eos_generated_{0};
  std::uint64_t eos_admitted_{0};
  std::uint64_t eos_dropped_{0};
  bool credit_in_flight_{false};
  bool credit_dirty_{false};
  Clock::time_point active_start_{};
  std::optional<Clock::time_point> first_data_time_;
  std::optional<Clock::time_point> last_data_time_;
};

ExternalBatchProducer::ExternalBatchProducer(ExternalBatchProducerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
ExternalBatchProducer::~ExternalBatchProducer() = default;
void ExternalBatchProducer::start() {
  impl_->start();
}
void ExternalBatchProducer::wait_for_receiver() {
  impl_->wait_for_receiver();
}
std::optional<BatchLease> ExternalBatchProducer::try_acquire() {
  std::optional<AcquiredSlot> acquired = impl_->try_acquire();
  if (!acquired) {
    return std::nullopt;
  }
  return BatchLease(acquired->ucx_data, acquired->device_data, acquired->size, acquired->slot,
                    acquired->generation);
}
void ExternalBatchProducer::submit_after(BatchLease&& lease, std::uint64_t first_sequence,
                                         std::uint32_t image_count,
                                         cudaStream_t processing_stream) {
  if (!lease) {
    throw std::invalid_argument("submit_after requires a live BatchLease");
  }
  impl_->submit_after(lease.slot_, lease.generation_, first_sequence, image_count,
                      processing_stream);
  lease.invalidate();
}
void ExternalBatchProducer::cancel(BatchLease&& lease, std::uint64_t first_sequence,
                                   std::uint32_t image_count) {
  if (!lease) {
    throw std::invalid_argument("cancel requires a live BatchLease");
  }
  impl_->cancel(lease.slot_, lease.generation_, first_sequence, image_count);
  lease.invalidate();
}
void ExternalBatchProducer::release_unused(BatchLease&& lease) {
  if (!lease) {
    throw std::invalid_argument("release_unused requires a live BatchLease");
  }
  impl_->release_unused(lease.slot_, lease.generation_);
  lease.invalidate();
}
std::optional<RetiredBatch> ExternalBatchProducer::poll_retired() {
  return impl_->poll_retired();
}
void ExternalBatchProducer::finish_input(std::uint64_t total_generated) {
  impl_->finish_input(total_generated);
}
ExternalBatchProducerStats ExternalBatchProducer::stats() const {
  return impl_->stats();
}
std::optional<std::string> ExternalBatchProducer::error() const {
  return impl_->error();
}
void ExternalBatchProducer::acknowledge_local_quiescence() {
  impl_->acknowledge_local_quiescence();
}
void ExternalBatchProducer::close() {
  impl_->close();
}

Receiver::Receiver(ReceiverOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Receiver::~Receiver() = default;
void Receiver::start() {
  impl_->start();
}
ReceiveResult Receiver::receive(std::chrono::milliseconds timeout) {
  return impl_->receive(timeout);
}
void Receiver::release(ReceivedImage image) {
  if (!image) {
    throw std::invalid_argument("release requires a live ReceivedImage");
  }
  impl_->release(image.slot_, image.generation_);
  image.invalidate();
}
void Receiver::release_after(ReceivedImage image, cudaStream_t stream) {
  if (!image) {
    throw std::invalid_argument("release_after requires a live ReceivedImage");
  }
  impl_->release_after(image.slot_, image.generation_, stream);
  image.invalidate();
}
TransportStats Receiver::stats() const {
  return impl_->stats();
}
std::optional<std::string> Receiver::error() const {
  return impl_->error_optional();
}
void Receiver::close() {
  impl_->close();
}

}  // namespace daqiri::ucx_gpu
