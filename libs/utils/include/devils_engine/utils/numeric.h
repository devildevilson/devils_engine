#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace devils_engine::utils {
inline constexpr double epsilon = 0.000001;

template <typename T>
constexpr const T& max(const T& a) {
  return a;
}

template <typename T, typename... Ts>
constexpr const T& max(const T& a, const T& b, Ts&&... args) {
  const T& m = max(b, std::forward<Ts>(args)...);
  return a >= m ? a : m;
}

template <typename T>
constexpr T min(const T& a, const T& b) noexcept {
  return a < b ? a : b;
}

template <typename T>
constexpr const T& min(const T& a) {
  return a;
}

template <typename T, typename... Ts>
constexpr const T& min(const T& a, const T& b, Ts&&... args) {
  static_assert((std::is_same_v<T, std::remove_cvref_t<Ts>> && ...));
  // The binary min returns by value. Recursing through that overload
  // would return a reference to a destroyed temporary at the bottom of the recursion.
  const T* selected = &a;
  const auto consider = [&](const T& value) {
    if (value < *selected) selected = &value;
  };
  consider(b);
  (consider(args), ...);
  return *selected;
}

constexpr double floor(const double val) noexcept {
  const int64_t value = static_cast<int64_t>(val);
  return val < value ? value - 1 : value;
}

constexpr double ceil(const double val) noexcept {
  const int64_t value = static_cast<int64_t>(val);
  return val > value ? value + 1 : value;
}

constexpr double fract(const double val) noexcept {
  const int64_t integral = static_cast<int64_t>(val);
  return val - integral;
}

constexpr double sign_fract(const double val) noexcept {
  const int64_t integral = static_cast<int64_t>(floor(val));
  return val > 0.0 ? val - integral : integral - val;
}

constexpr double abs(const double val) noexcept {
  const uint64_t mask = ~(static_cast<uint64_t>(INT64_MIN));
  return std::bit_cast<double>(std::bit_cast<uint64_t>(val) & mask);
}

constexpr double round(const double val) noexcept {
  return double(val >= 0.0 ? int64_t(val + 0.5) : int64_t(val - 0.5));
}

constexpr double sign(const double val) noexcept {
  return val >= 0.0 ? 1.0 : -1.0;
}

constexpr size_t align_to(const size_t size, const size_t alignment) noexcept {
  return (size + alignment - 1) / alignment * alignment;
}

template <typename T>
constexpr size_t count_significant(T v) {
  if constexpr (std::is_enum_v<T>) {
    return count_significant(static_cast<int64_t>(v));
  } else {
    size_t i = 0;
    for (; v != 0; v >>= 1, ++i) {
    }
    return i;
  }
}

// https://stackoverflow.com/questions/466204/rounding-up-to-next-power-of-2
constexpr uint32_t next_power_of_2(uint32_t v) {
  --v;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  ++v;

  return v;
}

static_assert(count_significant(1) == 1);
static_assert(count_significant(2) == 2);
static_assert(count_significant(3) == 2);
static_assert(count_significant(4) == 3);

} // namespace devils_engine::utils
