#ifndef DEVILS_ENGINE_UTILS_PATTERNS_H
#define DEVILS_ENGINE_UTILS_PATTERNS_H

#include <string_view>
#include <tuple>

namespace devils_engine {
namespace utils {
bool is_number(const std::string_view& str);
bool is_ip_address(const std::string_view &str);
bool is_default_valid_name(const std::string_view& str);
bool is_valid_loc_key(const std::string_view& str);

std::tuple<std::string_view, std::string_view> pattern_split(const std::string_view& str, const char* pattern);
}
}

#endif