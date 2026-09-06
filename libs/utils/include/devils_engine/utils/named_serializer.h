#ifndef DEVILS_ENGINE_UTILS_FAST_SERIALIZER_H
#define DEVILS_ENGINE_UTILS_FAST_SERIALIZER_H

#include <array>
#include <format>
#include <iostream>
#include <reflect>
#include <stdexcept>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
//#include <alpaca/alpaca.h>

#include "core.h"
#include "string-utils.hpp"
#include "type_traits.h"

// для json лучше использовать glaze
// для lua имеет смысл написать десериализатор (позже)

namespace devils_engine {
namespace utils {

template <typename T>
glz::error_ctx to_json(const T& x, std::string& c) {
  return glz::write_json(x, c);
  // if (ec) {
  //   utils::error{}("Could not write json for struct '{}' (err code: {})", utils::type_name<T>(), static_cast<size_t>(ec.ec));
  // }
}

template <glz::opts O, typename T>
glz::error_ctx to_json(const T& x, std::string& c) {
  return glz::write<O>(x, c);
  // if (ec) {
  //   utils::error{}("Could not write json for struct '{}' (err code: {})", utils::type_name<T>(), static_cast<size_t>(ec.ec));
  // }
}

template <typename T>
std::expected<std::string, glz::error_ctx> to_json(const T& x) {
  return glz::write_json(x);
}

template <glz::opts O, typename T>
std::expected<std::string, glz::error_ctx> to_json(const T& x) {
  return glz::write<O>(x);
}

template <typename T>
auto from_json(T& x, const std::string& c) {
  return glz::read_json(x, c);
}

template <typename T>
bool to_lua_impl(const T& x, std::string& c);

template <typename T>
bool to_lua_value(const T& val, std::string& c) {
  using mem_type = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<mem_type, bool>) {
    c.append(val ? "true" : "false");
    return true;
  } else if constexpr (std::is_arithmetic_v<mem_type>) {
    c.append(std::format("{}", val));
    return true;
  } else if constexpr (std::is_same_v<mem_type, const char*> ||
                       std::is_same_v<mem_type, std::string> ||
                       std::is_same_v<mem_type, std::string_view>) {
    c.push_back('"');
    c.append(val);
    c.push_back('"');
    return true;
  } else if constexpr (is_container_v<mem_type>) {
    c.push_back('{');
    if (!val.empty()) {
      for (const auto& el : val) {
        to_lua_value(el, c);
        c.push_back(',');
      }
      c.pop_back(); // лишняя запятая по идее
    }
    c.push_back('}');
    return true;
  } else if constexpr (is_map_v<mem_type>) {
    c.push_back('{');
    if (!val.empty()) {
      for (const auto& [key, el] : val) {
        c.append(key);
        c.push_back('=');
        to_lua_value(el, c);
        c.push_back(',');
      }
      c.pop_back(); // лишняя запятая по идее
    }
    c.push_back('}');
    return true;
  } else if constexpr (std::is_pointer_v<mem_type>) {
    return false;
  } else {
    to_lua_impl(val, c);
    return true;
  }
}

template <typename T>
bool to_lua_impl(const T& x, std::string& c) {
  c.push_back('{');

  reflect::for_each([&](auto I) {
    using mem_type = std::remove_cvref_t<decltype(reflect::get<I>(x))>;
    const std::string_view name = reflect::member_name<I>(x);
    const auto& val = reflect::get<I>(x);
    c.append(name);
    c.push_back('=');
    const bool ret = to_lua_value(val, c);
    if (!ret) {
      utils::error{}("Could not serialize value of type '{}' ({})", utils::type_name<mem_type>(), name);
    }
    c.push_back(',');
  },
                    x);

  if (c.back() != '{') {
    c.pop_back();
  }
  c.push_back('}');
  return true;
}

template <typename T>
size_t to_lua(const T& x, std::string& c) {
  to_lua_impl(x, c);
  c = "return " + c;
  return c.size();
}

// from_lua: по идее надо со стаком по скобкам пройтись - идем слева до первой скобки, затем справа, это таблица
// нет идея такая себе, нужно идти со стаком последовательно + придется делать стак чтобы найти все 'end'
// рекурсивно проваливаемся ниже, когда парсим новую строку, проходим по запятым и нам может попастся:
// *название*=булеан,*название*=число,*название*="строка",*название*={таблица}, + теоретически может попастся
// функция, но ее по идее нужно найти и игнорировать
} // namespace utils
} // namespace devils_engine

#endif
