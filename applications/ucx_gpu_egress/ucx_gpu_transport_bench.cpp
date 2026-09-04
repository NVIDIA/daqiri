// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "cuda_image.h"
#include "ucx_transport.h"

#include <cuda_runtime.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using daqiri::ucx_gpu::MemoryKind;

enum class ValidationMode { generated, raw_transform };

struct Cli {
  std::string mode;
  std::string listen{"0.0.0.0:13341"};
  std::string connect;
  std::string local;
  std::uint64_t images{1024};
  std::size_t queue_depth{64};
  std::size_t batch_slots{16};
  int gpu_id{0};
  int cpu_core{-1};
  MemoryKind memory_kind{MemoryKind::host_pinned_mapped};
  bool wait_for_credit{true};
  int timeout_seconds{30};
  ValidationMode validation{ValidationMode::generated};
  float scale{1.0F};
  float offset{0.0F};
};

void usage(const char* program) {
  std::cout << "Usage:\n"
            << "  " << program << " --mode producer --listen IP:PORT [options]\n"
            << "  " << program << " --mode receiver --connect IP:PORT --local IP:0 [options]\n\n"
            << "Options:\n"
            << "  --images N            fixed generated image count (default 1024)\n"
            << "  --queue-depth N       producer/receiver slot count (default 64)\n"
            << "  --batch-slots N       producer 2-MiB batch slots (default 16)\n"
            << "  --gpu-id N            CUDA device (default 0)\n"
            << "  --cpu-core N          UCX progress-thread CPU core\n"
            << "  --memory-kind KIND    host_pinned_mapped|cuda_device\n"
            << "  --credit-mode MODE    wait|drop (default wait)\n"
            << "  --timeout-seconds N   connection/EOS timeout (default 30)\n"
            << "  --validation MODE     generated|raw-transform (receiver, default generated)\n"
            << "  --scale F             raw-transform scale (default 1)\n"
            << "  --offset F            raw-transform offset (default 0)\n";
}

std::uint64_t parse_u64(const std::string& text, const char* option) {
  std::size_t consumed = 0;
  const auto value = std::stoull(text, &consumed);
  if (consumed != text.size()) {
    throw std::invalid_argument(std::string(option) + " requires an integer");
  }
  return value;
}

Cli parse_cli(int argc, char** argv) {
  Cli cli;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--help" || option == "-h") {
      usage(argv[0]);
      std::exit(0);
    }
    if (i + 1 >= argc) {
      throw std::invalid_argument("missing value for " + option);
    }
    const std::string value = argv[++i];
    if (option == "--mode") {
      cli.mode = value;
    } else if (option == "--listen") {
      cli.listen = value;
    } else if (option == "--connect") {
      cli.connect = value;
    } else if (option == "--local") {
      cli.local = value;
    } else if (option == "--images") {
      cli.images = parse_u64(value, "--images");
    } else if (option == "--queue-depth") {
      const auto parsed = parse_u64(value, "--queue-depth");
      if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("--queue-depth is too large");
      }
      cli.queue_depth = static_cast<std::size_t>(parsed);
    } else if (option == "--batch-slots") {
      cli.batch_slots = static_cast<std::size_t>(parse_u64(value, "--batch-slots"));
    } else if (option == "--gpu-id") {
      cli.gpu_id = static_cast<int>(parse_u64(value, "--gpu-id"));
    } else if (option == "--cpu-core") {
      cli.cpu_core = static_cast<int>(parse_u64(value, "--cpu-core"));
    } else if (option == "--memory-kind") {
      if (!daqiri::ucx_gpu::parse_memory_kind(value, cli.memory_kind)) {
        throw std::invalid_argument("unknown --memory-kind: " + value);
      }
    } else if (option == "--credit-mode") {
      if (value == "wait") {
        cli.wait_for_credit = true;
      } else if (value == "drop") {
        cli.wait_for_credit = false;
      } else {
        throw std::invalid_argument("--credit-mode must be wait or drop");
      }
    } else if (option == "--timeout-seconds") {
      cli.timeout_seconds = static_cast<int>(parse_u64(value, "--timeout-seconds"));
    } else if (option == "--validation") {
      if (value == "generated") {
        cli.validation = ValidationMode::generated;
      } else if (value == "raw-transform") {
        cli.validation = ValidationMode::raw_transform;
      } else {
        throw std::invalid_argument("--validation must be generated or raw-transform");
      }
    } else if (option == "--scale") {
      cli.scale = std::stof(value);
    } else if (option == "--offset") {
      cli.offset = std::stof(value);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  if (cli.mode != "producer" && cli.mode != "receiver") {
    throw std::invalid_argument("--mode must be producer or receiver");
  }
  if (cli.images == 0 || cli.queue_depth == 0 || cli.batch_slots == 0 || cli.timeout_seconds <= 0) {
    throw std::invalid_argument("images, queue depth, batch slots, and timeout must be nonzero");
  }
  if (!std::isfinite(cli.scale) || !std::isfinite(cli.offset)) {
    throw std::invalid_argument("scale and offset must be finite");
  }
  if (cli.mode == "receiver" && (cli.connect.empty() || cli.local.empty())) {
    throw std::invalid_argument("receiver requires --connect and --local");
  }
  return cli;
}

void print_stats(const daqiri::ucx_gpu::TransportStats& stats, double seconds) {
  if (stats.active_nanoseconds != 0) {
    seconds = static_cast<double>(stats.active_nanoseconds) / 1e9;
  }
  const double gib_per_second =
      seconds == 0 ? 0 : static_cast<double>(stats.bytes) * 8.0 / seconds / 1e9;
  std::cout << "generated=" << stats.generated << " admitted=" << stats.admitted
            << " dropped_no_connection=" << stats.dropped_no_connection
            << " dropped_no_credit=" << stats.dropped_no_credit
            << " send_completed=" << stats.send_completed << " send_failed=" << stats.send_failed
            << " outstanding=" << stats.outstanding
            << " delivery_unknown=" << stats.delivery_unknown << " delivered=" << stats.delivered
            << " released=" << stats.released << " sequence_gaps=" << stats.sequence_gaps
            << " validation_failures=" << stats.validation_failures << " bytes=" << stats.bytes
            << " elapsed_s=" << std::fixed << std::setprecision(6) << seconds
            << " payload_Gbit_s=" << gib_per_second << '\n';
}

int run_producer(const Cli& cli) {
  cudaError_t cuda_status = cudaSetDevice(cli.gpu_id);
  if (cuda_status != cudaSuccess) {
    throw std::runtime_error(std::string("cudaSetDevice: ") + cudaGetErrorString(cuda_status));
  }
  cudaStream_t stream = nullptr;
  cuda_status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
  if (cuda_status != cudaSuccess) {
    throw std::runtime_error(std::string("cudaStreamCreate: ") + cudaGetErrorString(cuda_status));
  }

  daqiri::ucx_gpu::ExternalBatchProducerOptions options;
  options.listen_endpoint = cli.listen;
  options.image_count = cli.images;
  options.batch_slot_count = cli.batch_slots;
  options.max_receiver_queue_depth = cli.queue_depth;
  options.gpu_id = cli.gpu_id;
  options.cpu_core = cli.cpu_core;
  options.memory_kind = cli.memory_kind;
  options.wait_for_credit = cli.wait_for_credit;
  options.timeout = std::chrono::seconds(cli.timeout_seconds);
  daqiri::ucx_gpu::ExternalBatchProducer producer(std::move(options));
  const auto begin = std::chrono::steady_clock::now();
  std::exception_ptr primary_error;
  try {
    producer.start();
    producer.wait_for_receiver();
    std::uint64_t next_sequence = 0;
    while (next_sequence < cli.images) {
      while (producer.poll_retired()) {
      }
      if (std::optional<std::string> error = producer.error()) {
        throw std::runtime_error(*error);
      }
      std::optional<daqiri::ucx_gpu::BatchLease> lease = producer.try_acquire();
      if (!lease) {
        std::this_thread::yield();
        continue;
      }
      const std::uint64_t remaining = cli.images - next_sequence;
      const std::uint64_t credit_limited =
          cli.wait_for_credit ? std::min<std::uint64_t>(remaining, cli.queue_depth) : remaining;
      const std::uint32_t image_count = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(credit_limited, daqiri::ucx_example::geometry::kImagesPerBatch));
      for (std::uint32_t image = 0; image < image_count; ++image) {
        auto* destination = static_cast<std::uint8_t*>(lease->device_data()) +
                            static_cast<std::size_t>(image) * daqiri::ucx_gpu::kImageBytes;
        cuda_status = daqiri::ucx_gpu::fill_image_async(destination, next_sequence + image, stream);
        if (cuda_status != cudaSuccess) {
          throw std::runtime_error(std::string("fill_image_async: ") +
                                   cudaGetErrorString(cuda_status));
        }
      }
      producer.submit_after(std::move(*lease), next_sequence, image_count, stream);
      next_sequence += image_count;
    }
    cuda_status = cudaStreamSynchronize(stream);
    if (cuda_status != cudaSuccess) {
      throw std::runtime_error(std::string("cudaStreamSynchronize: ") +
                               cudaGetErrorString(cuda_status));
    }
    producer.finish_input(cli.images);
  } catch (...) {
    primary_error = std::current_exception();
    cudaStreamSynchronize(stream);
  }
  producer.acknowledge_local_quiescence();
  producer.close();
  const std::optional<std::string> producer_error = producer.error();
  const auto stats = producer.stats().transport;
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  cudaStreamDestroy(stream);
  if (primary_error) {
    std::rethrow_exception(primary_error);
  }
  if (producer_error) {
    throw std::runtime_error("producer close failed: " + *producer_error);
  }
  print_stats(stats, seconds);
  return 0;
}

int run_receiver(const Cli& cli) {
  cudaError_t cuda_status = cudaSetDevice(cli.gpu_id);
  if (cuda_status != cudaSuccess) {
    throw std::runtime_error(std::string("cudaSetDevice: ") + cudaGetErrorString(cuda_status));
  }
  cudaStream_t stream = nullptr;
  daqiri::ucx_gpu::ValidationResult* device_result = nullptr;
  cuda_status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
  if (cuda_status != cudaSuccess) {
    throw std::runtime_error(std::string("cudaStreamCreate: ") + cudaGetErrorString(cuda_status));
  }
  cuda_status = cudaMalloc(&device_result, sizeof(*device_result));
  if (cuda_status != cudaSuccess) {
    cudaStreamDestroy(stream);
    throw std::runtime_error(std::string("cudaMalloc(validation result): ") +
                             cudaGetErrorString(cuda_status));
  }
  daqiri::ucx_gpu::ValidationResult host_result{0, std::numeric_limits<unsigned long long>::max(),
                                                0, 0};
  cuda_status =
      cudaMemcpy(device_result, &host_result, sizeof(host_result), cudaMemcpyHostToDevice);
  if (cuda_status != cudaSuccess) {
    cudaFree(device_result);
    cudaStreamDestroy(stream);
    throw std::runtime_error(std::string("initialize validation result: ") +
                             cudaGetErrorString(cuda_status));
  }

  daqiri::ucx_gpu::ReceiverOptions options;
  options.server_endpoint = cli.connect;
  options.local_endpoint = cli.local;
  options.image_count = cli.images;
  options.queue_depth = cli.queue_depth;
  options.gpu_id = cli.gpu_id;
  options.cpu_core = cli.cpu_core;
  options.memory_kind = cli.memory_kind;
  options.timeout = std::chrono::seconds(cli.timeout_seconds);
  daqiri::ucx_gpu::Receiver receiver(std::move(options));
  const auto begin = std::chrono::steady_clock::now();
  std::exception_ptr primary_error;
  try {
    receiver.start();
    auto activity_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(cli.timeout_seconds);
    while (true) {
      auto result = receiver.receive(std::chrono::milliseconds(100));
      if (result.status == daqiri::ucx_gpu::ReceiveStatus::timeout) {
        if (std::chrono::steady_clock::now() > activity_deadline) {
          throw std::runtime_error("timed out waiting for DATA or EOS");
        }
        continue;
      }
      if (result.status == daqiri::ucx_gpu::ReceiveStatus::failed) {
        throw std::runtime_error(result.error);
      }
      if (result.status == daqiri::ucx_gpu::ReceiveStatus::end_of_stream) {
        break;
      }
      activity_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(cli.timeout_seconds);
      auto& image = *result.image;
      if (cli.validation == ValidationMode::generated) {
        cuda_status = daqiri::ucx_gpu::validate_image_async(image.device_data(), image.sequence(),
                                                            device_result, stream);
      } else {
        cuda_status = daqiri::ucx_gpu::validate_transformed_raw_image_async(
            image.device_data(), image.sequence(), cli.scale, cli.offset, device_result, stream);
      }
      if (cuda_status != cudaSuccess) {
        receiver.release_after(std::move(image), stream);
        throw std::runtime_error(std::string("CUDA validation: ") +
                                 cudaGetErrorString(cuda_status));
      }
      receiver.release_after(std::move(image), stream);
    }
  } catch (...) {
    primary_error = std::current_exception();
  }

  const cudaError_t drain_status = cudaStreamSynchronize(stream);
  receiver.close();
  const std::optional<std::string> receiver_error = receiver.error();
  cuda_status =
      drain_status == cudaSuccess
          ? cudaMemcpy(&host_result, device_result, sizeof(host_result), cudaMemcpyDeviceToHost)
          : drain_status;
  if (host_result.error_count != 0) {
    std::cerr << "validation failed errors=" << host_result.error_count
              << " sample_index=" << host_result.sample_bad_index
              << " sample_expected=" << host_result.sample_expected
              << " sample_actual=" << host_result.sample_actual << '\n';
  }
  auto stats = receiver.stats();
  stats.validation_failures = host_result.error_count;
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  cudaFree(device_result);
  cudaStreamDestroy(stream);
  if (primary_error) {
    std::rethrow_exception(primary_error);
  }
  if (receiver_error) {
    throw std::runtime_error("receiver close failed: " + *receiver_error);
  }
  if (cuda_status != cudaSuccess) {
    throw std::runtime_error(std::string("read validation result: ") +
                             cudaGetErrorString(cuda_status));
  }
  print_stats(stats, seconds);
  return host_result.error_count == 0 ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Cli cli = parse_cli(argc, argv);
    return cli.mode == "producer" ? run_producer(cli) : run_receiver(cli);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
