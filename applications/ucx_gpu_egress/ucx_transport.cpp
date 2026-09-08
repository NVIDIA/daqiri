// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "ucx_transport.h"

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
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace daqiri::ucx_gpu {

ReceivedBatch::ReceivedBatch(void* device_data, std::uint64_t first_sequence,
                             std::uint32_t image_count, std::size_t slot,
                             std::uint64_t generation) noexcept
    : device_data_(device_data),
      first_sequence_(first_sequence),
      image_count_(image_count),
      slot_(slot),
      generation_(generation) {}

ReceivedBatch::ReceivedBatch(ReceivedBatch&& other) noexcept
    : device_data_(other.device_data_),
      first_sequence_(other.first_sequence_),
      image_count_(other.image_count_),
      slot_(other.slot_),
      generation_(other.generation_) {
  other.invalidate();
}

void ReceivedBatch::invalidate() noexcept {
  device_data_ = nullptr;
  first_sequence_ = 0;
  image_count_ = 0;
  slot_ = 0;
  generation_ = 0;
}

BatchLease::BatchLease(void* device_data, std::size_t size, std::size_t slot,
                       std::uint64_t generation) noexcept
    : device_data_(device_data), size_(size), slot_(slot), generation_(generation) {}

BatchLease::BatchLease(BatchLease&& other) noexcept
    : device_data_(other.device_data_),
      size_(other.size_),
      slot_(other.slot_),
      generation_(other.generation_) {
  other.invalidate();
}

void BatchLease::invalidate() noexcept {
  device_data_ = nullptr;
  size_ = 0;
  slot_ = 0;
  generation_ = 0;
}

namespace {

using Clock = std::chrono::steady_clock;
template <typename T>
using SpscQueue = ucx_example::SpscQueue<T>;

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
  if (core >= CPU_SETSIZE) {
    throw std::runtime_error("invalid CPU core " + std::to_string(core));
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

struct AtomicStats {
  std::atomic<std::uint64_t> generated{0};
  std::atomic<std::uint64_t> admitted{0};
  std::atomic<std::uint64_t> dropped_no_credit{0};
  std::atomic<std::uint64_t> delivery_unknown{0};
  std::atomic<std::uint64_t> batches_sent{0};
  std::atomic<std::uint64_t> batches_delivered{0};
  std::atomic<std::uint64_t> batches_released{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> active_nanoseconds{0};

  TransportStats snapshot() const {
    TransportStats result;
    result.generated = generated.load();
    result.admitted = admitted.load();
    result.dropped_no_credit = dropped_no_credit.load();
    result.delivery_unknown = delivery_unknown.load();
    result.batches_sent = batches_sent.load();
    result.batches_delivered = batches_delivered.load();
    result.batches_released = batches_released.load();
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
                  std::size_t slot_bytes) {
    if (slots == 0 || slot_bytes == 0 ||
        slots > std::numeric_limits<std::size_t>::max() / slot_bytes) {
      throw std::invalid_argument("invalid slot count");
    }
    kind_ = kind;
    slot_bytes_ = slot_bytes;
    bytes_ = slots * slot_bytes_;
    struct InitializationGuard {
      RegisteredPool* pool;
      ucp_context_h context;
      ~InitializationGuard() {
        if (pool != nullptr) {
          pool->destroy(context);
        }
      }
      void release() noexcept {
        pool = nullptr;
      }
    } guard{this, context};
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
    guard.release();
  }

  void destroy(ucp_context_h context) noexcept {
    if (memh_ != nullptr) {
      const ucs_status_t status = ucp_mem_unmap(context, memh_);
      if (status != UCS_OK) {
        std::cerr << "ucp_mem_unmap failed: " << ucs_status_string(status) << '\n';
      }
      memh_ = nullptr;
    }
    void* allocation = kind_ == MemoryKind::host_pinned_mapped ? ucx_base_ : device_base_;
    if (allocation != nullptr) {
      const cudaError_t status =
          kind_ == MemoryKind::host_pinned_mapped ? cudaFreeHost(allocation) : cudaFree(allocation);
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
  using Done = void (*)(AsyncOperation&) noexcept;
  bool active{false};
  bool in_api{false};
  bool callback_seen{false};
  ucs_status_t status{UCS_INPROGRESS};
  std::size_t length{0};
  void* request{nullptr};
  void* callback_request{nullptr};
  void* owner{nullptr};
  std::size_t slot{0};
  std::uint64_t generation{0};
  ControlType control_type{ControlType::hello};
  bool counted{false};
  Done done{nullptr};
};

void begin_operation(AsyncOperation& operation, void* owner, AsyncOperation::Done done) {
  if (operation.active) {
    std::terminate();
  }
  operation = {};
  operation.active = true;
  operation.in_api = true;
  operation.owner = owner;
  operation.done = done;
}

void finalize_operation(AsyncOperation& operation) noexcept {
  if (!operation.active || operation.done == nullptr) {
    std::terminate();
  }
  void* request = operation.request != nullptr ? operation.request : operation.callback_request;
  if (request != nullptr && !UCS_PTR_IS_ERR(request)) {
    ucp_request_free(request);
  }
  operation.active = false;
  operation.done(operation);
}

void reconcile_nbx_result(AsyncOperation& operation, void* result) noexcept {
  operation.in_api = false;
  if (UCS_PTR_IS_ERR(result)) {
    operation.status = UCS_PTR_STATUS(result);
    operation.callback_seen = true;
  } else if (result == nullptr) {
    operation.status = UCS_OK;
    operation.callback_seen = true;
  } else {
    operation.request = result;
  }
  if (operation.callback_seen) {
    finalize_operation(operation);
  }
}

void send_callback(void* request, ucs_status_t status, void* user_data) {
  auto& operation = *static_cast<AsyncOperation*>(user_data);
  operation.callback_request = request;
  operation.status = status;
  operation.callback_seen = true;
  if (!operation.in_api) {
    finalize_operation(operation);
  }
}

void receive_callback(void* request, ucs_status_t status, std::size_t length, void* user_data) {
  auto& operation = *static_cast<AsyncOperation*>(user_data);
  operation.callback_request = request;
  operation.status = status;
  operation.length = length;
  operation.callback_seen = true;
  if (!operation.in_api) {
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
                                   Clock::duration timeout) noexcept {
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
    while (true) {
      if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        throw std::runtime_error(error_.empty() ? "external producer failed before ACCEPT"
                                                : error_);
      }
      if (receiver_ready_.load(std::memory_order_acquire)) {
        if (failed_.load(std::memory_order_acquire)) {
          std::lock_guard<std::mutex> lock(error_mutex_);
          throw std::runtime_error(error_.empty() ? "external producer failed during ACCEPT"
                                                  : error_);
        }
        return;
      }
      if (!running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("external producer stopped before ACCEPT");
      }
      if (Clock::now() >= deadline) {
        throw std::runtime_error("timed out waiting for receiver ACCEPT");
      }
      std::this_thread::yield();
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
    return ExternalBatchProducer::AcquiredSlot{pool_.device_slot(free.slot),
                                               detail::kExternalBatchBytes, free.slot, generation};
  }

  void submit_after(std::size_t slot, std::uint64_t generation, std::uint64_t first_sequence,
                    std::uint32_t image_count, cudaStream_t stream) {
    if (stream == nullptr) {
      throw std::invalid_argument("submit_after requires a CUDA stream");
    }
    enqueue(slot, generation, first_sequence, image_count, stream, false);
  }

  void discard(std::size_t slot, std::uint64_t generation) {
    enqueue(slot, generation, 0, 0, nullptr, true);
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
    dropped_before_submit_.store(total_generated - sequence_ledger_.submitted_images(),
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
    AsyncOperation send_operation;
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
    bool discarded{false};
  };

  void enqueue(std::size_t slot_index, std::uint64_t generation, std::uint64_t first_sequence,
               std::uint32_t image_count, cudaStream_t stream, bool discarded) {
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
    if (!discarded) {
      std::string sequence_error;
      if (!sequence_ledger_.record(first_sequence, image_count, sequence_error)) {
        throw std::invalid_argument(sequence_error);
      }
      check_cuda(cudaEventRecord(slot.ready_event, stream),
                 "cudaEventRecord(external batch ready)");
      completion_event = slot.ready_event;
    }
    slot.state.store(SlotState::ready, std::memory_order_release);
    if (!ready_batches_.try_push(
            {slot_index, generation, first_sequence, image_count, completion_event, discarded})) {
      fail("ready-batch queue overflow");
      throw std::runtime_error("ready-batch queue overflow");
    }
    if (!discarded) {
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
    } catch (const std::exception& error) {
      fail(error.what());
      signal_started(error.what());
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
    const auto attributes = query_worker(worker_);
    std::cout << "role=external-batch-producer ucx=" << major << '.' << minor << '.' << release
              << " memory_kind=" << memory_kind_name(options_.memory_kind)
              << " gpu=" << options_.gpu_id << " core=" << options_.cpu_core
              << " batch_slots=" << options_.batch_slot_count
              << " batch_bytes=" << detail::kExternalBatchBytes
              << " max_receiver_batch_slots=" << options_.max_receiver_queue_depth
              << " worker_thread_mode=" << thread_mode_name(attributes.thread_mode)
              << " max_am_header=" << attributes.max_am_header;
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
      if (active_ || message.connection_epoch != 0 || message.value0 != options_.image_count ||
          message.value2 != detail::kExternalBatchImages) {
        fail("HELLO does not match external producer configuration");
        return;
      }
      const std::uint64_t requested_depth = message.value1;
      if (requested_depth == 0 || requested_depth > options_.max_receiver_queue_depth) {
        fail("HELLO batch-slot depth exceeds external producer limit");
        return;
      }
      connection_epoch_ = random_nonzero_epoch();
      initial_credits_ = requested_depth;
      credits_ = requested_depth;
      active_ = true;
      active_start_ = Clock::now();
      note_activity();
      send_control({ControlType::accept, connection_epoch_, options_.image_count, requested_depth,
                    detail::kExternalBatchImages, 0});
      return;
    }
    if (!active_ || message.connection_epoch != connection_epoch_) {
      fail("external producer control message has a stale connection epoch");
      return;
    }
    if (message.type == ControlType::credit) {
      if (message.value0 < released_batches_total_ || message.value0 > admitted_batches_ ||
          message.value1 < released_images_total_ ||
          message.value1 > stats_.admitted.load(std::memory_order_acquire)) {
        fail("invalid cumulative CREDIT at external producer");
        return;
      }
      released_batches_total_ = message.value0;
      released_images_total_ = message.value1;
      stats_.batches_released.store(message.value0, std::memory_order_release);
      credits_ = initial_credits_ + released_batches_total_ - admitted_batches_;
      if (credits_ > initial_credits_) {
        fail("CREDIT overcommits external producer receiver slots");
      }
      note_activity();
    } else if (message.type == ControlType::eos_ack) {
      if (!eos_sent_ || message.value0 != stats_.admitted.load(std::memory_order_acquire) ||
          message.value1 != admitted_batches_ || message.value2 != admitted_batches_ ||
          message.value3 != stats_.admitted.load(std::memory_order_acquire)) {
        fail("EOS_ACK delivery accounting mismatch at external producer");
      } else {
        released_batches_total_ = message.value2;
        released_images_total_ = message.value3;
        stats_.batches_released.store(message.value2, std::memory_order_release);
        stats_.batches_delivered.store(message.value1, std::memory_order_release);
        const Clock::time_point start = first_data_time_.value_or(active_start_);
        const Clock::time_point end = last_data_time_.value_or(start);
        stats_.active_nanoseconds.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
            std::memory_order_release);
        done_ = true;
      }
    } else {
      fail("unexpected control message at external producer");
    }
  }

  static void control_done(AsyncOperation& operation) noexcept {
    auto* self = static_cast<Impl*>(operation.owner);
    if (operation.status != UCS_OK) {
      self->fail(std::string("external producer control send failed: ") +
                 ucs_status_string(operation.status));
      return;
    }
    if (operation.control_type == ControlType::accept &&
        !self->failed_.load(std::memory_order_acquire)) {
      self->receiver_ready_.store(true, std::memory_order_release);
    }
  }

  void send_control(ControlMessage message) {
    if (endpoint_ == nullptr || control_operation_.active) {
      fail("external producer cannot serialize a control message");
      return;
    }
    begin_operation(control_operation_, this, &Impl::control_done);
    control_operation_.control_type = message.type;
    const auto wire = encode_control(message);
    ucp_request_param_t params{};
    params.op_attr_mask =
        UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA | UCP_OP_ATTR_FIELD_FLAGS;
    params.cb.send = send_callback;
    params.user_data = &control_operation_;
    params.flags = UCP_AM_SEND_FLAG_COPY_HEADER;
    void* result =
        ucp_am_send_nbx(endpoint_, kControlAmId, wire.data(), wire.size(), nullptr, 0, &params);
    reconcile_nbx_result(control_operation_, result);
  }

  void process_ready_batches() {
    if (!active_) {
      return;
    }
    while (true) {
      if (!current_ready_) {
        ReadyBatch ready;
        if (!ready_batches_.try_pop(ready)) {
          return;
        }
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
        current_ready_ = ready;
      }

      const ReadyBatch ready = *current_ready_;
      if (!ready.discarded && options_.wait_for_credit && credits_ == 0) {
        return;
      }
      if (ready.processing_done != nullptr) {
        const cudaError_t status = cudaEventQuery(ready.processing_done);
        if (status == cudaErrorNotReady) {
          return;
        }
        if (status != cudaSuccess) {
          fail(std::string("external producer processing event failed: ") +
               cudaGetErrorString(status));
          return;
        }
      }
      current_ready_.reset();
      if (ready.discarded) {
        queue_retirement(ready.slot, ready.generation);
      } else if (credits_ == 0) {
        stats_.dropped_no_credit.fetch_add(ready.image_count, std::memory_order_relaxed);
        queue_retirement(ready.slot, ready.generation);
      } else {
        send_batch(ready);
      }
      if (failed_.load(std::memory_order_acquire)) {
        return;
      }
    }
  }

  void send_batch(const ReadyBatch& ready) {
    Slot& slot = slots_[ready.slot];
    slot.state.store(SlotState::sending, std::memory_order_release);
    --credits_;

    DataHeader header;
    header.connection_epoch = connection_epoch_;
    header.first_sequence = ready.first_sequence;
    header.image_count = ready.image_count;
    header.batch_ordinal = admitted_batches_;
    const auto wire = encode_data_header(header);

    begin_operation(slot.send_operation, this, &Impl::data_send_done);
    slot.send_operation.slot = ready.slot;
    slot.send_operation.generation = ready.generation;
    ucp_request_param_t params{};
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA |
                          UCP_OP_ATTR_FIELD_FLAGS | UCP_OP_ATTR_FIELD_MEMORY_TYPE |
                          UCP_OP_ATTR_FIELD_MEMH;
    params.cb.send = send_callback;
    params.user_data = &slot.send_operation;
    params.flags = UCP_AM_SEND_FLAG_RNDV | UCP_AM_SEND_FLAG_COPY_HEADER;
    params.memory_type = pool_.ucs_memory_type();
    params.memh = pool_.memh();
    const std::size_t bytes = static_cast<std::size_t>(ready.image_count) * kImageBytes;
    void* result = ucp_am_send_nbx(endpoint_, kDataAmId, wire.data(), wire.size(),
                                   pool_.ucx_slot(ready.slot), bytes, &params);
    if (!UCS_PTR_IS_ERR(result)) {
      const std::uint64_t previous = stats_.admitted.fetch_add(ready.image_count);
      if (previous == 0) {
        first_data_time_ = Clock::now();
      }
      stats_.bytes.fetch_add(bytes, std::memory_order_relaxed);
      ++admitted_batches_;
      slot.send_operation.counted = true;
    }
    reconcile_nbx_result(slot.send_operation, result);
  }

  static void data_send_done(AsyncOperation& operation) noexcept {
    static_cast<Impl*>(operation.owner)
        ->send_completed(operation.slot, operation.generation, operation.status, operation.counted);
  }

  void send_completed(std::size_t slot_index, std::uint64_t generation, ucs_status_t status,
                      bool counted) noexcept {
    if (slot_index >= options_.batch_slot_count ||
        slots_[slot_index].generation.load(std::memory_order_acquire) != generation ||
        slots_[slot_index].state.load(std::memory_order_acquire) != SlotState::sending) {
      fail("external DATA completion violated slot ownership");
      return;
    }
    if (!counted) {
      ++credits_;
    }
    if (status != UCS_OK) {
      fail(std::string("external DATA send failed (delivery unknown): ") +
           ucs_status_string(status));
      return;
    }
    stats_.batches_sent.fetch_add(1, std::memory_order_relaxed);
    last_data_time_ = Clock::now();
    note_activity();
    queue_retirement(slot_index, generation);
  }

  void queue_retirement(std::size_t slot_index, std::uint64_t generation) noexcept {
    Slot& slot = slots_[slot_index];
    const SlotState state = slot.state.load(std::memory_order_acquire);
    if (slot.generation.load(std::memory_order_acquire) != generation ||
        (state != SlotState::sending && state != SlotState::waiting_cuda)) {
      fail("external batch retirement violated slot ownership");
      return;
    }
    slot.state.store(SlotState::retirement_pending, std::memory_order_release);
  }

  void flush_retirements() {
    for (std::size_t index = 0; index < options_.batch_slot_count; ++index) {
      Slot& slot = slots_[index];
      if (slot.state.load(std::memory_order_acquire) != SlotState::retirement_pending) {
        continue;
      }
      const std::uint64_t generation = slot.generation.load(std::memory_order_acquire);
      slot.state.store(SlotState::free, std::memory_order_release);
      if (!free_slots_.try_push({index, generation})) {
        slot.state.store(SlotState::retirement_pending, std::memory_order_release);
        return;
      }
      retired_batches_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void maybe_send_eos() {
    if (eos_sent_ || !active_ || !finish_requested_.load(std::memory_order_acquire) ||
        !ready_batches_.empty() || current_ready_ || control_operation_.active) {
      return;
    }
    for (std::size_t index = 0; index < options_.batch_slot_count; ++index) {
      if (slots_[index].state.load(std::memory_order_acquire) != SlotState::free) {
        return;
      }
    }
    const std::uint64_t generated = stats_.generated.load(std::memory_order_acquire);
    const std::uint64_t admitted = stats_.admitted.load(std::memory_order_acquire);
    if (admitted > generated ||
        stats_.batches_sent.load(std::memory_order_acquire) != admitted_batches_) {
      fail("external producer EOS accounting invariant failed");
      return;
    }
    send_control({ControlType::eos, connection_epoch_, generated, admitted, admitted_batches_,
                  stats_.dropped_no_credit.load(std::memory_order_acquire)});
    eos_sent_ = true;
    eos_deadline_ = Clock::now() + options_.timeout;
  }

  void fail(std::string message) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (failed_.load(std::memory_order_relaxed)) {
      return;
    }
    const std::uint64_t admitted = stats_.admitted.load(std::memory_order_acquire);
    stats_.delivery_unknown.store(
        admitted >= released_images_total_ ? admitted - released_images_total_ : 0,
        std::memory_order_relaxed);
    error_ = std::move(message);
    failed_.store(true, std::memory_order_release);
  }

  void note_activity() noexcept {
    activity_deadline_ = Clock::now() + options_.timeout;
  }

  void quiesce_transport() noexcept {
    const EndpointCloseResult result = close_endpoint(
        worker_, endpoint_, failed_.load(std::memory_order_acquire), options_.timeout);
    if (result.status != UCS_OK) {
      fail(std::string("external producer endpoint close failed: ") +
           ucs_status_string(result.status));
    }
    if (listener_ != nullptr) {
      ucp_listener_destroy(listener_);
      listener_ = nullptr;
    }
    if (result.quiesced && worker_ != nullptr) {
      ucp_worker_destroy(worker_);
      worker_ = nullptr;
    }
    transport_quiesced_.store(result.quiesced, std::memory_order_release);
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
  std::optional<ReadyBatch> current_ready_;
  detail::SubmittedSequenceLedger sequence_ledger_;
  AtomicStats stats_;
  AsyncOperation control_operation_;
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
  std::uint64_t credits_{0};
  std::uint64_t admitted_batches_{0};
  std::uint64_t released_batches_total_{0};
  std::uint64_t released_images_total_{0};
  Clock::time_point connection_deadline_{};
  Clock::time_point activity_deadline_{};
  Clock::time_point eos_deadline_{};
  Clock::time_point active_start_{};
  std::optional<Clock::time_point> first_data_time_;
  std::optional<Clock::time_point> last_data_time_;
};

class Receiver::Impl {
 public:
  explicit Impl(ReceiverOptions options) : options_(std::move(options)) {
    if (options_.server_endpoint.empty() || options_.local_endpoint.empty() ||
        options_.image_count == 0 || options_.queue_depth == 0) {
      throw std::invalid_argument("receiver endpoints, image count, and batch depth are required");
    }
  }

  ~Impl() {
    close();
  }

  void start() {
    if (start_called_) {
      throw std::logic_error("Receiver::start called more than once");
    }
    start_called_ = true;
    try {
      pin_current_thread(options_.cpu_core);
      check_cuda(cudaSetDevice(options_.gpu_id), "receiver cudaSetDevice");
      ucx_binding_ = require_ucx_environment_binding();
      context_ = make_context("daqiri-ucx-batch-receiver");
      worker_ = make_worker(context_);
      pool_.initialize(context_, options_.memory_kind, options_.gpu_id, options_.queue_depth,
                       detail::kExternalBatchBytes);
      slots_.resize(options_.queue_depth);
      scoreboard_.resize(options_.queue_depth);
      completed_slots_.resize(options_.queue_depth);
      free_slots_.reserve(options_.queue_depth);
      for (std::size_t index = options_.queue_depth; index > 0; --index) {
        Slot& slot = slots_[index - 1];
        check_cuda(cudaEventCreateWithFlags(&slot.release_event, cudaEventDisableTiming),
                   "cudaEventCreate(receiver release)");
        free_slots_.push_back(index - 1);
      }
      install_handlers();
      create_endpoint();
      log_startup();
      running_ = true;
      connect_deadline_ = Clock::now() + options_.timeout;
      send_hello();
    } catch (const std::exception& error) {
      fail(error.what());
      quiesce_transport();
      finalize_storage();
      throw;
    }
  }

  ReceiveResult receive(std::chrono::milliseconds timeout) {
    if (!running_) {
      return {ReceiveStatus::failed, std::nullopt, "receiver is not running"};
    }
    const Clock::time_point deadline = Clock::now() + timeout;
    while (true) {
      std::size_t slot_index = 0;
      if (pop_completed(slot_index)) {
        Slot& slot = slots_[slot_index];
        ReceivedBatch batch(pool_.device_slot(slot_index), slot.header.first_sequence,
                            slot.header.image_count, slot_index, slot.generation);
        return {ReceiveStatus::batch, std::move(batch), {}};
      }
      if (failed_) {
        return {ReceiveStatus::failed, std::nullopt, error_};
      }
      if (eos_ready_) {
        return {ReceiveStatus::end_of_stream, std::nullopt, {}};
      }
      const unsigned progress = progress_once();
      if (!accepted_ && Clock::now() > connect_deadline_) {
        fail("timed out waiting for ACCEPT");
      }
      if (Clock::now() >= deadline) {
        return {ReceiveStatus::timeout, std::nullopt, {}};
      }
      if (progress == 0) {
        std::this_thread::yield();
      }
    }
  }

  void release(std::size_t slot, std::uint64_t generation) {
    validate_release(slot, generation);
    complete_release(slot, generation);
    progress_once();
  }

  void release_after(std::size_t slot, std::uint64_t generation, cudaStream_t stream) {
    if (stream == nullptr) {
      throw std::invalid_argument("release_after requires a CUDA stream");
    }
    validate_release(slot, generation);
    Slot& target = slots_[slot];
    check_cuda(cudaEventRecord(target.release_event, stream), "cudaEventRecord(receiver release)");
    target.state = SlotState::release_pending;
    progress_once();
  }

  TransportStats stats() const {
    return stats_;
  }

  std::optional<std::string> error_optional() const {
    return failed_ ? std::optional<std::string>(error_) : std::nullopt;
  }

  void close() noexcept {
    if (!start_called_ || closed_) {
      return;
    }
    closed_ = true;
    drain_releases_for_close();
    quiesce_transport();
    finalize_storage();
    running_ = false;
  }

 private:
  enum class SlotState : std::uint8_t { free, receiving, completed, delivered, release_pending };

  struct Slot {
    SlotState state{SlotState::free};
    std::uint64_t generation{0};
    DataHeader header{};
    std::uint64_t preceding_gap{0};
    cudaEvent_t release_event{nullptr};
    AsyncOperation receive_operation;
  };

  struct ScoreEntry {
    bool ready{false};
    std::uint64_t ordinal{0};
    std::size_t slot{0};
  };

  unsigned progress_once() noexcept {
    if (worker_ == nullptr || failed_) {
      return 0;
    }
    const unsigned progress = ucp_worker_progress(worker_);
    poll_release_events();
    publish_completed_in_order();
    flush_credit();
    maybe_finish_eos();
    return progress;
  }

  void log_startup() {
    unsigned major = 0, minor = 0, release = 0;
    ucp_get_version(&major, &minor, &release);
    const auto attributes = query_worker(worker_);
    std::cout << "role=receiver ucx=" << major << '.' << minor << '.' << release
              << " memory_kind=" << memory_kind_name(options_.memory_kind)
              << " gpu=" << options_.gpu_id << " core=" << options_.cpu_core
              << " batch_slots=" << options_.queue_depth
              << " batch_bytes=" << detail::kExternalBatchBytes
              << " worker_thread_mode=" << thread_mode_name(attributes.thread_mode)
              << " max_am_header=" << attributes.max_am_header;
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

  static void endpoint_error_callback(void* argument, ucp_ep_h, ucs_status_t status) {
    auto* self = static_cast<Impl*>(argument);
    if (self->eos_ready_) {
      self->peer_closed_ = true;
      return;
    }
    self->fail(std::string("receiver endpoint failure: ") + ucs_status_string(status));
  }

  static ucs_status_t control_callback(void* argument, const void* header,
                                       std::size_t header_length, void*, std::size_t,
                                       const ucp_am_recv_param_t*) {
    auto* self = static_cast<Impl*>(argument);
    ControlMessage message;
    std::string error;
    if (!decode_control(header, header_length, message, error)) {
      self->fail("invalid control message: " + error);
      return UCS_OK;
    }
    self->handle_control(message);
    return UCS_OK;
  }

  static ucs_status_t data_callback(void* argument, const void* header, std::size_t header_length,
                                    void* data, std::size_t length,
                                    const ucp_am_recv_param_t* params) {
    auto* self = static_cast<Impl*>(argument);
    if ((params->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) == 0) {
      self->fail("DATA arrived without a rendezvous descriptor");
      return UCS_OK;
    }
    DataHeader decoded;
    std::string error;
    if (!decode_data_header(header, header_length, decoded, error)) {
      self->fail("invalid DATA header: " + error);
      return UCS_OK;
    }
    const std::size_t expected_length = static_cast<std::size_t>(decoded.image_count) * kImageBytes;
    if (!self->accepted_ || decoded.connection_epoch != self->connection_epoch_ ||
        length != expected_length) {
      self->fail("DATA does not match the accepted connection or batch length");
      return UCS_OK;
    }
    if (decoded.batch_ordinal != self->next_header_ordinal_) {
      self->fail("DATA batch ordinal is not contiguous");
      return UCS_OK;
    }
    if (decoded.first_sequence < self->next_expected_sequence_ ||
        decoded.first_sequence > self->options_.image_count ||
        decoded.image_count > self->options_.image_count - decoded.first_sequence) {
      self->fail("DATA sequence range is outside the fixed run or regressed");
      return UCS_OK;
    }
    if (self->free_slots_.empty()) {
      self->fail("DATA arrived without a credited batch slot");
      return UCS_OK;
    }

    const std::size_t slot_index = self->free_slots_.back();
    Slot& slot = self->slots_[slot_index];
    if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
      self->fail("receiver slot generation overflow");
      return UCS_OK;
    }
    ++self->next_header_ordinal_;
    self->free_slots_.pop_back();
    ++slot.generation;
    slot.state = SlotState::receiving;
    slot.header = decoded;
    slot.preceding_gap = decoded.first_sequence - self->next_expected_sequence_;
    self->stats_.sequence_gaps += slot.preceding_gap;
    self->next_expected_sequence_ = decoded.first_sequence + decoded.image_count;

    begin_operation(slot.receive_operation, self, &Impl::data_receive_done);
    slot.receive_operation.slot = slot_index;
    slot.receive_operation.generation = slot.generation;
    slot.receive_operation.length = expected_length;
    ucp_request_param_t request_params{};
    request_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA |
                                  UCP_OP_ATTR_FIELD_MEMORY_TYPE | UCP_OP_ATTR_FIELD_MEMH |
                                  UCP_OP_ATTR_FIELD_RECV_INFO;
    request_params.cb.recv_am = receive_callback;
    request_params.user_data = &slot.receive_operation;
    request_params.memory_type = self->pool_.ucs_memory_type();
    request_params.memh = self->pool_.memh();
    request_params.recv_info.length = &slot.receive_operation.length;
    void* result = ucp_am_recv_data_nbx(self->worker_, data, self->pool_.ucx_slot(slot_index),
                                        expected_length, &request_params);
    reconcile_nbx_result(slot.receive_operation, result);
    return UCS_INPROGRESS;
  }

  void handle_control(const ControlMessage& message) {
    if (message.type == ControlType::accept) {
      if (accepted_ || message.connection_epoch == 0 || message.value0 != options_.image_count ||
          message.value1 != options_.queue_depth ||
          message.value2 != detail::kExternalBatchImages) {
        fail("ACCEPT does not match receiver configuration");
        return;
      }
      connection_epoch_ = message.connection_epoch;
      accepted_ = true;
      active_start_ = Clock::now();
      return;
    }
    if (!accepted_ || message.connection_epoch != connection_epoch_) {
      fail("control message has a stale connection epoch");
      return;
    }
    if (message.type != ControlType::eos || eos_received_ ||
        message.value0 != options_.image_count || message.value1 > message.value0 ||
        message.value2 > message.value1 || message.value3 > message.value0 - message.value1 ||
        delivered_images_ > message.value1 || next_header_ordinal_ > message.value2) {
      fail("unexpected control message or EOS accounting mismatch");
      return;
    }
    if (next_expected_sequence_ > message.value0) {
      fail("EOS generated count precedes the next expected DATA sequence");
      return;
    }
    stats_.sequence_gaps += message.value0 - next_expected_sequence_;
    next_expected_sequence_ = message.value0;
    eos_admitted_images_ = message.value1;
    eos_admitted_batches_ = message.value2;
    stats_.generated = message.value0;
    stats_.admitted = message.value1;
    stats_.dropped_no_credit = message.value3;
    stats_.batches_sent = message.value2;
    eos_received_ = true;
  }

  void send_hello() {
    send_control({ControlType::hello, 0, options_.image_count, options_.queue_depth,
                  detail::kExternalBatchImages, 0});
  }

  static void control_done(AsyncOperation& operation) noexcept {
    auto* self = static_cast<Impl*>(operation.owner);
    if (operation.status != UCS_OK) {
      self->fail(std::string("receiver control send failed: ") +
                 ucs_status_string(operation.status));
      return;
    }
    if (operation.control_type == ControlType::credit) {
      self->credit_dirty_ = self->credit_sent_batches_ != self->released_batches_total_ ||
                            self->credit_sent_images_ != self->released_images_total_;
    } else if (operation.control_type == ControlType::eos_ack) {
      const Clock::time_point start = self->first_data_time_.value_or(self->active_start_);
      const Clock::time_point end = self->last_data_time_.value_or(start);
      self->stats_.active_nanoseconds =
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
      self->eos_ready_ = true;
    }
  }

  void send_control(ControlMessage message) noexcept {
    if (endpoint_ == nullptr || control_operation_.active) {
      fail("receiver cannot serialize a control message");
      return;
    }
    begin_operation(control_operation_, this, &Impl::control_done);
    control_operation_.control_type = message.type;
    const auto wire = encode_control(message);
    ucp_request_param_t params{};
    params.op_attr_mask =
        UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA | UCP_OP_ATTR_FIELD_FLAGS;
    params.cb.send = send_callback;
    params.user_data = &control_operation_;
    params.flags = UCP_AM_SEND_FLAG_COPY_HEADER;
    void* result =
        ucp_am_send_nbx(endpoint_, kControlAmId, wire.data(), wire.size(), nullptr, 0, &params);
    reconcile_nbx_result(control_operation_, result);
  }

  static void data_receive_done(AsyncOperation& operation) noexcept {
    static_cast<Impl*>(operation.owner)
        ->receive_completed(operation.slot, operation.generation, operation.status,
                            operation.length);
  }

  void receive_completed(std::size_t slot_index, std::uint64_t generation, ucs_status_t status,
                         std::size_t length) noexcept {
    if (slot_index >= slots_.size() || slots_[slot_index].generation != generation ||
        slots_[slot_index].state != SlotState::receiving) {
      fail("DATA receive completion violated slot ownership");
      return;
    }
    Slot& slot = slots_[slot_index];
    const std::size_t expected = static_cast<std::size_t>(slot.header.image_count) * kImageBytes;
    if (status != UCS_OK || length != expected) {
      slot.state = SlotState::free;
      free_slots_.push_back(slot_index);
      fail(std::string("DATA receive failed: ") + ucs_status_string(status) +
           ", length=" + std::to_string(length) + ", expected=" + std::to_string(expected));
      return;
    }
    ScoreEntry& entry = scoreboard_[slot.header.batch_ordinal % scoreboard_.size()];
    if (entry.ready) {
      fail("DATA completion scoreboard overflow");
      return;
    }
    slot.state = SlotState::completed;
    entry = {true, slot.header.batch_ordinal, slot_index};
    const Clock::time_point now = Clock::now();
    if (stats_.batches_delivered == 0) {
      first_data_time_ = now;
    }
    last_data_time_ = now;
    ++stats_.batches_delivered;
    delivered_images_ += slot.header.image_count;
    stats_.bytes += expected;
  }

  void publish_completed_in_order() noexcept {
    while (!scoreboard_.empty()) {
      ScoreEntry& entry = scoreboard_[next_delivery_ordinal_ % scoreboard_.size()];
      if (!entry.ready || entry.ordinal != next_delivery_ordinal_) {
        return;
      }
      if (completed_count_ == completed_slots_.size()) {
        fail("completed batch ring overflow");
        return;
      }
      Slot& slot = slots_[entry.slot];
      if (slot.state != SlotState::completed) {
        fail("completion scoreboard referred to a non-completed slot");
        return;
      }
      slot.state = SlotState::delivered;
      completed_slots_[completed_head_] = entry.slot;
      completed_head_ = (completed_head_ + 1) % completed_slots_.size();
      ++completed_count_;
      entry.ready = false;
      ++next_delivery_ordinal_;
    }
  }

  bool pop_completed(std::size_t& slot) noexcept {
    if (completed_count_ == 0) {
      return false;
    }
    slot = completed_slots_[completed_tail_];
    completed_tail_ = (completed_tail_ + 1) % completed_slots_.size();
    --completed_count_;
    return true;
  }

  void validate_release(std::size_t slot, std::uint64_t generation) {
    if (slot >= slots_.size() || generation == 0 || slots_[slot].generation != generation ||
        slots_[slot].state != SlotState::delivered) {
      throw std::logic_error("invalid or duplicate receiver batch release");
    }
  }

  void poll_release_events() noexcept {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      Slot& slot = slots_[index];
      if (slot.state != SlotState::release_pending) {
        continue;
      }
      const cudaError_t status = cudaEventQuery(slot.release_event);
      if (status == cudaErrorNotReady) {
        continue;
      }
      if (status != cudaSuccess) {
        fail(std::string("receiver release event failed: ") + cudaGetErrorString(status));
        return;
      }
      complete_release(index, slot.generation);
    }
  }

  void complete_release(std::size_t slot_index, std::uint64_t generation) noexcept {
    Slot& slot = slots_[slot_index];
    if (slot.generation != generation ||
        (slot.state != SlotState::delivered && slot.state != SlotState::release_pending)) {
      fail("receiver completion attempted to release a stale batch generation");
      return;
    }
    const std::uint32_t image_count = slot.header.image_count;
    slot.state = SlotState::free;
    free_slots_.push_back(slot_index);
    ++released_batches_total_;
    released_images_total_ += image_count;
    ++stats_.batches_released;
    if (endpoint_ != nullptr && accepted_ && !eos_ack_queued_ && !failed_) {
      credit_dirty_ = true;
    }
  }

  void flush_credit() noexcept {
    if (!credit_dirty_ || control_operation_.active || endpoint_ == nullptr || !accepted_ ||
        eos_ack_queued_ || failed_) {
      return;
    }
    credit_dirty_ = false;
    credit_sent_batches_ = released_batches_total_;
    credit_sent_images_ = released_images_total_;
    send_control(
        {ControlType::credit, connection_epoch_, credit_sent_batches_, credit_sent_images_, 0, 0});
  }

  void maybe_finish_eos() noexcept {
    if (!eos_received_ || eos_ack_queued_ || control_operation_.active ||
        delivered_images_ != eos_admitted_images_ ||
        stats_.batches_delivered != eos_admitted_batches_ ||
        next_header_ordinal_ != eos_admitted_batches_ ||
        released_images_total_ != eos_admitted_images_ ||
        stats_.batches_released != eos_admitted_batches_) {
      return;
    }
    credit_dirty_ = false;
    eos_ack_queued_ = true;
    send_control({ControlType::eos_ack, connection_epoch_, delivered_images_,
                  stats_.batches_delivered, released_batches_total_, released_images_total_});
  }

  void fail(std::string message) noexcept {
    if (!failed_) {
      failed_ = true;
      error_ = std::move(message);
    }
  }

  void quiesce_transport() noexcept {
    if (worker_ == nullptr) {
      transport_quiesced_ = true;
      return;
    }
    const EndpointCloseResult result =
        close_endpoint(worker_, endpoint_, failed_ || peer_closed_, options_.timeout);
    if (result.status != UCS_OK) {
      fail(std::string("receiver endpoint close failed: ") + ucs_status_string(result.status));
    }
    if (result.quiesced) {
      publish_completed_in_order();
      ucp_worker_destroy(worker_);
      worker_ = nullptr;
    }
    transport_quiesced_ = result.quiesced;
  }

  void drain_releases_for_close() noexcept {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      Slot& slot = slots_[index];
      if (slot.state != SlotState::release_pending) {
        continue;
      }
      const cudaError_t status = cudaEventSynchronize(slot.release_event);
      if (status != cudaSuccess) {
        fail(std::string("receiver close release event failed: ") + cudaGetErrorString(status));
        continue;
      }
      complete_release(index, slot.generation);
    }
  }

  void finalize_storage() noexcept {
    if (context_ == nullptr) {
      return;
    }
    if (!transport_quiesced_) {
      fail("receiver endpoint close timed out; UCX resources remain quarantined");
      return;
    }
    if (stats_.batches_released != stats_.batches_delivered) {
      fail("receiver closed with delivered batches still owned; storage remains quarantined");
      return;
    }
    for (const Slot& slot : slots_) {
      if (slot.state != SlotState::free) {
        fail("receiver closed with a non-free batch slot; storage remains quarantined");
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
  std::vector<ScoreEntry> scoreboard_;
  std::vector<std::size_t> completed_slots_;
  AsyncOperation control_operation_;
  TransportStats stats_;
  std::string error_;
  bool start_called_{false};
  bool running_{false};
  bool closed_{false};
  bool failed_{false};
  bool accepted_{false};
  bool eos_received_{false};
  bool eos_ack_queued_{false};
  bool eos_ready_{false};
  bool peer_closed_{false};
  bool transport_quiesced_{false};
  bool credit_dirty_{false};
  std::size_t completed_head_{0};
  std::size_t completed_tail_{0};
  std::size_t completed_count_{0};
  std::uint64_t connection_epoch_{0};
  std::uint64_t next_header_ordinal_{0};
  std::uint64_t next_delivery_ordinal_{0};
  std::uint64_t next_expected_sequence_{0};
  std::uint64_t delivered_images_{0};
  std::uint64_t released_batches_total_{0};
  std::uint64_t released_images_total_{0};
  std::uint64_t credit_sent_batches_{0};
  std::uint64_t credit_sent_images_{0};
  std::uint64_t eos_admitted_images_{0};
  std::uint64_t eos_admitted_batches_{0};
  Clock::time_point connect_deadline_{};
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
  return BatchLease(acquired->device_data, acquired->size, acquired->slot, acquired->generation);
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
void ExternalBatchProducer::discard(BatchLease&& lease) {
  if (!lease) {
    throw std::invalid_argument("discard requires a live BatchLease");
  }
  impl_->discard(lease.slot_, lease.generation_);
  lease.invalidate();
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
void Receiver::release(ReceivedBatch batch) {
  if (!batch) {
    throw std::invalid_argument("release requires a live ReceivedBatch");
  }
  impl_->release(batch.slot_, batch.generation_);
  batch.invalidate();
}
void Receiver::release_after(ReceivedBatch batch, cudaStream_t stream) {
  if (!batch) {
    throw std::invalid_argument("release_after requires a live ReceivedBatch");
  }
  impl_->release_after(batch.slot_, batch.generation_, stream);
  batch.invalidate();
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
