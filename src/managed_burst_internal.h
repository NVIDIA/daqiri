/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

#include <daqiri/types.h>

namespace daqiri {

void managed_burst_runtime_start();
void managed_burst_runtime_shutdown();
uint64_t managed_burst_runtime_generation() noexcept;
bool managed_tx_send_consumed(Status status) noexcept;
void managed_rx_release(BurstParams* burst, bool connection_completion) noexcept;

}  // namespace daqiri
