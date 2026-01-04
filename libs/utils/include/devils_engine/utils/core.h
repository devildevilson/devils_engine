#ifndef DEVILS_ENGINE_UTILS_CORE_H
#define DEVILS_ENGINE_UTILS_CORE_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <stdexcept>
#include <iostream>
#include <source_location>
#include <format>
#include <print>
//#include <fmt/base.h>
#include <spdlog/spdlog.h>
#include "type_traits.h"

namespace devils_engine {
namespace utils {
#   ifdef _MSC_VER
#     define utils_pretty_function __FUNCSIG__
#   else
#     define utils_pretty_function __PRETTY_FUNCTION__
#   endif

#   define DEVILS_ENGINE_EPSILON 0.000001

constexpr std::string_view make_sane_file_name(const std::string_view &str) {
  const size_t slash1_index = str.rfind("/");
  if (slash1_index == std::string_view::npos) return str;

  const auto str2 = str.substr(0, slash1_index);
  const size_t slash2_index = str2.rfind("/");
  return slash2_index == std::string_view::npos ? str.substr(slash1_index + 1) : str.substr(slash2_index + 1);
}

struct error {
  std::source_location loc;
  explicit constexpr error(std::source_location l = std::source_location::current()) noexcept : loc(l) {}
  template <typename... Args>
  constexpr void operator()(const std::format_string<Args...>& format, Args&&... args) const {
    spdlog::error(format, std::forward<Args>(args)...);
    throw std::runtime_error(std::format("{}:{}: {}: Got runtime error", make_sane_file_name(loc.file_name()), loc.line(), loc.function_name()));
  }
};

//template <typename... Args>
//void error(const std::format_string<Args...> &format, Args&&... args, std::source_location loc = std::source_location::current()) {
//  spdlog::error(format, std::forward<Args>(args)...);
//  throw std::runtime_error(std::format("{}:{}: {}: Got runtime error", make_sane_file_name(loc.file_name()), loc.line(), loc.function_name()));
//}

template <typename... Args>
constexpr void info(const std::format_string<Args...> &format, Args&&... args) {
  spdlog::info(format, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void warn(const std::format_string<Args...> &format, Args&&... args) {
  spdlog::warn(format, std::forward<Args>(args)...);
}

template <typename... Args>
void assertf(const bool cond, const std::format_string<Args...> &format, Args&&... args) {
  if (!cond) {
    spdlog::error(format, std::forward<Args>(args)...);
    throw std::runtime_error("Assertion failed");
  }
}

template <typename... Args>
void assertf_failed_detail(
  const std::string_view &cond_str,
  const std::string_view &file_name,
  const std::string_view &func_name,
  const size_t line,
  const std::format_string<Args...> &format,
  Args&&... args
) {
  spdlog::error("{}:{}: {}: Assertion `{}` failed", make_sane_file_name(file_name), line, func_name, cond_str);
  spdlog::info(format, std::forward<Args>(args)...);
  throw std::runtime_error("Assertion failed");
}

void assert_failed_detail(
  const std::string_view &cond_str,
  const std::string_view &file_name,
  const std::string_view &func_name,
  const size_t line
);

void assert_failed_detail(
  const std::string_view &cond_str,
  const std::string_view &file_name,
  const std::string_view &func_name,
  const size_t line,
  const std::string_view &comm
);

#   define utils_assertf(cond, fmt, ...) {if (!(cond)) {devils_engine::utils::assertf_failed_detail(#cond, __FILE__, utils_pretty_function, __LINE__, fmt, __VA_ARGS__);}}
#   define utils_assert(cond) {if (!(cond)) {devils_engine::utils::assert_failed_detail(#cond, __FILE__, utils_pretty_function, __LINE__);}}
#   define utils_assertc(cond, comm) {if (!(cond)) {devils_engine::utils::assert_failed_detail(#cond, __FILE__, utils_pretty_function, __LINE__, comm);}}

template <typename T>
constexpr const T& max(const T &a) { return a; }

template <typename T, typename... Ts>
constexpr const T& max(const T &a, const T &b, Ts&&... args) {
  const T& m = max(b, std::forward<Ts>(args)...);
  return a >= m ? a : m;
}

template <typename T>
constexpr const T& min(const T &a) { return a; }

template <typename T, typename... Ts>
constexpr const T& min(const T &a, const T &b, Ts&&... args) {
  const T& m = max(b, std::forward<Ts>(args)...);
  return a < m ? a : m;
}

constexpr size_t align_to(const size_t size, const size_t alignment) {
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
  std::source_location l;

  tracer(std::source_location loc = std::source_location::current()) noexcept;
  ~tracer() noexcept;

  tracer(const tracer &copy) noexcept = delete;
  tracer(tracer &&move) noexcept = delete;
  tracer & operator=(const tracer &copy) noexcept = delete;
  tracer & operator=(tracer &&move) noexcept = delete;
};

#ifdef DEBUGTRACE
#define DS_TRACEOBJ devils_engine::utils::tracer __trace_obj;
#else
#define DS_TRACEOBJ
#endif

#define FORCE_DS_TRACEOBJ devils_engine::utils::tracer __trace_obj;

template <typename T>
constexpr size_t count_significant(T v) {
  if constexpr (std::is_enum_v<T>) { return count_significant(static_cast<int64_t>(v)); }
  else {
    size_t i = 0;
    for (; v != 0; v >>= 1, ++i) {}
    return i;
  }
}

// https://stackoverflow.com/questions/466204/rounding-up-to-next-power-of-2
constexpr uint32_t next_power_of_2(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;

  return v;
}

static_assert(count_significant(1) == 1);
static_assert(count_significant(2) == 2);
static_assert(count_significant(3) == 2);
static_assert(count_significant(4) == 3);

// пригодятся юникод строки
std::string cast(const std::wstring &str) noexcept;
std::string cast(const std::u16string_view &str) noexcept;
std::string cast(const std::u32string_view &str) noexcept;
std::wstring cast(const std::string &str) noexcept;
std::u16string cast16(const std::string_view &str) noexcept;
std::u32string cast32(const std::string_view &str) noexcept;

// windows, unix, macos
std::string app_path() noexcept;
std::string project_folder() noexcept;
std::string cache_folder() noexcept;

uint32_t crc32c(const uint8_t* data, const size_t len) noexcept;
uint32_t crc32c(const std::span<const uint8_t> &data) noexcept;
uint32_t crc32c(const std::span<const char> &data) noexcept;
uint32_t crc32c(const std::span<uint8_t> &data) noexcept;
uint32_t crc32c(const std::span<char> &data) noexcept;
uint32_t crc32c(const std::string_view &data) noexcept;
}
}

#endif
