#include "locale-utils.hpp"

#include "core.h"

namespace devils_engine {
namespace utils {
locale::locale() : locale("en") {}
locale::locale(const std::string_view& code) : container(0) {
  if (code.size() > maximum_language_code_length) utils::error("Could not create locale class from language code '{}', code size too large ({} > {})", code, code.size(), maximum_language_code_length);
  auto mem = reinterpret_cast<char*>(&container);
  memcpy(mem, code.data(), code.size());
}

uint32_t locale::native() const noexcept { return container; }
std::string_view locale::code() const noexcept {
  auto mem = reinterpret_cast<const char*>(&container);
  size_t size = maximum_language_code_length;
  while (mem[size - 1] == '\0') { size -= 1; }
  return std::string_view(mem, size);
}
}
}