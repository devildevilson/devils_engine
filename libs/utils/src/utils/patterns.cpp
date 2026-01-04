#include "patterns.h"

#include <reflex/matcher.h>
#include <reflex/pattern.h>

namespace devils_engine {
namespace utils {
const auto num_pattern = reflex::Pattern("^(?:-(?:[1-9](?:\\d{0,2}(?:,\\d{3})+|\\d*))|(?:0|(?:[1-9](?:\\d{0,2}(?:,\\d{3})+|\\d*))))(?:.\\d+|)$");
const auto ip_addr_pattern = reflex::Pattern("^(((?!25?[6-9])[12]\\d|[1-9])?\\d\\.?\\b){4}$");
const auto valid_name_pattern = reflex::Pattern("^[A-Za-z0-9_]+$");

bool is_number(const std::string_view& str) {
  reflex::Matcher m(num_pattern, reflex::Input(str.data(), str.size()));
  return m.matches() != 0;
}

bool is_ip_address(const std::string_view& str) {
  reflex::Matcher m(ip_addr_pattern, reflex::Input(str.data(), str.size()));
  return m.matches() != 0;
}

bool is_default_valid_name(const std::string_view& str) {
  reflex::Matcher m(valid_name_pattern, reflex::Input(str.data(), str.size()));
  return m.matches() != 0;
}

bool is_valid_loc_key(const std::string_view& str) {
  return false;
}

std::tuple<std::string_view, std::string_view> pattern_split(const std::string_view& str, const char* pattern) {
  const auto local_pattern = reflex::Pattern(pattern);
  reflex::Matcher m(local_pattern, reflex::Input(str.data(), str.size()));
  const size_t place = m.first();
  return std::make_tuple(str.substr(0, place), str.substr(place));
}

}
}