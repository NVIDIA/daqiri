#include "src/logging.hpp"

#include <algorithm>
#include <cctype>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace daqiri {
namespace {
std::atomic<LogSeverity> g_level{LogSeverity::INFO};

const char* to_string(LogSeverity level) {
  switch (level) {
    case LogSeverity::TRACE: return "TRACE";
    case LogSeverity::DEBUG: return "DEBUG";
    case LogSeverity::INFO: return "INFO";
    case LogSeverity::WARN: return "WARN";
    case LogSeverity::ERROR: return "ERROR";
    case LogSeverity::CRITICAL: return "CRITICAL";
    case LogSeverity::OFF: return "OFF";
    default: return "INFO";
  }
}

spdlog::level::level_enum to_spdlog_level(LogSeverity level) {
  switch (level) {
    case LogSeverity::TRACE: return spdlog::level::trace;
    case LogSeverity::DEBUG: return spdlog::level::debug;
    case LogSeverity::INFO: return spdlog::level::info;
    case LogSeverity::WARN: return spdlog::level::warn;
    case LogSeverity::ERROR: return spdlog::level::err;
    case LogSeverity::CRITICAL: return spdlog::level::critical;
    case LogSeverity::OFF: return spdlog::level::off;
    default: return spdlog::level::info;
  }
}

std::shared_ptr<spdlog::logger> get_logger() {
  static std::shared_ptr<spdlog::logger> logger = []() {
    auto existing = spdlog::get("daqiri");
    if (existing) { return existing; }

    auto created = spdlog::stderr_color_mt("daqiri");
    created->set_pattern("%v");
    created->set_level(to_spdlog_level(g_level.load(std::memory_order_relaxed)));
    return created;
  }();
  return logger;
}
}  // namespace

void set_log_level(LogSeverity level) {
  g_level.store(level, std::memory_order_relaxed);
  get_logger()->set_level(to_spdlog_level(level));
}

LogSeverity get_log_level() {
  return g_level.load(std::memory_order_relaxed);
}

LogSeverity log_level_from_string(const std::string& level_str) {
  std::string s = level_str;
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  if (s == "trace") return LogSeverity::TRACE;
  if (s == "debug") return LogSeverity::DEBUG;
  if (s == "info") return LogSeverity::INFO;
  if (s == "warn" || s == "warning") return LogSeverity::WARN;
  if (s == "error") return LogSeverity::ERROR;
  if (s == "critical") return LogSeverity::CRITICAL;
  if (s == "off") return LogSeverity::OFF;
  return LogSeverity::INFO;
}

void log_formatted_message(
    LogSeverity level, const char* file, int line, const std::string& message) {
  if (static_cast<int>(level) < static_cast<int>(get_log_level())) { return; }
  get_logger()->log(
      to_spdlog_level(level), "[{}] {}:{}: {}", to_string(level), file, line, message);
}

}  // namespace daqiri
