#pragma once

#include <cstddef>
#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace devils_engine::utils {
// Отдельная идентичность критического отказа нужна на C++/Lua границе. Обычная Lua-ошибка
// может быть обработана pcall, но отказ инварианта движка не становится успешным выполнением.
class critical_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

constexpr std::string_view make_sane_file_name(const std::string_view& str) {
  const size_t slash1_index = str.rfind("/");
  if (slash1_index == std::string_view::npos) {
    return str;
  }

  const auto str2 = str.substr(0, slash1_index);
  const size_t slash2_index = str2.rfind("/");
  return slash2_index == std::string_view::npos ? str.substr(slash1_index + 1) : str.substr(slash2_index + 1);
}

[[noreturn]] void raise_critical_error(std::source_location location, const std::string& message);

struct error {
  std::source_location location;

  explicit constexpr error(const std::source_location location = std::source_location::current()) noexcept : location(location) {}

  template <typename... Args>
  [[noreturn]] constexpr void operator()(const std::format_string<Args...>& format,
                                         Args&&... args) const {
    if consteval {
      // A fatal invariant cannot produce a value during constant evaluation. Throwing here is
      // deliberately not a recoverable exception path: it makes the surrounding constexpr
      // expression ill-formed and points at the utils::error call site.
      throw "devils_engine::utils::error during constant evaluation";
    } else {
      const std::string message = std::format(format, std::forward<Args>(args)...);
      raise_critical_error(location, message);
    }
  }
};

} // namespace devils_engine::utils
