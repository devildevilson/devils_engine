#ifndef DEVILS_ENGINE_UTILS_CORE_H
#define DEVILS_ENGINE_UTILS_CORE_H

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>

#include "type_traits.h"

namespace devils_engine {
namespace utils {
inline constexpr double epsilon = 0.000001;

constexpr std::string_view make_sane_file_name(const std::string_view& str) {
  const size_t slash1_index = str.rfind("/");
  if (slash1_index == std::string_view::npos) {
    return str;
  }

  const auto str2 = str.substr(0, slash1_index);
  const size_t slash2_index = str2.rfind("/");
  return slash2_index == std::string_view::npos ? str.substr(slash1_index + 1) : str.substr(slash2_index + 1);
}

struct error {
  std::source_location location;

  explicit constexpr error(const std::source_location location = std::source_location::current()) noexcept : location(location) {}

  template <typename... Args>
  [[noreturn]] void operator()(const std::format_string<Args...>& format, Args&&... args) const {
    const std::string message = std::format(format, std::forward<Args>(args)...);
    const std::string full_message = std::format(
      "{}:{}: {}: {}",
      make_sane_file_name(location.file_name()),
      location.line(),
      location.function_name(),
      message);
    spdlog::error("{}", full_message);
    throw std::runtime_error(full_message);
  }
};

template <typename... Args>
constexpr void info(const std::format_string<Args...>& format, Args&&... args) {
  spdlog::info(format, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void warn(const std::format_string<Args...>& format, Args&&... args) {
  spdlog::warn(format, std::forward<Args>(args)...);
}

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
constexpr const T& min(const T& a) {
  return a;
}

template <typename T, typename... Ts>
constexpr const T& min(const T& a, const T& b, Ts&&... args) {
  const T& m = max(b, std::forward<Ts>(args)...);
  return a < m ? a : m;
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

inline void print_detail() {}

template <typename Arg, typename... Args>
void print_detail(Arg&& arg, Args&&... args) {
  //std::cout << " " << arg;
  std::print(" {}", std::forward<Arg>(arg));
  print_detail(std::forward<Args>(args)...);
}

inline void print() {}

template <typename Arg, typename... Args>
void print(Arg&& arg, Args&&... args) {
  //std::cout << arg;
  std::print("{}", std::forward<Arg>(arg));
  print_detail(std::forward<Args>(args)...);
}

template <typename... Args>
void println(Args&&... args) {
  print(std::forward<Args>(args)...);
  //std::cout << "\n";
  std::print("\n");
}

class tracer {
public:
  std::source_location location;

  tracer(std::source_location loc = std::source_location::current()) noexcept;
  ~tracer() noexcept;

  tracer(const tracer& copy) noexcept = delete;
  tracer(tracer&& move) noexcept = delete;
  tracer& operator=(const tracer& copy) noexcept = delete;
  tracer& operator=(tracer&& move) noexcept = delete;
};

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

// пригодятся юникод строки
std::string cast(const std::wstring& str) noexcept;
std::string cast(const std::u16string_view& str) noexcept;
std::string cast(const std::u32string_view& str) noexcept;
std::wstring cast(const std::string& str) noexcept;
std::u16string cast16(const std::string_view& str) noexcept;
std::u32string cast32(const std::string_view& str) noexcept;

// windows, unix, macos
std::string app_path() noexcept;
std::string project_folder() noexcept;
std::string cache_folder() noexcept;
// имеет смысл добавить получение каких то цифр к этому
std::string get_cpu_name() noexcept;

uint32_t crc32c(const uint8_t* data, const size_t len) noexcept;
uint32_t crc32c(const std::span<const uint8_t>& data) noexcept;
uint32_t crc32c(const std::span<const char>& data) noexcept;
uint32_t crc32c(const std::span<uint8_t>& data) noexcept;
uint32_t crc32c(const std::span<char>& data) noexcept;
uint32_t crc32c(const std::string_view& data) noexcept;
} // namespace utils
} // namespace devils_engine

#endif
