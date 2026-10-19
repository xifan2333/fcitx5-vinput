#pragma once

#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <cctype>
#include <concepts>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <spdlog/sinks/stderr_color_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <utility>

namespace vinput::log {

inline spdlog::level::level_enum
ParseLogLevel(std::string_view level_str,
              spdlog::level::level_enum fallback = spdlog::level::info) {
  if (level_str.empty()) {
    return fallback;
  }
  std::string lower;
  lower.reserve(level_str.size());
  for (char c : level_str) {
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
      return std::string(env_pattern);
    }
  }
  return std::string(kDefaultLogPattern);
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

inline void SetLogPattern(std::string pattern) {
  GetLogger()->set_pattern(std::move(pattern));
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

struct FormatWithLocation {
  std::string_view fmt;
  std::source_location loc;

  template <typename S>
    requires std::convertible_to<S, std::string_view>
  constexpr FormatWithLocation(const S& s, std::source_location l = std::source_location::current())
      : fmt(s), loc(l) {}
};

template <typename... Args> inline void Trace(FormatWithLocation f, Args&&... args) {
  auto& logger = GetLogger();
  if (!logger->should_log(spdlog::level::trace)) {
    return;
  }
  spdlog::source_loc sloc{f.loc.file_name(), static_cast<int>(f.loc.line()), f.loc.function_name()};
  logger->log(sloc, spdlog::level::trace, spdlog::fmt_lib::runtime(f.fmt),
              std::forward<Args>(args)...);
}

template <typename... Args> inline void Debug(FormatWithLocation f, Args&&... args) {
  auto& logger = GetLogger();
  if (!logger->should_log(spdlog::level::debug)) {
    return;
  }
  spdlog::source_loc sloc{f.loc.file_name(), static_cast<int>(f.loc.line()), f.loc.function_name()};
  logger->log(sloc, spdlog::level::debug, spdlog::fmt_lib::runtime(f.fmt),
              std::forward<Args>(args)...);
}

template <typename... Args> inline void Info(FormatWithLocation f, Args&&... args) {
  auto& logger = GetLogger();
  if (!logger->should_log(spdlog::level::info)) {
    return;
  }
  spdlog::source_loc sloc{f.loc.file_name(), static_cast<int>(f.loc.line()), f.loc.function_name()};
  logger->log(sloc, spdlog::level::info, spdlog::fmt_lib::runtime(f.fmt),
              std::forward<Args>(args)...);
}

template <typename... Args> inline void Warn(FormatWithLocation f, Args&&... args) {
  auto& logger = GetLogger();
  if (!logger->should_log(spdlog::level::warn)) {
    return;
  }
  spdlog::source_loc sloc{f.loc.file_name(), static_cast<int>(f.loc.line()), f.loc.function_name()};
  logger->log(sloc, spdlog::level::warn, spdlog::fmt_lib::runtime(f.fmt),
              std::forward<Args>(args)...);
}

template <typename... Args> inline void Error(FormatWithLocation f, Args&&... args) {
  auto& logger = GetLogger();
  if (!logger->should_log(spdlog::level::err)) {
    return;
  }
  spdlog::source_loc sloc{f.loc.file_name(), static_cast<int>(f.loc.line()), f.loc.function_name()};
  logger->log(sloc, spdlog::level::err, spdlog::fmt_lib::runtime(f.fmt),
              std::forward<Args>(args)...);
}

template <typename... Args> inline void Critical(FormatWithLocation f, Args&&... args) {
  auto& logger = GetLogger();
  if (!logger->should_log(spdlog::level::critical)) {
    return;
  }
  spdlog::source_loc sloc{f.loc.file_name(), static_cast<int>(f.loc.line()), f.loc.function_name()};
  logger->log(sloc, spdlog::level::critical, spdlog::fmt_lib::runtime(f.fmt),
              std::forward<Args>(args)...);
}

} // namespace vinput::log
