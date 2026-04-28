/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <string>
#include <utility>

#include <spdlog/fmt/fmt.h>

namespace daqiri {

enum class LogSeverity : int {
  TRACE = 0,
  DEBUG = 1,
  INFO = 2,
  WARN = 3,
  ERROR = 4,
  CRITICAL = 5,
  OFF = 6
};

void set_log_level(LogSeverity level);
LogSeverity get_log_level();
LogSeverity log_level_from_string(const std::string &level_str);

void log_formatted_message(LogSeverity level, const char *file, int line,
                           const std::string &message);

inline void log_message(LogSeverity level, const char *file, int line,
                        const char *message) {
  log_formatted_message(level, file, line, message);
}

inline void log_message(LogSeverity level, const char *file, int line,
                        const std::string &message) {
  log_formatted_message(level, file, line, message);
}

template <typename... Args>
inline void log_message(LogSeverity level, const char *file, int line,
                        fmt::format_string<Args...> fmt, Args &&...args) {
  log_formatted_message(level, file, line,
                        fmt::format(fmt, std::forward<Args>(args)...));
}

} // namespace daqiri

#define DAQIRI_LOG_TRACE(...)                                                  \
  ::daqiri::log_message(::daqiri::LogSeverity::TRACE, __FILE__, __LINE__,      \
                        __VA_ARGS__)
#define DAQIRI_LOG_DEBUG(...)                                                  \
  ::daqiri::log_message(::daqiri::LogSeverity::DEBUG, __FILE__, __LINE__,      \
                        __VA_ARGS__)
#define DAQIRI_LOG_INFO(...)                                                   \
  ::daqiri::log_message(::daqiri::LogSeverity::INFO, __FILE__, __LINE__,       \
                        __VA_ARGS__)
#define DAQIRI_LOG_WARN(...)                                                   \
  ::daqiri::log_message(::daqiri::LogSeverity::WARN, __FILE__, __LINE__,       \
                        __VA_ARGS__)
#define DAQIRI_LOG_ERROR(...)                                                  \
  ::daqiri::log_message(::daqiri::LogSeverity::ERROR, __FILE__, __LINE__,      \
                        __VA_ARGS__)
#define DAQIRI_LOG_CRITICAL(...)                                               \
  ::daqiri::log_message(::daqiri::LogSeverity::CRITICAL, __FILE__, __LINE__,   \
                        __VA_ARGS__)
