#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <reflect>
#include "devils_engine/bindings/lua_header.h"
#include "devils_engine/utils/core.h"

namespace devils_engine::bindings {
// These limits bound one conversion, not a project's total memory budget.
struct conversion_limits {
  size_t depth = 32;
  size_t values = 65536;
  size_t string_bytes = 1024 * 1024;
};
enum class data_table_kind { object,
                             array,
                             tuple };
void mark_data_table(lua_State* L, int index, data_table_kind kind);
std::optional<data_table_kind> data_table_type(lua_State* L, int index);
void push_data_null(lua_State* L);
bool is_data_null(lua_State* L, int index);
// Unsigned values above LUA_MAXINTEGER use an opaque, immutable userdata. No double rounding.
void push_data_uint64(lua_State* L, uint64_t value);
std::optional<uint64_t> data_uint64(lua_State* L, int index);

namespace data_detail {
struct stack_guard {
  lua_State* L;
  int top;
  explicit stack_guard(lua_State* state) : L(state), top(lua_gettop(state)) {}
  ~stack_guard() {
    lua_settop(L, top);
  }
};
inline void require(bool condition, const std::string& path, const char* message) {
  if (!condition) utils::error{}("Lua data '{}': {}", path, message);
}
struct budget {
  conversion_limits remaining;
  void enter(size_t depth, const std::string& path) {
    require(depth <= remaining.depth, path, "depth limit exceeded (cyclic data is unsupported)");
    require(remaining.values != 0, path, "value limit exceeded");
    --remaining.values;
  }
  void text(size_t size, const std::string& path) {
    require(size <= remaining.string_bytes, path, "string byte limit exceeded");
    remaining.string_bytes -= size;
  }
};
template <class>
struct optional_type : std::false_type {};
template <class T>
struct optional_type<std::optional<T>> : std::true_type {};
template <class>
struct sequence_type : std::false_type {};
template <class T, class A>
struct sequence_type<std::vector<T, A>> : std::true_type {};
template <class T, size_t N>
struct sequence_type<std::array<T, N>> : std::true_type {};
template <class>
struct map_type : std::false_type {};
template <class V, class C, class A>
struct map_type<std::map<std::string, V, C, A>> : std::true_type {};
template <class V, class H, class E, class A>
struct map_type<std::unordered_map<std::string, V, H, E, A>> : std::true_type {};
template <class>
inline constexpr bool unsupported = false;
inline std::string string_at(lua_State* L, int index, budget& limits, const std::string& path) {
  require(lua_type(L, index) == LUA_TSTRING, path, "expected string");
  size_t length = 0;
  const auto* text = lua_tolstring(L, index, &length);
  limits.text(length, path);
  return std::string(text, length);
}
inline size_t sequence_size(lua_State* L, int index, budget& limits, const std::string& path) {
  require(lua_type(L, index) == LUA_TTABLE, path, "expected sequence table");
  const auto kind = data_table_type(L, index);
  require(!kind || *kind != data_table_kind::object, path, "object table is not a sequence");
  const size_t count = lua_rawlen(L, index);
  require(count <= limits.remaining.values, path, "sequence value limit exceeded");
  size_t seen = 0;
  lua_pushnil(L);
  while (lua_next(L, index)) {
    require(lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 && uint64_t(lua_tointeger(L, -2)) <= count,
            path, "non-sequence key or sequence hole");
    ++seen;
    lua_pop(L, 1);
  }
  require(seen == count, path, "sequence contains holes");
  return count;
}
template <class T>
void push(lua_State*, const T&, budget&, const std::string&, size_t);
template <class T>
T read(lua_State*, int, budget&, const std::string&, size_t);

template <class T>
void push(lua_State* L, const T& value, budget& limits, const std::string& path, size_t depth) {
  limits.enter(depth, path);
  require(lua_checkstack(L, 6), path, "Lua stack cannot grow");
  if constexpr (std::is_same_v<T, bool>) {
    lua_pushboolean(L, value);
  } else if constexpr (std::is_enum_v<T>) {
    push(L, static_cast<std::underlying_type_t<T>>(value), limits, path, depth);
  } else if constexpr (std::is_integral_v<T>) {
    if constexpr (std::is_unsigned_v<T>)
      push_data_uint64(L, value);
    else
      lua_pushinteger(L, value);
  } else if constexpr (std::is_floating_point_v<T>) {
    require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<lua_Number>::max(), path, "non-finite or out-of-range number");
    lua_pushnumber(L, value);
  } else if constexpr (std::is_same_v<T, std::string>) {
    limits.text(value.size(), path);
    lua_pushlstring(L, value.data(), value.size());
  } else if constexpr (optional_type<T>::value) {
    if (value)
      push(L, *value, limits, path, depth + 1);
    else
      push_data_null(L);
  } else if constexpr (sequence_type<T>::value) {
    require(value.size() <= limits.remaining.values && std::in_range<int>(value.size()), path, "sequence value limit exceeded");
    lua_createtable(L, int(value.size()), 0);
    mark_data_table(L, -1, data_table_kind::array);
    size_t i = 0;
    for (const auto& element : value) {
      // vector<bool> yields proxy references.
      push<typename T::value_type>(L, element, limits, path + "[" + std::to_string(i + 1) + "]", depth + 1);
      lua_rawseti(L, -2, ++i);
    }
  } else if constexpr (map_type<T>::value) {
    require(value.size() <= limits.remaining.values, path, "map value limit exceeded");
    lua_createtable(L, 0, 0);
    mark_data_table(L, -1, data_table_kind::object);
    for (const auto& [key, element] : value) {
      limits.text(key.size(), path);
      lua_pushlstring(L, key.data(), key.size());
      push(L, element, limits, path + "." + key, depth + 1);
      lua_rawset(L, -3);
    }
  } else if constexpr (std::is_aggregate_v<T>) {
    lua_createtable(L, 0, reflect::size<T>());
    mark_data_table(L, -1, data_table_kind::object);
    reflect::for_each([&](auto I) {
      const auto name = reflect::member_name<I>(value);
      limits.text(name.size(), path);
      lua_pushlstring(L, name.data(), name.size());
      push(L, reflect::get<I>(value), limits, path + "." + std::string(name), depth + 1);
      lua_rawset(L, -3);
    },
                      value);
  } else
    static_assert(unsupported<T>, "Lua data conversion requires owned values, sequences, string maps, or reflect aggregates");
}

template <class T>
T read(lua_State* L, int index, budget& limits, const std::string& path, size_t depth) {
  stack_guard stack(L);
  limits.enter(depth, path);
  require(lua_checkstack(L, 6), path, "Lua stack cannot grow");
  index = lua_absindex(L, index);
  if constexpr (std::is_same_v<T, bool>) {
    require(lua_type(L, index) == LUA_TBOOLEAN, path, "expected boolean");
    return lua_toboolean(L, index) != 0;
  } else if constexpr (std::is_enum_v<T>) {
    return static_cast<T>(read<std::underlying_type_t<T>>(L, index, limits, path, depth));
  } else if constexpr (std::is_integral_v<T>) {
    if (auto number = data_uint64(L, index)) {
      require(std::in_range<T>(*number), path, "integer outside destination range");
      return static_cast<T>(*number);
    }
    require(lua_isinteger(L, index), path, "expected integer (no numeric coercion)");
    const auto number = lua_tointeger(L, index);
    require(std::in_range<T>(number), path, "integer outside destination range");
    return static_cast<T>(number);
  } else if constexpr (std::is_floating_point_v<T>) {
    require(lua_type(L, index) == LUA_TNUMBER, path, "expected number");
    const auto number = lua_tonumber(L, index);
    require(std::isfinite(number) && std::abs(number) <= std::numeric_limits<T>::max(), path, "non-finite or out-of-range number");
    return static_cast<T>(number);
  } else if constexpr (std::is_same_v<T, std::string>) {
    return string_at(L, index, limits, path);
  } else if constexpr (optional_type<T>::value) {
    if (lua_isnil(L, index) || is_data_null(L, index)) return std::nullopt;
    return read<typename T::value_type>(L, index, limits, path, depth + 1);
  } else if constexpr (sequence_type<T>::value) {
    const auto count = sequence_size(L, index, limits, path);
    T result{};
    if constexpr (requires { result.resize(count); })
      result.resize(count);
    else
      require(count == result.size(), path, "fixed sequence size mismatch");
    for (size_t i = 0; i < count; ++i) {
      lua_rawgeti(L, index, i + 1);
      result[i] = read<typename T::value_type>(L, -1, limits, path + "[" + std::to_string(i + 1) + "]", depth + 1);
      lua_pop(L, 1);
    }
    return result;
  } else if constexpr (map_type<T>::value) {
    require(lua_type(L, index) == LUA_TTABLE, path, "expected string map table");
    const auto kind = data_table_type(L, index);
    require(!kind || *kind == data_table_kind::object, path, "sequence table is not a string map");
    T result;
    lua_pushnil(L);
    while (lua_next(L, index)) {
      const auto key = string_at(L, -2, limits, path);
      result.emplace(key, read<typename T::mapped_type>(L, -1, limits, path + "." + key, depth + 1));
      lua_pop(L, 1);
    }
    return result;
  } else if constexpr (std::is_aggregate_v<T>) {
    require(lua_type(L, index) == LUA_TTABLE, path, "expected record table");
    const auto kind = data_table_type(L, index);
    require(!kind || *kind == data_table_kind::object, path, "sequence table is not a record");
    T result{};
    lua_pushnil(L);
    while (lua_next(L, index)) {
      const auto key = string_at(L, -2, limits, path);
      bool known = false;
      reflect::for_each([&](auto I) {
        known = known || reflect::member_name<I>(result) == key;
      },
                        result);
      require(known, path + "." + key, "unknown record field");
      lua_pop(L, 1);
    }
    reflect::for_each([&](auto I) {
      const auto name = reflect::member_name<I>(result);
      lua_pushlstring(L, name.data(), name.size());
      lua_rawget(L, index);
      using field = std::remove_cvref_t<decltype(reflect::get<I>(result))>;
      const auto child = path + "." + std::string(name);
      if constexpr (!optional_type<field>::value) require(!lua_isnil(L, -1), child, "missing required field");
      reflect::get<I>(result) = read<field>(L, -1, limits, child, depth + 1);
      lua_pop(L, 1);
    },
                      result);
    return result;
  } else
    static_assert(unsupported<T>, "unsupported Lua data destination");
}
} // namespace data_detail

template <class T>
sol::object to_lua(sol::state_view lua, const T& value, conversion_limits limits = {}) {
  data_detail::stack_guard stack(lua.lua_state());
  data_detail::budget budget{limits};
  data_detail::push(lua.lua_state(), value, budget, "$", 0);
  return sol::stack::get<sol::object>(lua.lua_state(), -1);
}
template <class T>
T from_lua(const sol::object& object, conversion_limits limits = {}) {
  auto* L = object.lua_state();
  data_detail::require(L != nullptr, "$", "Lua object has no owning state");
  data_detail::stack_guard stack(L);
  object.push();
  data_detail::budget budget{limits};
  return data_detail::read<T>(L, -1, budget, "$", 0);
}
// TAVL documents are records (implicit root object); no Lua source is evaluated.
sol::object tavl_to_lua(sol::state_view lua, std::string_view text, conversion_limits limits = {});
std::string lua_to_tavl(const sol::object& object, conversion_limits limits = {});
} // namespace devils_engine::bindings
