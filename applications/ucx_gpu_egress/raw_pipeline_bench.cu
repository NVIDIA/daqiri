// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "cuda_image.h"
#include "pipeline_spsc_queue.h"
#include "processing/scale_offset.h"
#include "raw/dqri_header.h"
#include "raw/fragment_placement.h"
#include "raw/image_assembler.h"
#include "ucx_transport.h"

#include <daqiri/daqiri.h>

#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace raw = daqiri::ucx_example;
namespace gpu = daqiri::ucx_gpu;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kHeaderStatusCount = 22;

enum class PipelineStage { kAssemble, kProcess, kEgress };

enum class SlotState : std::uint8_t {
  kFree,
  kPlacing,
  kProcessQueued,
  kProcessing,
  kSinkQueued,
  kValidating,
  kRecycleQueued,
  kRejectPending,
  kEgressQueued,
};

struct SlotTicket {
  std::uint32_t index = 0;
  std::uint64_t generation = 0;
  bool rejected = false;
  bool unused = false;
};

struct PipelineConfig {
  std::string interface_name{"rx_port"};
  int queue_id = 0;
  int gpu_id = 0;
  int rx_core = 19;
  int processing_core = 17;
  int sink_core = 16;
  int ucx_core = 16;
  int daqiri_poller_core = -1;
  std::size_t ring_slots = 16;
  std::size_t descriptor_pool_depth = 32;
  std::size_t max_burst_packets = 1024;
  std::uint64_t source_epoch = 0;
  std::uint64_t assembly_timeout_us = 2000;
  float scale = 1.0F;
  float offset = 0.0F;
  bool poison_rejected_slots = true;
  std::string listen_endpoint{"0.0.0.0:13341"};
  gpu::MemoryKind memory_kind = gpu::MemoryKind::host_pinned_mapped;
  std::size_t batch_slot_count = 16;
  std::size_t max_receiver_depth = 64;
  int timeout_seconds = 30;
  int overall_timeout_seconds = 300;
  int idle_timeout_seconds = 10;
};

struct CommandLine {
  std::string config_path;
  PipelineStage stage = PipelineStage::kAssemble;
  int seconds = 10;
  bool seconds_provided = false;
  std::optional<std::uint64_t> batches;
};

struct ImageSlot {
  void* host_data = nullptr;
  void* device_data = nullptr;
  std::uint64_t generation = 1;
  cudaEvent_t placement_done = nullptr;
  cudaEvent_t processing_done = nullptr;
  cudaEvent_t validation_done = nullptr;
  std::optional<gpu::BatchLease> external_lease;
  raw::BatchDescriptor batch{};
  std::atomic<SlotState> state{SlotState::kFree};
};

struct BurstSubmission {
  raw::FragmentPlacement* host_descriptors = nullptr;
  raw::FragmentPlacement* device_descriptors = nullptr;
  cudaEvent_t reads_done = nullptr;
  daqiri::BurstParams* burst = nullptr;
  bool active = false;
};

struct RxCounters {
  std::uint64_t bursts = 0;
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::uint64_t burst_releases = 0;
  std::uint64_t immediate_burst_releases = 0;
  std::uint64_t deferred_burst_releases = 0;
  std::uint64_t completed_batches = 0;
  std::uint64_t completed_images = 0;
  std::uint64_t rejected_batches = 0;
  std::uint64_t descriptor_pool_stalls = 0;
  std::uint64_t process_queue_full_waits = 0;
  std::uint64_t process_queue_max_depth = 0;
  std::uint64_t pending_burst_max = 0;
  std::uint64_t slot_occupancy_max = 0;
  std::uint64_t alias_cache_hits = 0;
  std::uint64_t alias_cache_misses = 0;
  std::array<std::uint64_t, kHeaderStatusCount> header_status{};
  std::optional<Clock::time_point> first_packet_time;
  std::optional<Clock::time_point> last_packet_time;
};

struct ProcessingCounters {
  std::uint64_t batches = 0;
  std::uint64_t event_polls_not_ready = 0;
  std::uint64_t sink_queue_full_waits = 0;
  std::uint64_t sink_queue_max_depth = 0;
};

struct SinkCounters {
  std::uint64_t batches = 0;
  std::uint64_t images = 0;
  std::uint64_t event_polls_not_ready = 0;
  std::uint64_t recycle_queue_full_waits = 0;
  std::uint64_t recycle_queue_max_depth = 0;
};

struct PipelineRuntime {
  PipelineRuntime(PipelineConfig config_value, PipelineStage stage_value,
                  std::uint64_t expected_batches_value)
      : config(std::move(config_value)),
        stage(stage_value),
        expected_batches(expected_batches_value),
        slot_count(stage == PipelineStage::kEgress ? config.batch_slot_count : config.ring_slots),
        rx_to_processing(slot_count),
        processing_to_sink(slot_count),
        sink_to_rx(slot_count),
        slots(new ImageSlot[slot_count]),
        submissions(config.descriptor_pool_depth) {}

  PipelineConfig config;
  PipelineStage stage;
  std::uint64_t expected_batches = 0;
  std::size_t slot_count = 0;
  raw::SpscQueue<SlotTicket> rx_to_processing;
  raw::SpscQueue<SlotTicket> processing_to_sink;
  raw::SpscQueue<SlotTicket> sink_to_rx;
  std::unique_ptr<ImageSlot[]> slots;
  std::vector<BurstSubmission> submissions;
  gpu::ValidationResult* validation_result_device = nullptr;
  gpu::ExternalBatchProducer* external_producer = nullptr;

  std::atomic<bool> stop_requested{false};
  std::atomic<bool> rx_input_done{false};
  std::atomic<bool> rx_ready{false};
  std::atomic<bool> processing_ready{false};
  std::atomic<bool> sink_ready{false};
  std::atomic<bool> processing_done{false};
  std::atomic<bool> sink_done{false};
  std::atomic<bool> fatal{false};
  std::atomic<std::uint64_t> last_input_progress_ns{0};
  std::mutex fatal_mutex;
  std::string fatal_message;

  RxCounters rx_counters;
  ProcessingCounters processing_counters;
  SinkCounters sink_counters;
};

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

const char* stage_name(PipelineStage stage) {
  switch (stage) {
    case PipelineStage::kAssemble:
      return "assemble";
    case PipelineStage::kProcess:
      return "process";
    case PipelineStage::kEgress:
      return "egress";
  }
  return "unknown";
}

void check_cuda(cudaError_t status, const char* operation) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
  }
}

void set_fatal(PipelineRuntime& runtime, const std::string& message) {
  bool expected = false;
  if (runtime.fatal.compare_exchange_strong(expected, true)) {
    std::lock_guard<std::mutex> lock(runtime.fatal_mutex);
    runtime.fatal_message = message;
  }
  runtime.stop_requested.store(true, std::memory_order_release);
}

void pin_current_thread(int core, const char* owner) {
  if (core < 0 || core >= CPU_SETSIZE) {
    throw std::runtime_error(std::string(owner) + " has invalid cpu_core " + std::to_string(core));
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  const int status = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (status != 0) {
    throw std::runtime_error(std::string("pthread_setaffinity_np(") + owner + ", core " +
                             std::to_string(core) + ") failed: " + std::strerror(status));
  }
}

void transition_slot(ImageSlot& slot, SlotState expected, SlotState desired,
                     const char* operation) {
  if (!slot.state.compare_exchange_strong(expected, desired)) {
    throw std::runtime_error(std::string("slot ownership violation in ") + operation +
                             ": observed state " +
                             std::to_string(static_cast<unsigned int>(expected)));
  }
}

void update_max(std::uint64_t& maximum, std::size_t current) {
  maximum = std::max(maximum, static_cast<std::uint64_t>(current));
}

CommandLine parse_command_line(int argc, char** argv) {
  if (argc < 2) {
    throw std::runtime_error(std::string("usage: ") + argv[0] +
                             " CONFIG --stage assemble|process|egress "
                             "[--seconds N | --batches N]");
  }
  CommandLine result;
  result.config_path = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--stage" && index + 1 < argc) {
      const std::string value = argv[++index];
      if (value == "assemble") {
        result.stage = PipelineStage::kAssemble;
      } else if (value == "process") {
        result.stage = PipelineStage::kProcess;
      } else if (value == "egress") {
        result.stage = PipelineStage::kEgress;
      } else {
        throw std::runtime_error("--stage must be assemble, process, or egress");
      }
    } else if (option == "--seconds" && index + 1 < argc) {
      result.seconds = std::stoi(argv[++index]);
      result.seconds_provided = true;
    } else if (option == "--batches" && index + 1 < argc) {
      result.batches = std::stoull(argv[++index]);
    } else {
      throw std::runtime_error("unknown or incomplete option: " + option);
    }
  }
  if (result.seconds <= 0) {
    throw std::runtime_error("--seconds must be positive");
  }
  if (result.stage == PipelineStage::kEgress) {
    if (!result.batches || *result.batches == 0) {
      throw std::runtime_error("--stage egress requires a positive --batches N fixed run");
    }
    if (result.seconds_provided) {
      throw std::runtime_error("--stage egress uses configured timeouts, not --seconds");
    }
  } else if (result.batches) {
    throw std::runtime_error("--batches is supported only by --stage egress");
  }
  return result;
}

int find_daqiri_poller_core(const YAML::Node& root, const std::string& interface_name,
                            int queue_id) {
  const YAML::Node interfaces = root["daqiri"]["cfg"]["interfaces"];
  if (!interfaces || !interfaces.IsSequence()) {
    return -1;
  }
  for (const YAML::Node& interface : interfaces) {
    if (interface["name"].as<std::string>("") != interface_name) {
      continue;
    }
    const YAML::Node queues = interface["rx"]["queues"];
    if (!queues || !queues.IsSequence()) {
      return -1;
    }
    for (const YAML::Node& queue : queues) {
      if (queue["id"].as<int>(-1) == queue_id) {
        return queue["cpu_core"].as<int>(-1);
      }
    }
  }
  return -1;
}

std::size_t find_daqiri_batch_size(const YAML::Node& root, const std::string& interface_name,
                                   int queue_id) {
  const YAML::Node interfaces = root["daqiri"]["cfg"]["interfaces"];
  if (!interfaces || !interfaces.IsSequence()) {
    return 0;
  }
  for (const YAML::Node& interface : interfaces) {
    if (interface["name"].as<std::string>("") != interface_name) {
      continue;
    }
    const YAML::Node queues = interface["rx"]["queues"];
    if (!queues || !queues.IsSequence()) {
      return 0;
    }
    for (const YAML::Node& queue : queues) {
      if (queue["id"].as<int>(-1) == queue_id) {
        return queue["batch_size"].as<std::size_t>(0);
      }
    }
  }
  return 0;
}

PipelineConfig load_pipeline_config(const YAML::Node& root, PipelineStage stage) {
  const YAML::Node node = root["ucx_gpu_raw_processor"];
  if (!node || !node.IsMap()) {
    throw std::runtime_error("config requires a top-level ucx_gpu_raw_processor map");
  }
  PipelineConfig result;
  result.interface_name = node["interface_name"].as<std::string>(result.interface_name);
  result.queue_id = node["queue_id"].as<int>(result.queue_id);
  result.gpu_id = node["gpu_id"].as<int>(result.gpu_id);
  result.rx_core = node["rx_core"].as<int>(result.rx_core);
  result.processing_core = node["processing_core"].as<int>(result.processing_core);
  result.sink_core = node["sink_core"].as<int>(result.sink_core);
  result.ucx_core = node["ucx_core"].as<int>(result.ucx_core);
  result.ring_slots = node["ring_slots"].as<std::size_t>(result.ring_slots);
  result.descriptor_pool_depth =
      node["descriptor_pool_depth"].as<std::size_t>(result.descriptor_pool_depth);
  result.max_burst_packets = node["max_burst_packets"].as<std::size_t>(result.max_burst_packets);
  result.source_epoch = node["source_epoch"].as<std::uint64_t>(result.source_epoch);
  result.assembly_timeout_us =
      node["assembly_timeout_us"].as<std::uint64_t>(result.assembly_timeout_us);
  result.scale = node["scale"].as<float>(result.scale);
  result.offset = node["offset"].as<float>(result.offset);
  result.poison_rejected_slots =
      node["poison_rejected_slots"].as<bool>(result.poison_rejected_slots);
  result.listen_endpoint = node["listen_endpoint"].as<std::string>(result.listen_endpoint);
  const std::string memory_kind =
      node["memory_kind"].as<std::string>(gpu::memory_kind_name(result.memory_kind));
  if (!gpu::parse_memory_kind(memory_kind, result.memory_kind)) {
    throw std::runtime_error("memory_kind must be host_pinned_mapped or cuda_device");
  }
  result.batch_slot_count = node["batch_slot_count"].as<std::size_t>(result.batch_slot_count);
  result.max_receiver_depth = node["max_receiver_depth"].as<std::size_t>(result.max_receiver_depth);
  result.timeout_seconds = node["timeout_seconds"].as<int>(result.timeout_seconds);
  result.overall_timeout_seconds =
      node["overall_timeout_seconds"].as<int>(result.overall_timeout_seconds);
  result.idle_timeout_seconds = node["idle_timeout_seconds"].as<int>(result.idle_timeout_seconds);
  result.daqiri_poller_core = find_daqiri_poller_core(root, result.interface_name, result.queue_id);

  const std::size_t daqiri_batch_size =
      find_daqiri_batch_size(root, result.interface_name, result.queue_id);
  if (result.interface_name.empty() || result.queue_id < 0 || result.gpu_id < 0 ||
      result.rx_core < 0 || result.processing_core < 0 ||
      (stage != PipelineStage::kEgress && result.sink_core < 0) ||
      (stage == PipelineStage::kEgress &&
       (result.ucx_core < 0 || result.listen_endpoint.empty() || result.batch_slot_count < 2 ||
        result.max_receiver_depth == 0 || result.timeout_seconds <= 0 ||
        result.overall_timeout_seconds <= 0 || result.idle_timeout_seconds <= 0)) ||
      (stage != PipelineStage::kEgress && result.ring_slots < 2) ||
      result.descriptor_pool_depth == 0 || result.max_burst_packets == 0 ||
      result.source_epoch == 0 || result.assembly_timeout_us == 0 || !std::isfinite(result.scale) ||
      !std::isfinite(result.offset)) {
    throw std::runtime_error("ucx_gpu_raw_processor contains an invalid or missing value");
  }
  const std::array<int, 4> cores{
      result.daqiri_poller_core, result.rx_core, result.processing_core,
      stage == PipelineStage::kEgress ? result.ucx_core : result.sink_core};
  for (std::size_t left = 0; left < cores.size(); ++left) {
    if (cores[left] < 0) {
      throw std::runtime_error("DAQIRI RX queue and all stage cpu cores must be configured");
    }
    for (std::size_t right = left + 1; right < cores.size(); ++right) {
      if (cores[left] == cores[right]) {
        throw std::runtime_error("DAQIRI, RX, processing, and final-stage cores must not overlap");
      }
    }
  }
  if (daqiri_batch_size == 0 || daqiri_batch_size > result.max_burst_packets) {
    throw std::runtime_error("max_burst_packets must cover the configured DAQIRI RX batch_size");
  }
  return result;
}

void initialize_cuda_resources(PipelineRuntime& runtime) {
  check_cuda(cudaSetDevice(runtime.config.gpu_id), "cudaSetDevice(main)");
  check_cuda(cudaFree(nullptr), "initialize CUDA context");
  for (std::size_t index = 0; index < runtime.slot_count; ++index) {
    ImageSlot& slot = runtime.slots[index];
    if (runtime.stage != PipelineStage::kEgress) {
      check_cuda(cudaHostAlloc(&slot.host_data, raw::kBatchBytes,
                               cudaHostAllocMapped | cudaHostAllocPortable),
                 "cudaHostAlloc(image slot)");
      check_cuda(cudaHostGetDevicePointer(&slot.device_data, slot.host_data, 0),
                 "cudaHostGetDevicePointer(image slot)");
    }
    check_cuda(cudaEventCreateWithFlags(&slot.placement_done, cudaEventDisableTiming),
               "cudaEventCreate(placement_done)");
    if (runtime.stage != PipelineStage::kEgress) {
      check_cuda(cudaEventCreateWithFlags(&slot.processing_done, cudaEventDisableTiming),
                 "cudaEventCreate(processing_done)");
      check_cuda(cudaEventCreateWithFlags(&slot.validation_done, cudaEventDisableTiming),
                 "cudaEventCreate(validation_done)");
      check_cuda(cudaMemset(slot.device_data, 0xa5, raw::kBatchBytes), "initialize image slot");
    }
  }
  for (BurstSubmission& submission : runtime.submissions) {
    check_cuda(cudaHostAlloc(reinterpret_cast<void**>(&submission.host_descriptors),
                             runtime.config.max_burst_packets * sizeof(raw::FragmentPlacement),
                             cudaHostAllocDefault),
               "cudaHostAlloc(fragment descriptors)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&submission.device_descriptors),
                          runtime.config.max_burst_packets * sizeof(raw::FragmentPlacement)),
               "cudaMalloc(fragment descriptors)");
    check_cuda(cudaEventCreateWithFlags(&submission.reads_done, cudaEventDisableTiming),
               "cudaEventCreate(fragment reads_done)");
  }
  if (runtime.stage != PipelineStage::kEgress) {
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&runtime.validation_result_device),
                          sizeof(gpu::ValidationResult)),
               "cudaMalloc(validation result)");
    const gpu::ValidationResult initial{0, std::numeric_limits<unsigned long long>::max(), 0, 0};
    check_cuda(cudaMemcpy(runtime.validation_result_device, &initial, sizeof(initial),
                          cudaMemcpyHostToDevice),
               "initialize validation result");
  }
}

void destroy_cuda_resources(PipelineRuntime& runtime) noexcept {
  for (BurstSubmission& submission : runtime.submissions) {
    if (submission.reads_done != nullptr) {
      cudaEventDestroy(submission.reads_done);
    }
    if (submission.device_descriptors != nullptr) {
      cudaFree(submission.device_descriptors);
    }
    if (submission.host_descriptors != nullptr) {
      cudaFreeHost(submission.host_descriptors);
    }
  }
  if (runtime.validation_result_device != nullptr) {
    cudaFree(runtime.validation_result_device);
  }
  for (std::size_t index = 0; index < runtime.slot_count; ++index) {
    ImageSlot& slot = runtime.slots[index];
    if (slot.validation_done != nullptr) {
      cudaEventDestroy(slot.validation_done);
    }
    if (slot.processing_done != nullptr) {
      cudaEventDestroy(slot.processing_done);
    }
    if (slot.placement_done != nullptr) {
      cudaEventDestroy(slot.placement_done);
    }
    if (runtime.stage != PipelineStage::kEgress && slot.host_data != nullptr) {
      cudaFreeHost(slot.host_data);
    }
  }
}

class RxOwner {
 public:
  RxOwner(PipelineRuntime& runtime, int port_id, int queue_id)
      : runtime_(runtime),
        port_id_(port_id),
        queue_id_(queue_id),
        assembler_({runtime.config.source_epoch, runtime.config.assembly_timeout_us * 1000ULL}) {
    fragment_inputs_.reserve(runtime.config.max_burst_packets);
    reservations_.reserve(runtime.slot_count);
    if (runtime.stage != PipelineStage::kEgress) {
      for (std::size_t index = 0; index < runtime.slot_count; ++index) {
        free_slots_.push_back(static_cast<std::uint32_t>(index));
      }
    }
    for (std::size_t index = 0; index < runtime.submissions.size(); ++index) {
      free_submissions_.push_back(index);
    }
    alias_cache_.reserve(runtime.config.max_burst_packets * 4);
  }

  void run() {
    cudaStream_t placement_stream = nullptr;
    daqiri::BurstParams* current_burst = nullptr;
    try {
      pin_current_thread(runtime_.config.rx_core, "RX/placement owner");
      check_cuda(cudaSetDevice(runtime_.config.gpu_id), "cudaSetDevice(RX)");
      check_cuda(cudaStreamCreateWithFlags(&placement_stream, cudaStreamNonBlocking),
                 "cudaStreamCreate(placement)");
      drain_recycled_slots();
      if (free_slots_.empty()) {
        throw std::runtime_error("RX/placement owner started without an available image slot");
      }
      runtime_.rx_ready.store(true, std::memory_order_release);

      while (!runtime_.stop_requested.load(std::memory_order_acquire)) {
        drain_recycled_slots();
        drain_rejected_slots();
        drain_burst_submissions();
        flush_timeout(placement_stream, now_ns());
        if (runtime_.stage == PipelineStage::kEgress) {
          const std::uint64_t boundary = terminal_batches();
          if (boundary > runtime_.expected_batches) {
            throw std::runtime_error("raw source batch ID exceeds the fixed run boundary");
          }
          if (boundary == runtime_.expected_batches) {
            break;
          }
        }

        if (free_submissions_.empty()) {
          ++runtime_.rx_counters.descriptor_pool_stalls;
          std::this_thread::yield();
          continue;
        }

        const daqiri::Status status = daqiri::get_rx_burst(&current_burst, port_id_, queue_id_);
        if (status != daqiri::Status::SUCCESS || current_burst == nullptr) {
          current_burst = nullptr;
          std::this_thread::yield();
          continue;
        }
        process_burst(current_burst, placement_stream);
        current_burst = nullptr;
      }

      flush_active_for_shutdown(placement_stream);
      if (runtime_.stage == PipelineStage::kEgress) {
        publish_unused_external_slots();
      }
      runtime_.rx_input_done.store(true, std::memory_order_release);
      while (
          !runtime_.fatal.load(std::memory_order_acquire) &&
          (!(runtime_.stage == PipelineStage::kEgress
                 ? runtime_.processing_done.load(std::memory_order_acquire) && all_slots_terminal()
                 : runtime_.sink_done.load(std::memory_order_acquire) &&
                       free_slots_.size() == runtime_.slot_count) ||
           !all_submissions_free() || !rejected_pending_.empty())) {
        drain_recycled_slots();
        drain_rejected_slots();
        drain_burst_submissions();
        std::this_thread::yield();
      }
      check_cuda(cudaStreamSynchronize(placement_stream), "drain placement stream");
      drain_rejected_slots();
      drain_burst_submissions();
      if (!runtime_.fatal.load(std::memory_order_acquire) &&
          runtime_.rx_counters.burst_releases != runtime_.rx_counters.bursts) {
        throw std::runtime_error("RX burst release accounting mismatch");
      }
    } catch (const std::exception& error) {
      set_fatal(runtime_, std::string("RX/placement thread: ") + error.what());
      if (placement_stream != nullptr) {
        cudaStreamSynchronize(placement_stream);
      }
      if (current_burst != nullptr) {
        release_burst(current_burst, false);
      }
      release_all_active_bursts();
      runtime_.rx_input_done.store(true, std::memory_order_release);
    }
    if (placement_stream != nullptr) {
      cudaStreamDestroy(placement_stream);
    }
  }

  const raw::AssemblerCounters& assembler_counters() const noexcept {
    return assembler_.counters();
  }

 private:
  const std::uint8_t* device_alias_for(const std::uint8_t* host_pointer) {
    const auto found = alias_cache_.find(host_pointer);
    if (found != alias_cache_.end()) {
      ++runtime_.rx_counters.alias_cache_hits;
      return found->second;
    }
    void* alias = nullptr;
    check_cuda(cudaHostGetDevicePointer(&alias, const_cast<std::uint8_t*>(host_pointer), 0),
               "cudaHostGetDevicePointer(DAQIRI packet)");
    const auto* typed_alias = static_cast<const std::uint8_t*>(alias);
    alias_cache_.emplace(host_pointer, typed_alias);
    ++runtime_.rx_counters.alias_cache_misses;
    return typed_alias;
  }

  void release_burst(daqiri::BurstParams* burst, bool deferred) {
    daqiri::free_all_packets_and_burst_rx(burst);
    ++runtime_.rx_counters.burst_releases;
    if (deferred) {
      ++runtime_.rx_counters.deferred_burst_releases;
    } else {
      ++runtime_.rx_counters.immediate_burst_releases;
    }
  }

  void release_all_active_bursts() noexcept {
    for (BurstSubmission& submission : runtime_.submissions) {
      if (submission.active && submission.burst != nullptr) {
        daqiri::free_all_packets_and_burst_rx(submission.burst);
        submission.burst = nullptr;
        submission.active = false;
        ++runtime_.rx_counters.burst_releases;
        ++runtime_.rx_counters.deferred_burst_releases;
      }
    }
  }

  void drain_burst_submissions() {
    for (std::size_t index = 0; index < runtime_.submissions.size(); ++index) {
      BurstSubmission& submission = runtime_.submissions[index];
      if (!submission.active) {
        continue;
      }
      const cudaError_t status = cudaEventQuery(submission.reads_done);
      if (status == cudaErrorNotReady) {
        continue;
      }
      check_cuda(status, "cudaEventQuery(fragment reads_done)");
      release_burst(submission.burst, true);
      submission.burst = nullptr;
      submission.active = false;
      free_submissions_.push_back(index);
    }
  }

  bool all_submissions_free() const noexcept {
    return free_submissions_.size() == runtime_.submissions.size();
  }

  void drain_recycled_slots() {
    if (runtime_.stage == PipelineStage::kEgress) {
      drain_external_slots();
      return;
    }
    SlotTicket ticket;
    while (runtime_.sink_to_rx.try_pop(ticket)) {
      ImageSlot& slot = checked_slot(ticket, "sink-to-RX recycle");
      transition_slot(slot, SlotState::kRecycleQueued, SlotState::kFree, "sink-to-RX recycle");
      ++slot.generation;
      free_slots_.push_back(ticket.index);
    }
  }

  void drain_external_slots() {
    if (runtime_.external_producer == nullptr) {
      throw std::runtime_error("egress stage has no external producer");
    }
    while (std::optional<gpu::RetiredBatch> retired = runtime_.external_producer->poll_retired()) {
      if (retired->slot >= runtime_.slot_count) {
        throw std::runtime_error("UCX retired an out-of-range slot");
      }
      ImageSlot& slot = runtime_.slots[retired->slot];
      if (slot.generation != retired->generation) {
        throw std::runtime_error("UCX retired a stale slot generation");
      }
      transition_slot(slot, SlotState::kEgressQueued, SlotState::kFree, "UCX-to-RX retirement");
    }
    if (std::optional<std::string> error = runtime_.external_producer->error()) {
      throw std::runtime_error("UCX producer failed: " + *error);
    }
    // During fixed-run drain, retired leases must stay producer-free so EOS can
    // observe an entirely free internal pool. RX no longer needs reservations.
    if (runtime_.rx_input_done.load(std::memory_order_acquire)) {
      return;
    }
    while (std::optional<gpu::BatchLease> lease = runtime_.external_producer->try_acquire()) {
      const std::size_t slot_index = lease->slot();
      if (slot_index >= runtime_.slot_count || lease->size() != raw::kBatchBytes) {
        throw std::runtime_error("UCX producer returned an invalid external batch slot");
      }
      ImageSlot& slot = runtime_.slots[slot_index];
      if (slot.state.load(std::memory_order_acquire) != SlotState::kFree ||
          slot.external_lease.has_value()) {
        throw std::runtime_error("UCX producer returned a locally owned slot");
      }
      slot.host_data = lease->ucx_data();
      slot.device_data = lease->device_data();
      slot.generation = lease->generation();
      slot.external_lease.emplace(std::move(*lease));
      free_slots_.push_back(static_cast<std::uint32_t>(slot_index));
    }
  }

  bool all_slots_terminal() const noexcept {
    for (std::size_t index = 0; index < runtime_.slot_count; ++index) {
      if (runtime_.slots[index].state.load(std::memory_order_acquire) != SlotState::kFree) {
        return false;
      }
    }
    return true;
  }

  std::uint64_t terminal_batches() const noexcept {
    return assembler_.accounted_batch_boundary();
  }

  void publish_unused_external_slots() {
    while (!free_slots_.empty()) {
      const std::uint32_t index = free_slots_.front();
      free_slots_.pop_front();
      ImageSlot& slot = runtime_.slots[index];
      transition_slot(slot, SlotState::kFree, SlotState::kProcessQueued,
                      "publish unused external slot");
      SlotTicket ticket{index, slot.generation, false, true};
      while (!runtime_.rx_to_processing.try_push(ticket)) {
        if (runtime_.fatal.load(std::memory_order_acquire)) {
          throw std::runtime_error("fatal error while releasing unused external slot");
        }
        drain_burst_submissions();
        std::this_thread::yield();
      }
    }
  }

  void drain_rejected_slots() {
    auto current = rejected_pending_.begin();
    while (current != rejected_pending_.end()) {
      ImageSlot& slot = checked_slot(*current, "rejected-slot completion");
      const cudaError_t status = cudaEventQuery(slot.placement_done);
      if (status == cudaErrorNotReady) {
        ++current;
        continue;
      }
      check_cuda(status, "cudaEventQuery(rejected slot)");
      transition_slot(slot, SlotState::kRejectPending, SlotState::kFree,
                      "rejected-slot completion");
      ++slot.generation;
      free_slots_.push_back(current->index);
      current = rejected_pending_.erase(current);
    }
  }

  ImageSlot& checked_slot(const SlotTicket& ticket, const char* operation) {
    if (ticket.index >= runtime_.slot_count) {
      throw std::runtime_error(std::string(operation) + " has out-of-range slot index");
    }
    ImageSlot& slot = runtime_.slots[ticket.index];
    if (slot.generation != ticket.generation) {
      throw std::runtime_error(std::string(operation) + " has stale slot generation");
    }
    return slot;
  }

  void enqueue_processing_ticket(const SlotTicket& ticket) {
    while (!runtime_.rx_to_processing.try_push(ticket)) {
      if (runtime_.fatal.load(std::memory_order_acquire)) {
        throw std::runtime_error("fatal error while waiting for RX-to-processing queue");
      }
      ++runtime_.rx_counters.process_queue_full_waits;
      drain_recycled_slots();
      drain_rejected_slots();
      drain_burst_submissions();
      std::this_thread::yield();
    }
    update_max(runtime_.rx_counters.process_queue_max_depth, runtime_.rx_to_processing.depth());
  }

  void schedule_rejection(const raw::RejectedBatch& rejected, cudaStream_t stream,
                          std::vector<SlotTicket>* ordered_egress_tickets = nullptr) {
    const SlotTicket ticket{rejected.batch.slot.slot_index, rejected.batch.slot.generation};
    ImageSlot& slot = checked_slot(ticket, "batch rejection");
    transition_slot(slot, SlotState::kPlacing,
                    runtime_.stage == PipelineStage::kEgress ? SlotState::kProcessQueued
                                                             : SlotState::kRejectPending,
                    "batch rejection");
    if (runtime_.config.poison_rejected_slots) {
      check_cuda(raw::poison_batch_slot_async(slot.device_data, 0xa5, stream),
                 "poison_batch_slot_async");
    }
    check_cuda(cudaEventRecord(slot.placement_done, stream), "record rejected-slot event");
    if (runtime_.stage == PipelineStage::kEgress) {
      slot.batch = rejected.batch;
      SlotTicket rejected_ticket{ticket.index, ticket.generation, true, false};
      if (ordered_egress_tickets != nullptr) {
        ordered_egress_tickets->push_back(rejected_ticket);
      } else {
        enqueue_processing_ticket(rejected_ticket);
      }
    } else {
      rejected_pending_.push_back(ticket);
    }
    ++runtime_.rx_counters.rejected_batches;
  }

  void publish_completed(const raw::BatchDescriptor& batch,
                         std::vector<SlotTicket>* ordered_egress_tickets = nullptr) {
    const SlotTicket ticket{batch.slot.slot_index, batch.slot.generation};
    ImageSlot& slot = checked_slot(ticket, "publish completed batch");
    slot.batch = batch;
    transition_slot(slot, SlotState::kPlacing, SlotState::kProcessQueued,
                    "publish completed batch");
    if (ordered_egress_tickets != nullptr) {
      ordered_egress_tickets->push_back(ticket);
    } else {
      enqueue_processing_ticket(ticket);
    }
    ++runtime_.rx_counters.completed_batches;
    runtime_.rx_counters.completed_images += raw::kImagesPerBatch;
  }

  void make_reservations() {
    reservations_.clear();
    for (const std::uint32_t index : free_slots_) {
      ImageSlot& slot = runtime_.slots[index];
      reservations_.push_back({index, slot.generation, slot.device_data});
    }
  }

  void consume_reserved_slots(std::size_t count) {
    if (count > free_slots_.size()) {
      throw std::runtime_error("assembler consumed more slots than RX supplied");
    }
    for (std::size_t index = 0; index < count; ++index) {
      const std::uint32_t slot_index = free_slots_.front();
      free_slots_.pop_front();
      transition_slot(runtime_.slots[slot_index], SlotState::kFree, SlotState::kPlacing,
                      "reserve image slot");
    }
    update_max(runtime_.rx_counters.slot_occupancy_max, runtime_.slot_count - free_slots_.size());
  }

  void process_burst(daqiri::BurstParams* burst, cudaStream_t stream) {
    ++runtime_.rx_counters.bursts;
    const std::int64_t signed_packet_count = daqiri::get_num_packets(burst);
    if (signed_packet_count <= 0) {
      release_burst(burst, false);
      return;
    }
    const std::size_t packet_count = static_cast<std::size_t>(signed_packet_count);
    if (packet_count > runtime_.config.max_burst_packets) {
      throw std::runtime_error("DAQIRI burst exceeds max_burst_packets");
    }

    const Clock::time_point packet_time = Clock::now();
    if (!runtime_.rx_counters.first_packet_time) {
      runtime_.rx_counters.first_packet_time = packet_time;
    }
    runtime_.rx_counters.last_packet_time = packet_time;
    runtime_.last_input_progress_ns.store(now_ns(), std::memory_order_release);
    fragment_inputs_.clear();
    fragment_inputs_.resize(packet_count);
    for (std::size_t packet_index = 0; packet_index < packet_count; ++packet_index) {
      ++runtime_.rx_counters.packets;
      const std::uint32_t packet_length =
          daqiri::get_packet_length(burst, static_cast<int>(packet_index));
      runtime_.rx_counters.bytes += packet_length;
      auto* host_frame = static_cast<const std::uint8_t*>(
          daqiri::get_segment_packet_ptr(burst, 0, static_cast<int>(packet_index)));

      raw::ParsedPacket parsed;
      raw::HeaderStatus status = raw::HeaderStatus::kTruncated;
      if (host_frame != nullptr) {
        status = raw::parse_dqri_frame(host_frame, packet_length, &parsed,
                                       raw::HeaderValidation{true, runtime_.config.source_epoch});
      }
      const std::size_t status_index = static_cast<std::size_t>(status);
      if (status_index < runtime_.rx_counters.header_status.size()) {
        ++runtime_.rx_counters.header_status[status_index];
      }
      raw::FragmentInput& input = fragment_inputs_[packet_index];
      input.header_status = status;
      if (status == raw::HeaderStatus::kOk) {
        input.header = parsed.header;
        const std::uint8_t* device_frame = device_alias_for(host_frame);
        input.payload_device = device_frame + raw::kPayloadOffsetBytes;
      }
    }

    make_reservations();
    raw::BurstAssemblyResult result = assembler_.consume_burst(
        next_burst_lease_id_++, now_ns(), fragment_inputs_.data(), fragment_inputs_.size(),
        reservations_.data(), reservations_.size());
    consume_reserved_slots(result.supplied_slots_consumed);

    std::size_t descriptor_count = 0;
    for (const raw::PlacementOperation& operation : result.placement_operations) {
      descriptor_count += operation.fragments.size();
    }
    if (descriptor_count > runtime_.config.max_burst_packets) {
      throw std::runtime_error("placement descriptor count exceeds configured capacity");
    }

    std::size_t submission_index = std::numeric_limits<std::size_t>::max();
    BurstSubmission* submission = nullptr;
    std::vector<SlotTicket> ordered_egress_tickets;
    if (descriptor_count != 0) {
      if (free_submissions_.empty()) {
        throw std::runtime_error("descriptor pool invariant violated");
      }
      submission_index = free_submissions_.front();
      free_submissions_.pop_front();
      submission = &runtime_.submissions[submission_index];

      std::size_t offset = 0;
      for (const raw::PlacementOperation& operation : result.placement_operations) {
        std::copy(operation.fragments.begin(), operation.fragments.end(),
                  submission->host_descriptors + offset);
        offset += operation.fragments.size();
      }
      check_cuda(cudaMemcpyAsync(submission->device_descriptors, submission->host_descriptors,
                                 descriptor_count * sizeof(raw::FragmentPlacement),
                                 cudaMemcpyHostToDevice, stream),
                 "copy placement descriptors");

      offset = 0;
      for (const raw::PlacementOperation& operation : result.placement_operations) {
        check_cuda(raw::place_fragments_async(operation.destination.destination_device,
                                              submission->device_descriptors + offset,
                                              operation.fragments.size(), stream),
                   "place_fragments_async");
        offset += operation.fragments.size();
        if (operation.completed_batch) {
          ImageSlot& slot = runtime_.slots[operation.completed_batch->slot.slot_index];
          check_cuda(cudaEventRecord(slot.placement_done, stream),
                     "record completed placement event");
          publish_completed(*operation.completed_batch, runtime_.stage == PipelineStage::kEgress
                                                            ? &ordered_egress_tickets
                                                            : nullptr);
        }
      }
    }

    for (const raw::RejectedBatch& rejected : result.rejected_batches) {
      schedule_rejection(
          rejected, stream,
          runtime_.stage == PipelineStage::kEgress ? &ordered_egress_tickets : nullptr);
    }
    if (!ordered_egress_tickets.empty()) {
      std::sort(ordered_egress_tickets.begin(), ordered_egress_tickets.end(),
                [this](const SlotTicket& left, const SlotTicket& right) {
                  return runtime_.slots[left.index].batch.unwrapped_batch_id <
                         runtime_.slots[right.index].batch.unwrapped_batch_id;
                });
      for (const SlotTicket& ticket : ordered_egress_tickets) {
        enqueue_processing_ticket(ticket);
      }
    }

    if (descriptor_count == 0) {
      release_burst(burst, false);
      return;
    }
    check_cuda(cudaEventRecord(submission->reads_done, stream), "record burst read event");
    submission->burst = burst;
    submission->active = true;
    const std::size_t active = runtime_.submissions.size() - free_submissions_.size();
    update_max(runtime_.rx_counters.pending_burst_max, active);
  }

  void flush_timeout(cudaStream_t stream, std::uint64_t timestamp_ns) {
    if (std::optional<raw::RejectedBatch> rejected = assembler_.flush_timeout(timestamp_ns)) {
      schedule_rejection(*rejected, stream);
    }
  }

  void flush_active_for_shutdown(cudaStream_t stream) {
    if (!assembler_.has_active_batch()) {
      return;
    }
    const std::uint64_t timeout_ns = runtime_.config.assembly_timeout_us * 1000ULL;
    const std::uint64_t timestamp =
        now_ns() > std::numeric_limits<std::uint64_t>::max() - timeout_ns
            ? std::numeric_limits<std::uint64_t>::max()
            : now_ns() + timeout_ns;
    flush_timeout(stream, timestamp);
  }

  PipelineRuntime& runtime_;
  int port_id_;
  int queue_id_;
  raw::ImageBatchAssembler assembler_;
  std::vector<raw::FragmentInput> fragment_inputs_;
  std::vector<raw::SlotReservation> reservations_;
  std::deque<std::uint32_t> free_slots_;
  std::deque<std::size_t> free_submissions_;
  std::vector<SlotTicket> rejected_pending_;
  std::unordered_map<const std::uint8_t*, const std::uint8_t*> alias_cache_;
  std::uint64_t next_burst_lease_id_ = 1;
};

cudaError_t query_event(cudaEvent_t event, std::uint64_t& not_ready_counter) {
  const cudaError_t status = cudaEventQuery(event);
  if (status == cudaErrorNotReady) {
    ++not_ready_counter;
  }
  return status;
}

void processing_worker(PipelineRuntime& runtime) {
  cudaStream_t stream = nullptr;
  try {
    pin_current_thread(runtime.config.processing_core, "CUDA processing owner");
    check_cuda(cudaSetDevice(runtime.config.gpu_id), "cudaSetDevice(processing)");
    check_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
               "cudaStreamCreate(processing)");
    runtime.processing_ready.store(true, std::memory_order_release);
    std::optional<SlotTicket> pending;
    while (!runtime.fatal.load(std::memory_order_acquire) &&
           (!runtime.rx_input_done.load(std::memory_order_acquire) || pending ||
            !runtime.rx_to_processing.empty())) {
      if (!pending) {
        SlotTicket ticket;
        if (!runtime.rx_to_processing.try_pop(ticket)) {
          std::this_thread::yield();
          continue;
        }
        pending = ticket;
      }
      if (pending->index >= runtime.slot_count) {
        throw std::runtime_error("RX-to-processing ticket has invalid slot index");
      }
      ImageSlot& slot = runtime.slots[pending->index];
      if (slot.generation != pending->generation) {
        throw std::runtime_error("RX-to-processing ticket has stale generation");
      }
      if (!pending->unused) {
        const cudaError_t event_status =
            query_event(slot.placement_done, runtime.processing_counters.event_polls_not_ready);
        if (event_status == cudaErrorNotReady) {
          std::this_thread::yield();
          continue;
        }
        check_cuda(event_status, "cudaEventQuery(placement_done)");
      }
      transition_slot(slot, SlotState::kProcessQueued, SlotState::kProcessing,
                      "processing acquire");
      if (runtime.stage == PipelineStage::kEgress) {
        if (runtime.external_producer == nullptr) {
          throw std::runtime_error("egress processing has no external producer");
        }
        if (!slot.external_lease || !*slot.external_lease) {
          throw std::runtime_error("egress processing has no live UCX batch lease");
        }
        if (pending->unused) {
          runtime.external_producer->release_unused(std::move(*slot.external_lease));
          slot.external_lease.reset();
        } else {
          if (slot.batch.unwrapped_batch_id >
              std::numeric_limits<std::uint64_t>::max() / raw::kImagesPerBatch) {
            throw std::runtime_error("unwrapped batch ID overflows image sequence");
          }
          const std::uint64_t first_sequence = slot.batch.unwrapped_batch_id * raw::kImagesPerBatch;
          if (pending->rejected) {
            runtime.external_producer->cancel(std::move(*slot.external_lease), first_sequence,
                                              raw::kImagesPerBatch);
            slot.external_lease.reset();
          } else {
            check_cuda(gpu::scale_offset_u16_batch_async(
                           static_cast<std::uint16_t*>(slot.device_data), runtime.config.scale,
                           runtime.config.offset, stream),
                       "scale_offset_u16_batch_async");
            runtime.external_producer->submit_after(std::move(*slot.external_lease), first_sequence,
                                                    raw::kImagesPerBatch, stream);
            slot.external_lease.reset();
          }
        }
        transition_slot(slot, SlotState::kProcessing, SlotState::kEgressQueued,
                        "processing-to-UCX publish");
      } else {
        if (runtime.stage == PipelineStage::kProcess) {
          check_cuda(gpu::scale_offset_u16_batch_async(
                         static_cast<std::uint16_t*>(slot.device_data), runtime.config.scale,
                         runtime.config.offset, stream),
                     "scale_offset_u16_batch_async");
        }
        check_cuda(cudaEventRecord(slot.processing_done, stream), "record processing_done");
        transition_slot(slot, SlotState::kProcessing, SlotState::kSinkQueued, "processing publish");
        while (!runtime.processing_to_sink.try_push(*pending)) {
          if (runtime.fatal.load(std::memory_order_acquire)) {
            throw std::runtime_error("fatal error while waiting for processing-to-sink queue");
          }
          ++runtime.processing_counters.sink_queue_full_waits;
          std::this_thread::yield();
        }
        update_max(runtime.processing_counters.sink_queue_max_depth,
                   runtime.processing_to_sink.depth());
      }
      if (!pending->unused) {
        ++runtime.processing_counters.batches;
      }
      pending.reset();
    }
    if (runtime.stage == PipelineStage::kEgress && !runtime.fatal.load(std::memory_order_acquire)) {
      runtime.external_producer->finish_input(runtime.expected_batches * raw::kImagesPerBatch);
    }
    check_cuda(cudaStreamSynchronize(stream), "drain processing stream");
  } catch (const std::exception& error) {
    set_fatal(runtime, std::string("processing thread: ") + error.what());
    if (stream != nullptr) {
      cudaStreamSynchronize(stream);
    }
  }
  runtime.processing_done.store(true, std::memory_order_release);
  if (stream != nullptr) {
    cudaStreamDestroy(stream);
  }
}

void sink_worker(PipelineRuntime& runtime) {
  cudaStream_t stream = nullptr;
  try {
    pin_current_thread(runtime.config.sink_core, "validation/recycle owner");
    check_cuda(cudaSetDevice(runtime.config.gpu_id), "cudaSetDevice(sink)");
    check_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
               "cudaStreamCreate(validation)");
    runtime.sink_ready.store(true, std::memory_order_release);
    std::optional<SlotTicket> pending;
    bool validation_launched = false;
    while (!runtime.fatal.load(std::memory_order_acquire) &&
           (!runtime.processing_done.load(std::memory_order_acquire) || pending ||
            !runtime.processing_to_sink.empty())) {
      if (!pending) {
        SlotTicket ticket;
        if (!runtime.processing_to_sink.try_pop(ticket)) {
          std::this_thread::yield();
          continue;
        }
        pending = ticket;
        validation_launched = false;
      }
      if (pending->index >= runtime.slot_count) {
        throw std::runtime_error("processing-to-sink ticket has invalid slot index");
      }
      ImageSlot& slot = runtime.slots[pending->index];
      if (slot.generation != pending->generation) {
        throw std::runtime_error("processing-to-sink ticket has stale generation");
      }
      if (!validation_launched) {
        const cudaError_t event_status =
            query_event(slot.processing_done, runtime.sink_counters.event_polls_not_ready);
        if (event_status == cudaErrorNotReady) {
          std::this_thread::yield();
          continue;
        }
        check_cuda(event_status, "cudaEventQuery(processing_done)");
        transition_slot(slot, SlotState::kSinkQueued, SlotState::kValidating, "validation acquire");
        if (slot.batch.unwrapped_batch_id >
            std::numeric_limits<std::uint64_t>::max() / raw::kImagesPerBatch) {
          throw std::runtime_error("unwrapped batch ID overflows image sequence");
        }
        const std::uint64_t first_sequence = slot.batch.unwrapped_batch_id * raw::kImagesPerBatch;
        const float expected_scale =
            runtime.stage == PipelineStage::kAssemble ? 1.0F : runtime.config.scale;
        const float expected_offset =
            runtime.stage == PipelineStage::kAssemble ? 0.0F : runtime.config.offset;
        for (std::size_t image = 0; image < raw::kImagesPerBatch; ++image) {
          const auto* image_data =
              static_cast<const std::uint8_t*>(slot.device_data) + image * raw::kImageBytes;
          check_cuda(gpu::validate_transformed_raw_image_async(
                         image_data, first_sequence + image, expected_scale, expected_offset,
                         runtime.validation_result_device, stream),
                     "validate_transformed_raw_image_async");
        }
        check_cuda(cudaEventRecord(slot.validation_done, stream), "record validation_done");
        validation_launched = true;
      }

      const cudaError_t validation_status =
          query_event(slot.validation_done, runtime.sink_counters.event_polls_not_ready);
      if (validation_status == cudaErrorNotReady) {
        std::this_thread::yield();
        continue;
      }
      check_cuda(validation_status, "cudaEventQuery(validation_done)");
      transition_slot(slot, SlotState::kValidating, SlotState::kRecycleQueued,
                      "validation recycle");
      while (!runtime.sink_to_rx.try_push(*pending)) {
        if (runtime.fatal.load(std::memory_order_acquire)) {
          throw std::runtime_error("fatal error while waiting for sink-to-RX queue");
        }
        ++runtime.sink_counters.recycle_queue_full_waits;
        std::this_thread::yield();
      }
      update_max(runtime.sink_counters.recycle_queue_max_depth, runtime.sink_to_rx.depth());
      ++runtime.sink_counters.batches;
      runtime.sink_counters.images += raw::kImagesPerBatch;
      pending.reset();
      validation_launched = false;
    }
    check_cuda(cudaStreamSynchronize(stream), "drain validation stream");
  } catch (const std::exception& error) {
    set_fatal(runtime, std::string("sink thread: ") + error.what());
    if (stream != nullptr) {
      cudaStreamSynchronize(stream);
    }
  }
  runtime.sink_done.store(true, std::memory_order_release);
  if (stream != nullptr) {
    cudaStreamDestroy(stream);
  }
}

void print_summary(const PipelineRuntime& runtime, const raw::AssemblerCounters& assembler,
                   const gpu::ValidationResult& validation,
                   const std::optional<gpu::ExternalBatchProducerStats>& egress_stats,
                   double wall_seconds) {
  const RxCounters& rx = runtime.rx_counters;
  double active_seconds = 0.0;
  if (rx.first_packet_time && rx.last_packet_time) {
    active_seconds =
        std::chrono::duration<double>(*rx.last_packet_time - *rx.first_packet_time).count();
  }
  const double rate_seconds = active_seconds > 0.0 ? active_seconds : wall_seconds;
  const double packet_rate =
      rate_seconds > 0.0 ? static_cast<double>(rx.packets) / rate_seconds : 0;
  const double frame_gbps =
      rate_seconds > 0.0 ? static_cast<double>(rx.bytes) * 8.0 / rate_seconds / 1.0e9 : 0;
  const double image_rate =
      rate_seconds > 0.0 ? static_cast<double>(runtime.sink_counters.images) / rate_seconds : 0;

  std::cout << std::fixed << std::setprecision(3)
            << "raw_pipeline stage=" << stage_name(runtime.stage)
            << " wall_seconds=" << wall_seconds << " active_seconds=" << active_seconds << '\n'
            << "rx bursts=" << rx.bursts << " releases=" << rx.burst_releases
            << " immediate_releases=" << rx.immediate_burst_releases
            << " deferred_releases=" << rx.deferred_burst_releases << " packets=" << rx.packets
            << " bytes=" << rx.bytes << " packet_rate=" << packet_rate
            << " frame_gbps=" << frame_gbps << '\n'
            << "assembly completed_batches=" << rx.completed_batches
            << " completed_images=" << rx.completed_images
            << " rejected_batches=" << rx.rejected_batches
            << " no_slot_batches=" << assembler.batches_dropped_no_slot
            << " missing_batches=" << assembler.batches_missing
            << " start_mid_batch_fragments=" << assembler.fragments_start_mid_batch
            << " duplicate_fragments=" << assembler.duplicate_fragments
            << " timeout_rejects=" << assembler.batches_rejected_timeout
            << " transition_rejects=" << assembler.batches_rejected_transition << '\n'
            << "pipeline processed_batches=" << runtime.processing_counters.batches
            << " validated_batches=" << runtime.sink_counters.batches
            << " validated_images=" << runtime.sink_counters.images << " image_rate=" << image_rate
            << " validation_errors=" << validation.error_count << '\n'
            << "queues rx_to_processing_max=" << rx.process_queue_max_depth
            << " rx_to_processing_full_waits=" << rx.process_queue_full_waits
            << " processing_to_sink_max=" << runtime.processing_counters.sink_queue_max_depth
            << " processing_to_sink_full_waits="
            << runtime.processing_counters.sink_queue_full_waits
            << " sink_to_rx_max=" << runtime.sink_counters.recycle_queue_max_depth
            << " sink_to_rx_full_waits=" << runtime.sink_counters.recycle_queue_full_waits
            << " internal_queue_policy=blocking_handoff" << '\n'
            << "resources pending_burst_max=" << rx.pending_burst_max
            << " descriptor_pool_stalls=" << rx.descriptor_pool_stalls
            << " alias_cache_hits=" << rx.alias_cache_hits
            << " alias_cache_misses=" << rx.alias_cache_misses
            << " slot_occupancy_max=" << rx.slot_occupancy_max << '\n';

  for (std::size_t index = 1; index < rx.header_status.size(); ++index) {
    if (rx.header_status[index] != 0) {
      std::cout << "header_error status="
                << raw::header_status_string(static_cast<raw::HeaderStatus>(index))
                << " count=" << rx.header_status[index] << '\n';
    }
  }
  if (validation.error_count != 0) {
    std::cout << "validation_sample_bad_index=" << validation.sample_bad_index
              << " sample_expected=" << validation.sample_expected
              << " sample_actual=" << validation.sample_actual << '\n';
  }
  if (egress_stats) {
    const gpu::ExternalBatchProducerStats& stats = *egress_stats;
    const double egress_seconds = static_cast<double>(stats.transport.active_nanoseconds) / 1.0e9;
    const double egress_gbps = egress_seconds > 0.0 ? static_cast<double>(stats.transport.bytes) *
                                                          8.0 / egress_seconds / 1.0e9
                                                    : 0.0;
    std::cout << "egress generated=" << stats.transport.generated
              << " submitted_images=" << stats.submitted_images
              << " submitted_batches=" << stats.submitted_batches
              << " admitted=" << stats.transport.admitted
              << " send_completed=" << stats.transport.send_completed
              << " send_failed=" << stats.transport.send_failed
              << " outstanding=" << stats.transport.outstanding
              << " delivery_unknown=" << stats.transport.delivery_unknown
              << " dropped_no_connection=" << stats.transport.dropped_no_connection
              << " dropped_before_submit=" << stats.dropped_before_submit
              << " dropped_no_credit=" << stats.transport.dropped_no_credit
              << " retired_leases=" << stats.retired_batches << " bytes=" << stats.transport.bytes
              << " active_seconds=" << egress_seconds << " payload_gbps=" << egress_gbps << '\n';
  }
}

int run(const CommandLine& command_line) {
  const YAML::Node root = YAML::LoadFile(command_line.config_path);
  PipelineConfig config = load_pipeline_config(root, command_line.stage);
  const std::uint64_t expected_batches = command_line.batches.value_or(0);
  if (expected_batches > std::numeric_limits<std::uint64_t>::max() / raw::kImagesPerBatch) {
    throw std::runtime_error("--batches overflows the fixed image count");
  }
  PipelineRuntime runtime(config, command_line.stage, expected_batches);
  bool daqiri_initialized = false;
  std::unique_ptr<gpu::ExternalBatchProducer> external_producer;
  try {
    initialize_cuda_resources(runtime);
    if (command_line.stage == PipelineStage::kEgress) {
      gpu::ExternalBatchProducerOptions options;
      options.listen_endpoint = config.listen_endpoint;
      options.image_count = expected_batches * raw::kImagesPerBatch;
      options.batch_slot_count = config.batch_slot_count;
      options.max_receiver_queue_depth = config.max_receiver_depth;
      options.gpu_id = config.gpu_id;
      options.cpu_core = config.ucx_core;
      options.memory_kind = config.memory_kind;
      options.timeout = std::chrono::seconds(config.timeout_seconds);
      external_producer = std::make_unique<gpu::ExternalBatchProducer>(std::move(options));
      external_producer->start();
      std::cout << "raw_pipeline listener_ready endpoint=" << config.listen_endpoint << std::endl;
      external_producer->wait_for_receiver();
      std::cout << "raw_pipeline receiver_ready endpoint=" << config.listen_endpoint << std::endl;
      runtime.external_producer = external_producer.get();
    }
    if (daqiri::daqiri_init(command_line.config_path) != daqiri::Status::SUCCESS) {
      throw std::runtime_error("daqiri_init failed");
    }
    daqiri_initialized = true;
    const int port_id = daqiri::get_port_id(config.interface_name);
    if (port_id < 0) {
      throw std::runtime_error("DAQIRI has no interface named " + config.interface_name);
    }
    if (config.queue_id >= static_cast<int>(daqiri::get_num_rx_queues(port_id))) {
      throw std::runtime_error("configured DAQIRI RX queue does not exist");
    }

    std::cout << "raw_pipeline configuration stage=" << stage_name(command_line.stage)
              << " interface=" << config.interface_name << " queue=" << config.queue_id
              << " gpu=" << config.gpu_id << " slots=" << runtime.slot_count
              << " max_burst_packets=" << config.max_burst_packets
              << " source_epoch=" << config.source_epoch
              << " cores=[daqiri:" << config.daqiri_poller_core << ",rx:" << config.rx_core
              << ",processing:" << config.processing_core
              << (command_line.stage == PipelineStage::kEgress ? ",ucx:" : ",sink:")
              << (command_line.stage == PipelineStage::kEgress ? config.ucx_core : config.sink_core)
              << "] scale=" << config.scale << " offset=" << config.offset;
    if (command_line.stage == PipelineStage::kEgress) {
      std::cout << " batches=" << expected_batches << " listen_endpoint=" << config.listen_endpoint
                << " memory_kind=" << gpu::memory_kind_name(config.memory_kind)
                << " batch_slot_count=" << config.batch_slot_count
                << " max_receiver_depth=" << config.max_receiver_depth
                << " timeout_seconds=" << config.timeout_seconds
                << " overall_timeout_seconds=" << config.overall_timeout_seconds
                << " idle_timeout_seconds=" << config.idle_timeout_seconds;
    }
    std::cout << '\n';

    RxOwner rx_owner(runtime, port_id, config.queue_id);
    const Clock::time_point start = Clock::now();
    std::optional<std::thread> sink_thread;
    if (command_line.stage != PipelineStage::kEgress) {
      sink_thread.emplace(sink_worker, std::ref(runtime));
    }
    std::thread processing_thread(processing_worker, std::ref(runtime));
    std::thread rx_thread(&RxOwner::run, &rx_owner);
    const Clock::time_point readiness_deadline =
        Clock::now() + std::chrono::seconds(config.timeout_seconds);
    const bool need_sink = command_line.stage != PipelineStage::kEgress;
    while (!runtime.fatal.load(std::memory_order_acquire) &&
           !(runtime.rx_ready.load(std::memory_order_acquire) &&
             runtime.processing_ready.load(std::memory_order_acquire) &&
             (!need_sink || runtime.sink_ready.load(std::memory_order_acquire)))) {
      if (Clock::now() >= readiness_deadline) {
        set_fatal(runtime, "timed out waiting for pipeline worker readiness");
        break;
      }
      std::this_thread::yield();
    }
    if (!runtime.fatal.load(std::memory_order_acquire)) {
      std::cout << "raw_pipeline ingress_ready interface=" << config.interface_name
                << " queue=" << config.queue_id << std::endl;
    }

    if (command_line.stage == PipelineStage::kEgress) {
      const Clock::time_point overall_deadline =
          start + std::chrono::seconds(config.overall_timeout_seconds);
      const std::uint64_t idle_timeout_ns =
          static_cast<std::uint64_t>(config.idle_timeout_seconds) * 1'000'000'000ULL;
      while (!runtime.fatal.load(std::memory_order_acquire) &&
             !runtime.rx_input_done.load(std::memory_order_acquire)) {
        if (Clock::now() >= overall_deadline) {
          set_fatal(runtime, "egress fixed run exceeded overall_timeout_seconds");
          break;
        }
        const std::uint64_t last_progress =
            runtime.last_input_progress_ns.load(std::memory_order_acquire);
        if (last_progress != 0 && now_ns() - last_progress >= idle_timeout_ns) {
          set_fatal(runtime, "egress raw input exceeded idle_timeout_seconds");
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    } else {
      const Clock::time_point deadline = start + std::chrono::seconds(command_line.seconds);
      while (!runtime.fatal.load(std::memory_order_acquire) && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      runtime.stop_requested.store(true, std::memory_order_release);
    }
    rx_thread.join();
    processing_thread.join();
    if (sink_thread) {
      sink_thread->join();
    }
    // The external pool outlives the transport thread. Synchronize every local
    // CUDA borrower before acknowledging that the registration/allocation may
    // be destroyed, including on fail-stop paths.
    check_cuda(cudaDeviceSynchronize(), "final CUDA drain");
    std::optional<gpu::ExternalBatchProducerStats> egress_stats;
    std::optional<std::string> egress_error;
    if (external_producer) {
      external_producer->acknowledge_local_quiescence();
      external_producer->close();
      egress_error = external_producer->error();
      egress_stats = external_producer->stats();
    }
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - start).count();

    gpu::ValidationResult validation{};
    if (command_line.stage != PipelineStage::kEgress) {
      check_cuda(cudaMemcpy(&validation, runtime.validation_result_device, sizeof(validation),
                            cudaMemcpyDeviceToHost),
                 "copy final validation result");
    }
    print_summary(runtime, rx_owner.assembler_counters(), validation, egress_stats, wall_seconds);

    if (daqiri_initialized) {
      daqiri::shutdown();
      daqiri_initialized = false;
    }
    if (runtime.fatal.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(runtime.fatal_mutex);
      throw std::runtime_error(runtime.fatal_message);
    }
    if (egress_error) {
      throw std::runtime_error("UCX egress failed: " + *egress_error);
    }
    destroy_cuda_resources(runtime);
    return validation.error_count == 0 ? 0 : 2;
  } catch (...) {
    // All worker catch paths synchronize and destroy their streams before
    // joining. This final device drain also covers failures before a normal
    // join sequence reaches the success path.
    cudaDeviceSynchronize();
    if (external_producer) {
      external_producer->acknowledge_local_quiescence();
      external_producer->close();
      external_producer.reset();
      runtime.external_producer = nullptr;
    }
    if (daqiri_initialized) {
      daqiri::shutdown();
    }
    destroy_cuda_resources(runtime);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(parse_command_line(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << argv[0] << ": " << error.what() << '\n';
    return 1;
  }
}
