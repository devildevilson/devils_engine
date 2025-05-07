#ifndef DEVILS_ENGINE_UTILS_LOCALE_H
#define DEVILS_ENGINE_UTILS_LOCALE_H

#include <cstdint>
#include <cstddef>
#include <string_view>

// поддерживаю локаль с помощью 2значных кодов языков (хотя можно и любые другие комбинации использовать)
// https://en.wikipedia.org/wiki/List_of_ISO_639_language_codes

namespace devils_engine {
namespace utils {
class locale {
public:
  using native_code_t = uint32_t;
  constexpr static const size_t maximum_language_code_length = sizeof(native_code_t);

  locale(); //  локаль по умолчанию 'en'
  locale(const std::string_view& code);
  locale(const locale& copy) noexcept = default;
  locale(locale&& move) noexcept = default;
  locale& operator=(const locale& copy) noexcept = default;
  locale& operator=(locale&& move) noexcept = default;
  bool operator==(const locale& other) const noexcept = default;

  native_code_t native() const noexcept;
  std::string_view code() const noexcept;
private:
  native_code_t container;
};
}
}

namespace std {
template <>
struct hash<devils_engine::utils::locale> {
  size_t operator()(const devils_engine::utils::locale& l) const noexcept {
    return std::hash<decltype(l.native())>{}(l.native());
  }
};
}

#endif