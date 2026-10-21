#pragma once

#include <string>
#include <string_view>
#include <tl/expected.hpp>
#include <utility>

namespace vinput {

// Type alias forwarding to tl::expected.
// Designed for a drop-in 1:1 replacement with std::expected in C++23.
template <typename T, typename E = std::string> using Expected = tl::expected<T, E>;

template <typename E = std::string> using Unexpected = tl::unexpected<E>;

// Helper to construct unexpected error types
template <typename E> [[nodiscard]] inline auto MakeUnexpected(E&& error) {
  return tl::make_unexpected(std::forward<E>(error));
}

// Convenient helper for standard string error
[[nodiscard]] inline auto UnexpectedError(std::string message) {
  return tl::make_unexpected(std::move(message));
}

[[nodiscard]] inline auto UnexpectedError(std::string_view message) {
  return tl::make_unexpected(std::string(message));
}

[[nodiscard]] inline auto UnexpectedError(const char* message) {
  return tl::make_unexpected(std::string(message));
}

} // namespace vinput
