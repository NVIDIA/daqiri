/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <cuda_runtime_api.h>

#include <daqiri/daqiri.h>

namespace {

daqiri::ResourceOpResult wait_for(daqiri::ResourceOpId wanted) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    daqiri::ResourceOpResult result;
    const daqiri::Status status = daqiri::poll_resource_op(&result);
    if (status == daqiri::Status::NOT_READY) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (status != daqiri::Status::SUCCESS) {
      throw std::runtime_error("poll_resource_op failed");
    }
    if (result.op_id_ == wanted) {
      return result;
    }
  }
  throw std::runtime_error("runtime resource operation timed out");
}

void require_success(daqiri::Status accepted, daqiri::ResourceOpId op_id) {
  if (accepted != daqiri::Status::SUCCESS) {
    throw std::runtime_error("runtime resource operation was rejected");
  }
  const auto result = wait_for(op_id);
  if (result.status_ != daqiri::Status::SUCCESS) {
    throw std::runtime_error("runtime resource operation failed");
  }
}

int unused_queue_id(const std::vector<daqiri::RxQueueConfig>& queues) {
  int id = 0;
  for (;;) {
    bool used = false;
    for (const auto& queue : queues) {
      used |= queue.common_.id_ == id;
    }
    if (!used) {
      return id;
    }
    ++id;
  }
}

int unused_queue_id(const std::vector<daqiri::TxQueueConfig>& queues) {
  int id = 0;
  for (;;) {
    bool used = false;
    for (const auto& queue : queues) {
      used |= queue.common_.id_ == id;
    }
    if (!used) {
      return id;
    }
    ++id;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: daqiri_example_dynamic_resource <ibverbs-config.yaml>\n";
    return 2;
  }

  daqiri::NetworkConfig config;
  if (daqiri::parse_network_config(argv[1], config) != daqiri::Status::SUCCESS ||
      config.ifs_.empty() || config.ifs_[0].rx_.queues_.empty() ||
      config.ifs_[0].tx_.queues_.empty()) {
    std::cerr << "The example requires an ibverbs config with an RX and TX queue\n";
    return 2;
  }

  try {
    auto rx_queue = config.ifs_[0].rx_.queues_.front();
    auto tx_queue = config.ifs_[0].tx_.queues_.front();
    rx_queue.common_.id_ = unused_queue_id(config.ifs_[0].rx_.queues_);
    tx_queue.common_.id_ = unused_queue_id(config.ifs_[0].tx_.queues_);
    rx_queue.common_.name_ = "runtime_rx";
    tx_queue.common_.name_ = "runtime_tx";

    auto rx_mr = config.mrs_.at(rx_queue.common_.mrs_.front());
    auto tx_mr = config.mrs_.at(tx_queue.common_.mrs_.front());
    rx_mr.name_ = "RUNTIME_RX_MR";
    tx_mr.name_ = "RUNTIME_TX_MR";
    rx_mr.owned_ = true;
    tx_mr.owned_ = true;
    rx_queue.common_.mrs_[0] = rx_mr.name_;
    tx_queue.common_.mrs_[0] = tx_mr.name_;

    if (daqiri::daqiri_init(config) != daqiri::Status::SUCCESS) {
      throw std::runtime_error("daqiri_init failed");
    }

    daqiri::ResourceOpId op = 0;
    const auto finish = [&](daqiri::Status status) { require_success(status, op); };
    finish(daqiri::add_memory_region_async(rx_mr, &op));
    finish(daqiri::add_memory_region_async(tx_mr, &op));
    finish(daqiri::add_rx_queue_async(0, rx_queue, &op));
    finish(daqiri::add_tx_queue_async(0, tx_queue, &op));

    daqiri::BurstParams* tx = daqiri::create_tx_burst_params();
    if (tx == nullptr) {
      throw std::runtime_error("could not allocate runtime TX metadata");
    }
    daqiri::set_header(tx, 0, static_cast<uint16_t>(tx_queue.common_.id_), 1, 1);
    if (daqiri::get_tx_packet_burst(tx) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(tx);
      throw std::runtime_error("could not allocate a runtime TX packet");
    }
    void* packet = daqiri::get_packet_ptr(tx, 0);
    const auto clear_status = tx_mr.kind_ == daqiri::MemoryKind::DEVICE
                                  ? cudaMemset(packet, 0, 64)
                                  : (std::memset(packet, 0, 64), cudaSuccess);
    if (clear_status != cudaSuccess) {
      daqiri::free_all_packets_and_burst_tx(tx);
      throw std::runtime_error("could not initialize a runtime TX packet");
    }
    daqiri::set_packet_lengths(tx, 0, {64});
    const daqiri::Status send_status = daqiri::send_tx_burst(tx);
    if (send_status != daqiri::Status::SUCCESS) {
      if (send_status != daqiri::Status::NO_SPACE_AVAILABLE) {
        daqiri::free_all_packets_and_burst_tx(tx);
      }
      throw std::runtime_error("could not submit a runtime TX packet");
    }

    finish(daqiri::delete_rx_queue_async(0, rx_queue.common_.id_, &op));
    finish(daqiri::delete_tx_queue_async(0, tx_queue.common_.id_, &op));
    finish(daqiri::delete_memory_region_async(rx_mr.name_, &op));
    finish(daqiri::delete_memory_region_async(tx_mr.name_, &op));

    daqiri::shutdown();
    std::cout << "Dynamic memory-region and RX/TX queue lifecycle passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    daqiri::shutdown();
    return 1;
  }
}
