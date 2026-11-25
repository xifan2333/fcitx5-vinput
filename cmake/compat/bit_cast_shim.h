#pragma once

#include <bit>
#include <type_traits>
#include <version>

// Scoped C++20 std::bit_cast shim for libstdc++-10 (Debian 11 / Kylin V10 SP1),
// injected only for untouched 3rdparty headers such as glaze.
#if !defined(__cpp_lib_bit_cast)
namespace std {

template <class To, class From>
constexpr enable_if_t<
    sizeof(To) == sizeof(From) && is_trivially_copyable_v<From> && is_trivially_copyable_v<To>, To>
bit_cast(const From& src) noexcept {
  return __builtin_bit_cast(To, src);
}

} // namespace std
#endif
