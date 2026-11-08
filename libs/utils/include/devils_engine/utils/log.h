#pragma once

#include <utility>
#include <spdlog/spdlog.h>

namespace devils_engine::utils {
// `spdlog::format_string_t`, not `std::format_string`, and the difference is not
// cosmetic: spdlog picks `std::format_string<Args...>` only when the standard
// library advertises `__cpp_lib_format >= 202207L`, and falls back to a plain
// `std::string_view` otherwise. A wrapper which names the type itself therefore
// compiles on one standard library and not on another — GCC 16 reports 202304
// and accepts it, GCC 14 reports 202110 and does not. Deferring to spdlog's own
// alias means the wrapper agrees with whatever spdlog decided.
//
// The cost on a library taking the fallback is that the format string stops
// being checked at compile time; that is spdlog's choice for that library, not
// something this wrapper can improve.
template <typename... Args>
void info(spdlog::format_string_t<Args...> format, Args&&... args) {
  spdlog::info(std::move(format), std::forward<Args>(args)...);
}

template <typename... Args>
void warn(spdlog::format_string_t<Args...> format, Args&&... args) {
  spdlog::warn(std::move(format), std::forward<Args>(args)...);
}

} // namespace devils_engine::utils
