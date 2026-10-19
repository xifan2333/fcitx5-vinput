#pragma once

#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <cctype>
#include <cstdlib>
#include <memory>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace vinput::log {

inline spdlog::level::level_enum
ParseLogLevel(std::string_view level_str,
              spdlog::level::level_enum fallback = spdlog::level::info) {
  if (level_str.empty()) {
    return fallback;
  }
  std::string lower;
  lower.reserve(level_str.size());
  for (const char c : level_str) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "trace") {
    return spdlog::level::trace;
  }
  if (lower == "debug") {
    return spdlog::level::debug;
  }
  if (lower == "info") {
    return spdlog::level::info;
  }
  if (lower == "warn" || lower == "warning") {
    return spdlog::level::warn;
  }
  if (lower == "error" || lower == "err") {
    return spdlog::level::err;
  }
  if (lower == "critical" || lower == "fatal") {
    return spdlog::level::critical;
  }
  if (lower == "off") {
    return spdlog::level::off;
  }
  return fallback;
}

inline spdlog::level::level_enum DetermineInitialLogLevel() {
  if (const char* env_level = std::getenv("VINPUT_LOG_LEVEL")) {
    return ParseLogLevel(env_level, spdlog::level::info);
  }
  return spdlog::level::info;
}

inline constexpr std::string_view kDefaultLogPattern =
    "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%s:%#] %v";

inline std::string DetermineInitialLogPattern() {
  if (const char* env_pattern = std::getenv("VINPUT_LOG_PATTERN")) {
    if (env_pattern[0] != '\0') {
      return env_pattern;
    }
  }
  return std::string{kDefaultLogPattern};
}

inline std::shared_ptr<spdlog::logger> CreateDefaultLogger() {
  auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  const char* no_color = std::getenv("NO_COLOR");
  const char* term = std::getenv("TERM");
  if (no_color != nullptr || (term != nullptr && std::string_view(term) == "dumb")) {
    sink->set_color_mode(spdlog::color_mode::never);
  } else {
    sink->set_color_mode(spdlog::color_mode::automatic);
  }

  auto logger = std::make_shared<spdlog::logger>("vinput", sink);
  logger->set_pattern(DetermineInitialLogPattern());
  logger->set_level(DetermineInitialLogLevel());
  return logger;
}

inline std::shared_ptr<spdlog::logger>& GetLogger() {
  static std::shared_ptr<spdlog::logger> s_logger = CreateDefaultLogger();
  return s_logger;
}

inline void SetLogPattern(const std::string& pattern) {
  GetLogger()->set_pattern(pattern);
}

inline void SetLogLevel(spdlog::level::level_enum level) {
  GetLogger()->set_level(level);
}

inline void SetLogLevel(std::string_view level_str) {
  GetLogger()->set_level(ParseLogLevel(level_str, GetLogger()->level()));
}

inline spdlog::level::level_enum GetLogLevel() {
  return GetLogger()->level();
}

} // namespace vinput::log

#define VLOG_TRACE(...) SPDLOG_LOGGER_TRACE(::vinput::log::GetLogger(), __VA_ARGS__)
#define VLOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(::vinput::log::GetLogger(), __VA_ARGS__)
#define VLOG_INFO(...) SPDLOG_LOGGER_INFO(::vinput::log::GetLogger(), __VA_ARGS__)
#define VLOG_WARN(...) SPDLOG_LOGGER_WARN(::vinput::log::GetLogger(), __VA_ARGS__)
#define VLOG_ERROR(...) SPDLOG_LOGGER_ERROR(::vinput::log::GetLogger(), __VA_ARGS__)
#define VLOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(::vinput::log::GetLogger(), __VA_ARGS__)
