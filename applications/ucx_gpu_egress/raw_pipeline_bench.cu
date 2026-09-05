// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "cuda_image.h"
#include "image_geometry.h"
#include "processing/scale_offset.h"
#include "ucx_transport.h"

#include <daqiri/daqiri.h>

#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <pthread.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace geometry = daqiri::ucx_example::geometry;
namespace gpu = daqiri::ucx_gpu;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kPacketsPerImage = 16;
constexpr std::uint32_t kPacketsPerBatch =
    static_cast<std::uint32_t>(geometry::kImagesPerBatch) * kPacketsPerImage;
constexpr std::uint32_t kFragmentBytes =
    static_cast<std::uint32_t>(geometry::kImageBytes / kPacketsPerImage);
constexpr std::uint64_t kBatchSpace = (std::uint64_t{1} << 32U) / kPacketsPerBatch;

static_assert(geometry::kImageBytes % kPacketsPerImage == 0);
static_assert(geometry::kBatchBytes == gpu::kProcessingBatchBytes);

enum class Stage { receive, process, egress };

struct PipelineConfig {
  std::string interface_name{"rx_port"};
  std::string reorder_name{"rx_image_batches"};
  int queue_id{0};
  int gpu_id{0};
  int app_core{17};
  int ucx_core{16};
  int daqiri_poller_core{-1};
  std::size_t slot_count{16};
  float scale{1.0F};
  float offset{0.0F};
  std::string listen_endpoint{"0.0.0.0:13341"};
  gpu::MemoryKind memory_kind{gpu::MemoryKind::host_pinned_mapped};
  int timeout_seconds{30};
  int overall_timeout_seconds{300};
  int idle_timeout_seconds{10};
};

struct CommandLine {
  std::string config_path;
  Stage stage{Stage::receive};
  int seconds{10};
  bool seconds_provided{false};
  std::optional<std::uint64_t> batches;
};

struct Counters {
  std::uint64_t rx_batches{0};
  std::uint64_t rx_packets{0};
  std::uint64_t rx_payload_bytes{0};
  std::uint64_t released_bursts{0};
  std::uint64_t completed_batches{0};
  std::uint64_t processed_batches{0};
  std::uint64_t submitted_batches{0};
  std::uint64_t submitted_images{0};
  std::uint64_t missing_batches{0};
  std::uint64_t incomplete_batches{0};
  std::uint64_t stale_batches{0};
  std::uint64_t event_polls_not_ready{0};
  std::uint64_t no_lease_polls{0};
  std::size_t max_ready_depth{0};
  std::size_t max_in_flight{0};
  std::optional<Clock::time_point> first_batch_time;
  std::optional<Clock::time_point> last_batch_time;
};

struct InFlightBurst {
  daqiri::BurstParams* burst{nullptr};
  std::size_t event_index{0};
};

void check_cuda(cudaError_t status, const char* operation) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
  }
}

const char* stage_name(Stage stage) {
  switch (stage) {
    case Stage::receive:
      return "receive";
    case Stage::process:
      return "process";
    case Stage::egress:
      return "egress";
  }
  return "unknown";
}

std::uint64_t unwrap_batch_id(std::uint64_t batch_id, std::uint64_t next_expected) {
  const std::uint64_t base = next_expected - next_expected % kBatchSpace;
  std::uint64_t candidate = base + batch_id % kBatchSpace;
  if (candidate + kBatchSpace / 2U < next_expected) {
    candidate += kBatchSpace;
  } else if (candidate > next_expected + kBatchSpace / 2U && candidate >= kBatchSpace) {
    candidate -= kBatchSpace;
  }
  return candidate;
}

void pin_current_thread(int core) {
  if (core < 0 || core >= CPU_SETSIZE) {
    throw std::runtime_error("invalid app_core " + std::to_string(core));
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  const int status = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (status != 0) {
    throw std::runtime_error("pthread_setaffinity_np failed for app_core " + std::to_string(core));
  }
}

CommandLine parse_command_line(int argc, char** argv) {
  if (argc < 2) {
    throw std::runtime_error(std::string("usage: ") + argv[0] +
                             " CONFIG --stage receive|process|egress "
                             "[--seconds N | --batches N]");
  }
  CommandLine command;
  command.config_path = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string option = argv[index];
    if (index + 1 >= argc) {
      throw std::runtime_error("missing value for " + option);
    }
    const std::string value = argv[++index];
    if (option == "--stage") {
      if (value == "receive") {
        command.stage = Stage::receive;
      } else if (value == "process") {
        command.stage = Stage::process;
      } else if (value == "egress") {
        command.stage = Stage::egress;
      } else {
        throw std::runtime_error("--stage must be receive, process, or egress");
      }
    } else if (option == "--seconds") {
      command.seconds = std::stoi(value);
      command.seconds_provided = true;
    } else if (option == "--batches") {
      command.batches = std::stoull(value);
    } else {
      throw std::runtime_error("unknown option: " + option);
    }
  }
  if (command.seconds <= 0) {
    throw std::runtime_error("--seconds must be positive");
  }
  if (command.batches && (*command.batches == 0 || *command.batches > kBatchSpace)) {
    throw std::runtime_error("--batches must fit within the 32-bit source sequence space");
  }
  if (command.seconds_provided && command.batches) {
    throw std::runtime_error("specify either --seconds or --batches, not both");
  }
  if (command.stage == Stage::egress && !command.batches) {
    throw std::runtime_error("--stage egress requires --batches N");
  }
  return command;
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

bool has_gpu_reorder(const YAML::Node& root, const PipelineConfig& config) {
  const YAML::Node interfaces = root["daqiri"]["cfg"]["interfaces"];
  if (!interfaces || !interfaces.IsSequence()) {
    return false;
  }
  for (const YAML::Node& interface : interfaces) {
    if (interface["name"].as<std::string>("") != config.interface_name) {
      continue;
    }
    const YAML::Node reorders = interface["rx"]["reorder_configs"];
    if (!reorders || !reorders.IsSequence()) {
      return false;
    }
    for (const YAML::Node& reorder : reorders) {
      if (reorder["name"].as<std::string>("") == config.reorder_name &&
          reorder["reorder_type"].as<std::string>("") == "gpu") {
        return true;
      }
    }
  }
  return false;
}

bool has_host_pinned_rx_source(const YAML::Node& root, const PipelineConfig& config) {
  std::string source_name;
  const YAML::Node interfaces = root["daqiri"]["cfg"]["interfaces"];
  if (interfaces && interfaces.IsSequence()) {
    for (const YAML::Node& interface : interfaces) {
      if (interface["name"].as<std::string>("") != config.interface_name) {
        continue;
      }
      const YAML::Node queues = interface["rx"]["queues"];
      if (!queues || !queues.IsSequence()) {
        break;
      }
      for (const YAML::Node& queue : queues) {
        if (queue["id"].as<int>(-1) != config.queue_id) {
          continue;
        }
        const YAML::Node regions = queue["memory_regions"];
        if (regions && regions.IsSequence() && regions.size() == 1) {
          source_name = regions[0].as<std::string>("");
        }
        break;
      }
    }
  }
  const YAML::Node regions = root["daqiri"]["cfg"]["memory_regions"];
  if (source_name.empty() || !regions || !regions.IsSequence()) {
    return false;
  }
  for (const YAML::Node& region : regions) {
    if (region["name"].as<std::string>("") == source_name) {
      return region["kind"].as<std::string>("") == "host_pinned";
    }
  }
  return false;
}

PipelineConfig load_pipeline_config(const YAML::Node& root, Stage stage) {
  const YAML::Node node = root["ucx_gpu_raw_processor"];
  if (!node || !node.IsMap()) {
    throw std::runtime_error("config requires a ucx_gpu_raw_processor map");
  }
  PipelineConfig config;
  config.interface_name = node["interface_name"].as<std::string>(config.interface_name);
  config.reorder_name = node["reorder_name"].as<std::string>(config.reorder_name);
  config.queue_id = node["queue_id"].as<int>(config.queue_id);
  config.gpu_id = node["gpu_id"].as<int>(config.gpu_id);
  config.app_core = node["app_core"].as<int>(config.app_core);
  config.ucx_core = node["ucx_core"].as<int>(config.ucx_core);
  config.slot_count = node["slot_count"].as<std::size_t>(config.slot_count);
  config.scale = node["scale"].as<float>(config.scale);
  config.offset = node["offset"].as<float>(config.offset);
  config.listen_endpoint = node["listen_endpoint"].as<std::string>(config.listen_endpoint);
  const std::string memory_kind =
      node["memory_kind"].as<std::string>(gpu::memory_kind_name(config.memory_kind));
  if (!gpu::parse_memory_kind(memory_kind, config.memory_kind)) {
    throw std::runtime_error("memory_kind must be host_pinned_mapped or cuda_device");
  }
  config.timeout_seconds = node["timeout_seconds"].as<int>(config.timeout_seconds);
  config.overall_timeout_seconds =
      node["overall_timeout_seconds"].as<int>(config.overall_timeout_seconds);
  config.idle_timeout_seconds = node["idle_timeout_seconds"].as<int>(config.idle_timeout_seconds);
  config.daqiri_poller_core = find_daqiri_poller_core(root, config.interface_name, config.queue_id);

  if (config.interface_name.empty() || config.reorder_name.empty() || config.queue_id < 0 ||
      config.gpu_id < 0 || config.app_core < 0 || config.daqiri_poller_core < 0 ||
      config.slot_count < 2 || !std::isfinite(config.scale) || !std::isfinite(config.offset) ||
      config.timeout_seconds <= 0 || config.overall_timeout_seconds <= 0 ||
      config.idle_timeout_seconds <= 0 ||
      (stage == Stage::egress && (config.ucx_core < 0 || config.listen_endpoint.empty()))) {
    throw std::runtime_error("ucx_gpu_raw_processor contains an invalid or missing value");
  }
  if (!has_gpu_reorder(root, config)) {
    throw std::runtime_error("reorder_name must select a GPU RX reorder config");
  }
  if (!has_host_pinned_rx_source(root, config)) {
    throw std::runtime_error(
        "raw pipeline requires one host_pinned RX source region so DAQIRI can enforce source "
        "batch boundaries");
  }
  if (config.app_core == config.daqiri_poller_core ||
      (stage == Stage::egress &&
       (config.ucx_core == config.app_core || config.ucx_core == config.daqiri_poller_core))) {
    throw std::runtime_error("DAQIRI, application, and UCX cores must not overlap");
  }
  return config;
}

class Pipeline {
 public:
  Pipeline(PipelineConfig config, CommandLine command)
      : config_(std::move(config)), command_(std::move(command)) {}

  int run() {
    const auto wall_start = Clock::now();
    try {
      initialize();
      event_loop();
      finish_successfully();
    } catch (...) {
      cleanup_failed();
      throw;
    }

    const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();
    print_summary(wall_seconds);
    cleanup_cuda();
    return command_.stage == Stage::egress || validation_.error_count == 0 ? 0 : 1;
  }

 private:
  void initialize() {
    pin_current_thread(config_.app_core);
    device_aliases_.reserve(config_.slot_count);
    in_flight_.reserve(config_.slot_count);
    check_cuda(cudaSetDevice(config_.gpu_id), "cudaSetDevice");
    check_cuda(cudaFree(nullptr), "initialize CUDA context");
    check_cuda(cudaStreamCreateWithFlags(&reorder_stream_, cudaStreamNonBlocking),
               "cudaStreamCreate(reorder)");
    check_cuda(cudaStreamCreateWithFlags(&work_stream_, cudaStreamNonBlocking),
               "cudaStreamCreate(work)");
    for (std::size_t index = 0; index < config_.slot_count; ++index) {
      cudaEvent_t event = nullptr;
      check_cuda(cudaEventCreateWithFlags(&event, cudaEventDisableTiming),
                 "cudaEventCreate(completion)");
      completion_events_.push_back(event);
      free_events_.push_back(index);
    }
    if (command_.stage != Stage::egress) {
      check_cuda(cudaMalloc(reinterpret_cast<void**>(&validation_device_), sizeof(validation_)),
                 "cudaMalloc(validation result)");
      check_cuda(
          cudaMemcpy(validation_device_, &validation_, sizeof(validation_), cudaMemcpyHostToDevice),
          "initialize validation result");
    }

    if (command_.stage == Stage::egress) {
      gpu::ExternalBatchProducerOptions options;
      options.listen_endpoint = config_.listen_endpoint;
      options.image_count = *command_.batches * geometry::kImagesPerBatch;
      options.batch_slot_count = config_.slot_count;
      options.max_receiver_queue_depth = config_.slot_count;
      options.gpu_id = config_.gpu_id;
      options.cpu_core = config_.ucx_core;
      options.memory_kind = config_.memory_kind;
      options.wait_for_credit = false;
      options.timeout = std::chrono::seconds(config_.timeout_seconds);
      producer_ = std::make_unique<gpu::ExternalBatchProducer>(std::move(options));
      producer_->start();
      std::cout << "raw_pipeline listener_ready endpoint=" << config_.listen_endpoint << std::endl;
      producer_->wait_for_receiver();
      std::cout << "raw_pipeline receiver_ready endpoint=" << config_.listen_endpoint << std::endl;
    }

    if (daqiri::daqiri_init(command_.config_path) != daqiri::Status::SUCCESS) {
      throw std::runtime_error("daqiri_init failed");
    }
    daqiri_initialized_ = true;
    port_id_ = daqiri::get_port_id(config_.interface_name);
    if (port_id_ < 0 || config_.queue_id >= static_cast<int>(daqiri::get_num_rx_queues(port_id_))) {
      throw std::runtime_error("configured DAQIRI RX interface or queue does not exist");
    }
    if (daqiri::set_reorder_cuda_stream(config_.interface_name, config_.reorder_name,
                                        reorder_stream_) != daqiri::Status::SUCCESS) {
      throw std::runtime_error("set_reorder_cuda_stream failed");
    }

    std::cout << "raw_pipeline ingress_ready stage=" << stage_name(command_.stage)
              << " interface=" << config_.interface_name << " queue=" << config_.queue_id
              << " reorder=" << config_.reorder_name << " gpu=" << config_.gpu_id
              << " slots=" << config_.slot_count << " cores=[daqiri:" << config_.daqiri_poller_core
              << ",app:" << config_.app_core;
    if (command_.stage == Stage::egress) {
      std::cout << ",ucx:" << config_.ucx_core << "] batches=" << *command_.batches
                << " memory_kind=" << gpu::memory_kind_name(config_.memory_kind);
    } else if (command_.batches) {
      std::cout << "] batches=" << *command_.batches;
    } else {
      std::cout << "] seconds=" << command_.seconds;
    }
    std::cout << " scale=" << config_.scale << " offset=" << config_.offset << std::endl;
  }

  void event_loop() {
    const Clock::time_point start = Clock::now();
    const Clock::time_point deadline = start + std::chrono::seconds(command_.seconds);
    const Clock::time_point overall_deadline =
        start + std::chrono::seconds(config_.overall_timeout_seconds);
    Clock::time_point last_progress = start;
    bool input_done = false;

    while (!input_done) {
      bool progressed = drain_completed();
      progressed = process_ready() || progressed;

      if (command_.batches) {
        if (next_expected_batch_ >= *command_.batches) {
          input_done = true;
        } else if (Clock::now() >= overall_deadline) {
          throw std::runtime_error("overall timeout before the fixed raw run completed");
        } else if (Clock::now() - last_progress >=
                   std::chrono::seconds(config_.idle_timeout_seconds)) {
          throw std::runtime_error("raw input idle timeout before the fixed run completed");
        }
      } else if (Clock::now() >= deadline) {
        input_done = true;
      }

      if (!input_done && ready_.size() + in_flight_.size() < config_.slot_count) {
        daqiri::BurstParams* burst = nullptr;
        const daqiri::Status status = daqiri::get_rx_burst(&burst, port_id_, config_.queue_id);
        if (status == daqiri::Status::SUCCESS && burst != nullptr) {
          ready_.push_back(burst);
          counters_.max_ready_depth = std::max(counters_.max_ready_depth, ready_.size());
          last_progress = Clock::now();
          progressed = true;
        } else if (status != daqiri::Status::NOT_READY && status != daqiri::Status::NULL_PTR) {
          throw std::runtime_error("get_rx_burst failed with status " +
                                   std::to_string(static_cast<int>(status)));
        }
      }

      if (!progressed) {
        std::this_thread::yield();
      }
      if (producer_) {
        if (std::optional<std::string> error = producer_->error()) {
          throw std::runtime_error("UCX producer failed: " + *error);
        }
      }
    }

    // Complete every burst already accepted from DAQIRI before releasing its
    // output pool or declaring the producer locally quiescent.
    while (!ready_.empty() || !in_flight_.empty()) {
      bool progressed = drain_completed();
      progressed = process_ready() || progressed;
      if (!progressed) {
        std::this_thread::yield();
      }
    }
  }

  bool process_ready() {
    bool progressed = false;
    while (!ready_.empty()) {
      daqiri::BurstParams* burst = ready_.front();
      daqiri::ReorderBurstInfo info{};
      const daqiri::Status info_status = daqiri::get_reorder_burst_info(burst, &info);
      if (info_status == daqiri::Status::NOT_READY) {
        ++counters_.event_polls_not_ready;
        break;
      }
      if (info_status != daqiri::Status::SUCCESS) {
        throw std::runtime_error("get_reorder_burst_info failed with status " +
                                 std::to_string(static_cast<int>(info_status)));
      }
      const std::uint64_t batch_id = unwrap_batch_id(info.batch_id, next_expected_batch_);
      const bool stale = batch_id < next_expected_batch_;
      const bool complete = info.source_packet_count == kPacketsPerBatch &&
                            info.packets_per_batch == kPacketsPerBatch &&
                            info.payload_len == kFragmentBytes &&
                            info.aggregate_len == geometry::kBatchBytes &&
                            (info.burst_flags & daqiri::DAQIRI_BURST_FLAG_REORDER_TIMEOUT) == 0U;
      if (!stale && complete && free_events_.empty()) {
        break;
      }

      std::optional<gpu::BatchLease> lease;
      if (!stale && complete && command_.stage == Stage::egress) {
        std::optional<gpu::BatchLease> acquired = producer_->try_acquire();
        if (!acquired) {
          ++counters_.no_lease_polls;
          break;
        }
        lease.emplace(std::move(*acquired));
        if (lease->size() != geometry::kBatchBytes) {
          producer_->discard(std::move(*lease));
          throw std::runtime_error("UCX producer returned a batch slot with the wrong size");
        }
      }

      ready_.pop_front();
      try {
        note_input(info);
        if (stale) {
          ++counters_.stale_batches;
          release_burst(burst);
          progressed = true;
          continue;
        }
        counters_.missing_batches += batch_id - next_expected_batch_;
        next_expected_batch_ = batch_id + 1;

        if (!complete) {
          ++counters_.incomplete_batches;
          release_burst(burst);
          progressed = true;
          continue;
        }

        void* source = device_pointer(daqiri::get_segment_packet_ptr(burst, 0, 0));
        if (source == nullptr) {
          throw std::runtime_error("reordered burst has no CUDA-addressable aggregate");
        }

        const std::size_t event_index = free_events_.front();
        free_events_.pop_front();
        const std::uint64_t first_sequence = batch_id * geometry::kImagesPerBatch;
        if (command_.stage != Stage::egress) {
          const float scale = command_.stage == Stage::process ? config_.scale : 1.0F;
          const float offset = command_.stage == Stage::process ? config_.offset : 0.0F;
          if (command_.stage == Stage::process) {
            check_cuda(gpu::scale_offset_u16_batch_async(static_cast<std::uint16_t*>(source), scale,
                                                         offset, work_stream_),
                       "scale_offset_u16_batch_async");
            ++counters_.processed_batches;
          }
          for (std::size_t image = 0; image < geometry::kImagesPerBatch; ++image) {
            const auto* image_data =
                static_cast<const std::uint8_t*>(source) + image * geometry::kImageBytes;
            check_cuda(
                gpu::validate_transformed_raw_image_async(image_data, first_sequence + image, scale,
                                                          offset, validation_device_, work_stream_),
                "validate_transformed_raw_image_async");
          }
          check_cuda(cudaEventRecord(completion_events_[event_index], work_stream_),
                     "cudaEventRecord(validation completion)");
        } else {
          check_cuda(cudaMemcpyAsync(lease->device_data(), source, geometry::kBatchBytes,
                                     cudaMemcpyDeviceToDevice, work_stream_),
                     "cudaMemcpyAsync(reordered batch to UCX lease)");
          // The DAQIRI output can be released as soon as the bounded copy has
          // completed; later transform and UCX work touch only the lease.
          check_cuda(cudaEventRecord(completion_events_[event_index], work_stream_),
                     "cudaEventRecord(copy completion)");
          check_cuda(
              gpu::scale_offset_u16_batch_async(static_cast<std::uint16_t*>(lease->device_data()),
                                                config_.scale, config_.offset, work_stream_),
              "scale_offset_u16_batch_async");
          producer_->submit_after(std::move(*lease), first_sequence,
                                  static_cast<std::uint32_t>(geometry::kImagesPerBatch),
                                  work_stream_);
          ++counters_.processed_batches;
          ++counters_.submitted_batches;
          counters_.submitted_images += geometry::kImagesPerBatch;
        }
        ++counters_.completed_batches;
        in_flight_.push_back({burst, event_index});
        counters_.max_in_flight = std::max(counters_.max_in_flight, in_flight_.size());
        progressed = true;
      } catch (...) {
        cudaStreamSynchronize(work_stream_);
        if (lease && *lease && producer_) {
          try {
            producer_->discard(std::move(*lease));
          } catch (...) {
          }
        }
        release_burst(burst);
        throw;
      }
    }
    return progressed;
  }

  bool drain_completed() {
    bool progressed = false;
    auto current = in_flight_.begin();
    while (current != in_flight_.end()) {
      const cudaError_t status = cudaEventQuery(completion_events_[current->event_index]);
      if (status == cudaErrorNotReady) {
        ++counters_.event_polls_not_ready;
        ++current;
        continue;
      }
      check_cuda(status, "cudaEventQuery(completion)");
      release_burst(current->burst);
      free_events_.push_back(current->event_index);
      current = in_flight_.erase(current);
      progressed = true;
    }
    return progressed;
  }

  void note_input(const daqiri::ReorderBurstInfo& info) {
    const Clock::time_point now = Clock::now();
    if (!counters_.first_batch_time) {
      counters_.first_batch_time = now;
    }
    counters_.last_batch_time = now;
    ++counters_.rx_batches;
    counters_.rx_packets += info.source_packet_count;
    counters_.rx_payload_bytes += info.aggregate_len;
  }

  void* device_pointer(void* pointer) {
    if (pointer == nullptr) {
      return nullptr;
    }
    const auto found = device_aliases_.find(pointer);
    if (found != device_aliases_.end()) {
      return found->second;
    }
    cudaPointerAttributes attributes{};
    check_cuda(cudaPointerGetAttributes(&attributes, pointer), "cudaPointerGetAttributes");
    void* device = pointer;
    if (attributes.type == cudaMemoryTypeHost) {
      check_cuda(cudaHostGetDevicePointer(&device, pointer, 0), "cudaHostGetDevicePointer");
    } else if (attributes.type != cudaMemoryTypeDevice &&
               attributes.type != cudaMemoryTypeManaged) {
      throw std::runtime_error("reorder output is not CUDA-addressable");
    }
    device_aliases_.emplace(pointer, device);
    return device;
  }

  void release_burst(daqiri::BurstParams* burst) noexcept {
    daqiri::free_all_packets_and_burst_rx(burst);
    ++counters_.released_bursts;
  }

  void finish_successfully() {
    check_cuda(cudaStreamSynchronize(reorder_stream_), "cudaStreamSynchronize(reorder)");
    if (work_stream_ != nullptr) {
      check_cuda(cudaStreamSynchronize(work_stream_), "cudaStreamSynchronize(work)");
    }
    drain_completed();

    if (command_.stage != Stage::egress) {
      check_cuda(
          cudaMemcpy(&validation_, validation_device_, sizeof(validation_), cudaMemcpyDeviceToHost),
          "copy validation result");
    }
    if (command_.batches &&
        (counters_.rx_batches != *command_.batches ||
         counters_.completed_batches != *command_.batches || counters_.missing_batches != 0 ||
         counters_.incomplete_batches != 0 || counters_.stale_batches != 0 ||
         (command_.stage != Stage::receive && counters_.processed_batches != *command_.batches))) {
      throw std::runtime_error("fixed raw run did not complete every expected batch cleanly");
    }
    if (producer_) {
      producer_->finish_input(*command_.batches * geometry::kImagesPerBatch);
      producer_->acknowledge_local_quiescence();
      producer_->close();
      if (std::optional<std::string> error = producer_->error()) {
        throw std::runtime_error("UCX producer failed: " + *error);
      }
      egress_stats_ = producer_->stats();
      if (egress_stats_->retired_batches != egress_stats_->submitted_batches ||
          egress_stats_->submitted_batches != counters_.submitted_batches ||
          egress_stats_->submitted_images != counters_.submitted_images) {
        throw std::runtime_error("UCX producer completion accounting mismatch");
      }
      if (egress_stats_->dropped_before_submit != 0 ||
          egress_stats_->transport.dropped_no_credit != 0 ||
          egress_stats_->transport.delivery_unknown != 0 ||
          egress_stats_->transport.batches_delivered != egress_stats_->transport.batches_sent) {
        throw std::runtime_error("fixed UCX egress run lost one or more submitted batches");
      }
    }
    daqiri::print_stats();
    daqiri::shutdown();
    daqiri_initialized_ = false;
  }

  void cleanup_failed() noexcept {
    if (reorder_stream_ != nullptr) {
      cudaStreamSynchronize(reorder_stream_);
    }
    if (work_stream_ != nullptr) {
      cudaStreamSynchronize(work_stream_);
    }
    while (!ready_.empty()) {
      release_burst(ready_.front());
      ready_.pop_front();
    }
    for (const InFlightBurst& pending : in_flight_) {
      release_burst(pending.burst);
    }
    in_flight_.clear();
    if (producer_) {
      producer_->acknowledge_local_quiescence();
      producer_->close();
    }
    if (daqiri_initialized_) {
      daqiri::shutdown();
      daqiri_initialized_ = false;
    }
    cleanup_cuda();
  }

  void cleanup_cuda() noexcept {
    if (validation_device_ != nullptr) {
      cudaFree(validation_device_);
      validation_device_ = nullptr;
    }
    for (cudaEvent_t event : completion_events_) {
      cudaEventDestroy(event);
    }
    completion_events_.clear();
    if (work_stream_ != nullptr) {
      cudaStreamDestroy(work_stream_);
      work_stream_ = nullptr;
    }
    if (reorder_stream_ != nullptr) {
      cudaStreamDestroy(reorder_stream_);
      reorder_stream_ = nullptr;
    }
  }

  void print_summary(double wall_seconds) const {
    double active_seconds = 0.0;
    if (counters_.first_batch_time && counters_.last_batch_time) {
      active_seconds =
          std::chrono::duration<double>(*counters_.last_batch_time - *counters_.first_batch_time)
              .count();
    }
    const double rate_seconds = active_seconds > 0.0 ? active_seconds : wall_seconds;
    const double payload_gbps =
        rate_seconds > 0.0
            ? static_cast<double>(counters_.rx_payload_bytes) * 8.0 / rate_seconds / 1.0e9
            : 0.0;
    const double image_rate =
        rate_seconds > 0.0
            ? static_cast<double>(counters_.completed_batches * geometry::kImagesPerBatch) /
                  rate_seconds
            : 0.0;

    std::cout << std::fixed << std::setprecision(3)
              << "raw_pipeline stage=" << stage_name(command_.stage)
              << " wall_seconds=" << wall_seconds << " active_seconds=" << active_seconds << '\n'
              << "rx reordered_batches=" << counters_.rx_batches
              << " source_packets=" << counters_.rx_packets
              << " payload_bytes=" << counters_.rx_payload_bytes << " payload_gbps=" << payload_gbps
              << " released_bursts=" << counters_.released_bursts << '\n'
              << "pipeline completed_batches=" << counters_.completed_batches
              << " processed_batches=" << counters_.processed_batches
              << " images=" << counters_.completed_batches * geometry::kImagesPerBatch
              << " image_rate=" << image_rate << " missing_batches=" << counters_.missing_batches
              << " incomplete_batches=" << counters_.incomplete_batches
              << " stale_batches=" << counters_.stale_batches;
    if (command_.stage != Stage::egress) {
      std::cout << " validation_errors=" << validation_.error_count;
    }
    std::cout << '\n'
              << "resources max_ready_depth=" << counters_.max_ready_depth
              << " max_in_flight=" << counters_.max_in_flight
              << " event_polls_not_ready=" << counters_.event_polls_not_ready
              << " no_lease_polls=" << counters_.no_lease_polls << '\n';

    if (validation_.error_count != 0) {
      std::cout << "validation_sample_bad_index=" << validation_.sample_bad_index
                << " sample_expected=" << validation_.sample_expected
                << " sample_actual=" << validation_.sample_actual << '\n';
    }
    if (egress_stats_) {
      const auto& stats = *egress_stats_;
      const double egress_seconds = static_cast<double>(stats.transport.active_nanoseconds) / 1.0e9;
      const double egress_gbps = egress_seconds > 0.0 ? static_cast<double>(stats.transport.bytes) *
                                                            8.0 / egress_seconds / 1.0e9
                                                      : 0.0;
      std::cout << "egress submitted_batches=" << counters_.submitted_batches
                << " submitted_images=" << counters_.submitted_images
                << " retired_batches=" << stats.retired_batches
                << " admitted=" << stats.transport.admitted
                << " batches_sent=" << stats.transport.batches_sent
                << " batches_delivered=" << stats.transport.batches_delivered
                << " batches_released=" << stats.transport.batches_released
                << " dropped_before_submit=" << stats.dropped_before_submit
                << " dropped_no_credit=" << stats.transport.dropped_no_credit
                << " delivery_unknown=" << stats.transport.delivery_unknown
                << " payload_bytes=" << stats.transport.bytes
                << " active_seconds=" << egress_seconds << " payload_gbps=" << egress_gbps << '\n';
    }
  }

  PipelineConfig config_;
  CommandLine command_;
  int port_id_{-1};
  bool daqiri_initialized_{false};
  cudaStream_t reorder_stream_{nullptr};
  cudaStream_t work_stream_{nullptr};
  gpu::ValidationResult validation_{0, std::numeric_limits<unsigned long long>::max(), 0, 0};
  gpu::ValidationResult* validation_device_{nullptr};
  std::vector<cudaEvent_t> completion_events_;
  std::deque<std::size_t> free_events_;
  std::deque<daqiri::BurstParams*> ready_;
  std::vector<InFlightBurst> in_flight_;
  std::unordered_map<void*, void*> device_aliases_;
  std::unique_ptr<gpu::ExternalBatchProducer> producer_;
  std::optional<gpu::ExternalBatchProducerStats> egress_stats_;
  std::uint64_t next_expected_batch_{0};
  Counters counters_;
};

int run(const CommandLine& command) {
  const YAML::Node root = YAML::LoadFile(command.config_path);
  return Pipeline(load_pipeline_config(root, command.stage), command).run();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(parse_command_line(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
