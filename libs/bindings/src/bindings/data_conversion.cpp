#include "devils_engine/bindings/data_conversion.h"

#include <algorithm>
#include <tavl/parser.h>
#include <tavl/serialize.h>

namespace devils_engine::bindings {
namespace {
char null_key;
char table_kinds_key;
constexpr const char* uint64_metatable = "devils_engine.bindings.data_uint64";
void kinds(lua_State* L) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &table_kinds_key);
  if (!lua_isnil(L, -1)) return;
  lua_pop(L, 1);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_pushvalue(L, -1);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &table_kinds_key);
}
struct reader {
  lua_State* L;
  tavl::parser parser;
  data_detail::budget budget;
  tavl::event next() {
    for (;;) {
      const auto [event, error] = parser.poll_event();
      if (error.type != tavl::error_type::no_error) {
        utils::error{}("TAVL to Lua: {} at {}:{}", tavl::to_string(error.type), error.span.line + 1, error.span.column + 1);
      }
      if (event.type == tavl::event_type::got_comment || event.type == tavl::event_type::empty_row || event.type == tavl::event_type::row_begin) continue;
      data_detail::require(event.type != tavl::event_type::not_enought_data, "$", "incomplete TAVL document");
      return event;
    }
  }
  void value(tavl::event event, const std::string& path, size_t depth) {
    using ev = tavl::event_type;
    using tok = tavl::token_type;
    budget.enter(depth, path);
    data_detail::require(lua_checkstack(L, 6), path, "Lua stack cannot grow");
    if (event.type == ev::object_begin) return object(ev::object_end, path, depth);
    if (event.type == ev::array_begin || event.type == ev::tuple_begin) {
      const bool tuple = event.type == ev::tuple_begin;
      lua_newtable(L);
      mark_data_table(L, -1, tuple ? data_table_kind::tuple : data_table_kind::array);
      const auto end = tuple ? ev::tuple_end : ev::array_end;
      size_t count = 0;
      for (;;) {
        event = next();
        if (event.type == end) return;
        if (event.type == ev::row_end) continue;
        value(event, path + "[" + std::to_string(count + 1) + "]", depth + 1);
        lua_rawseti(L, -2, ++count);
      }
    }
    data_detail::require(event.type == ev::got_token, path, "expected TAVL data value (expressions are unsupported)");
    switch (event.token.type) {
      case tok::null: push_data_null(L); return;
      case tok::boolean: {
        const auto number = parser.to_boolean(event.token);
        data_detail::require(number.has_value(), path, "invalid boolean");
        lua_pushboolean(L, *number);
        return;
      }
      case tok::number_int: {
        const auto number = parser.to_int(event.token);
        data_detail::require(number.has_value(), path, "signed integer out of range");
        lua_pushinteger(L, *number);
        return;
      }
      case tok::number_uint: {
        const auto number = parser.to_uint(event.token);
        data_detail::require(number.has_value(), path, "unsigned integer out of range");
        push_data_uint64(L, *number);
        return;
      }
      case tok::number_float: {
        const auto number = parser.to_float(event.token);
        data_detail::require(number.has_value() && std::isfinite(*number), path, "non-finite or out-of-range number");
        lua_pushnumber(L, *number);
        return;
      }
      case tok::identifier:
      case tok::singlequote_string:
      case tok::doublequote_string: {
        auto text = parser.to_string(event.token);
        budget.text(text.size(), path);
        lua_pushlstring(L, text.data(), text.size());
        return;
      }
      default: data_detail::require(false, path, "unsupported TAVL data token");
    }
  }
  void object(tavl::event_type end, const std::string& path, size_t depth) {
    using ev = tavl::event_type;
    lua_newtable(L);
    mark_data_table(L, -1, data_table_kind::object);
    for (;;) {
      auto event = next();
      if (event.type == end) return;
      if (event.type == ev::row_end) continue;
      data_detail::require(event.type == ev::got_row_identifier, path, "expected TAVL named field");
      auto key = parser.to_string(event.token);
      budget.text(key.size(), path);
      const auto child = path + "." + key;
      lua_pushlstring(L, key.data(), key.size());
      lua_rawget(L, -2);
      const bool duplicate = !lua_isnil(L, -1);
      lua_pop(L, 1);
      data_detail::require(!duplicate, child, "duplicate TAVL field");
      event = next();
      data_detail::require(event.type == ev::got_row_operator && parser.content(event.token.span) == "=", child, "expected '=' field operator");
      lua_pushlstring(L, key.data(), key.size());
      value(next(), child, depth + 1);
      lua_rawset(L, -3);
      event = next();
      data_detail::require(event.type == ev::row_end, child, "expected end of field after one data value");
    }
  }
};
struct writer {
  lua_State* L;
  data_detail::budget budget;
  std::string output;
  static constexpr auto options = tavl::sopts{.prettify = false, .quote_all_strings = true};
  void value(int index, const std::string& path, size_t depth, bool root = false) {
    budget.enter(depth, path);
    data_detail::require(lua_checkstack(L, 6), path, "Lua stack cannot grow");
    index = lua_absindex(L, index);
    const auto wide = data_uint64(L, index);
    if (is_data_null(L, index))
      output += "null";
    else if (wide)
      tavl::serialize<options>(*wide, output);
    else if (lua_type(L, index) == LUA_TBOOLEAN)
      output += lua_toboolean(L, index) ? "true" : "false";
    else if (lua_type(L, index) == LUA_TNUMBER) {
      if (lua_isinteger(L, index))
        tavl::serialize<options>(lua_tointeger(L, index), output);
      else {
        const auto number = lua_tonumber(L, index);
        data_detail::require(std::isfinite(number), path, "non-finite number");
        tavl::serialize<options>(number, output);
      }
    } else if (lua_type(L, index) == LUA_TSTRING) {
      tavl::ser_string<options>(data_detail::string_at(L, index, budget, path), output);
    } else if (lua_type(L, index) == LUA_TTABLE) {
      auto kind = data_table_type(L, index);
      if (!kind) {
        lua_pushnil(L);
        if (lua_next(L, index)) {
          kind = lua_isinteger(L, -2) ? data_table_kind::array : data_table_kind::object;
          lua_pop(L, 2);
        } else
          kind = data_table_kind::object;
      }
      data_detail::require(!root || *kind == data_table_kind::object, path, "TAVL document root must be a record");
      if (*kind != data_table_kind::object) {
        const auto count = data_detail::sequence_size(L, index, budget, path);
        output += *kind == data_table_kind::tuple ? '(' : '[';
        for (size_t i = 0; i < count; ++i) {
          if (i) output += ", ";
          lua_rawgeti(L, index, i + 1);
          value(-1, path + "[" + std::to_string(i + 1) + "]", depth + 1);
          lua_pop(L, 1);
        }
        output += *kind == data_table_kind::tuple ? ')' : ']';
      } else {
        std::vector<std::string> keys;
        lua_pushnil(L);
        while (lua_next(L, index)) {
          data_detail::require(keys.size() < budget.remaining.values, path, "record value limit exceeded");
          keys.push_back(data_detail::string_at(L, -2, budget, path));
          lua_pop(L, 1);
        }
        std::sort(keys.begin(), keys.end());
        if (!root) output += '{';
        for (size_t i = 0; i < keys.size(); ++i) {
          if (i) output += root ? '\n' : ',';
          tavl::ser_string<options>(keys[i], output);
          output += " = ";
          lua_pushlstring(L, keys[i].data(), keys[i].size());
          lua_rawget(L, index);
          value(-1, path + "." + keys[i], depth + 1);
          lua_pop(L, 1);
        }
        if (!root) output += '}';
      }
    } else
      data_detail::require(false, path, "expected plain data, unsupported Lua value");
  }
};
} // namespace

void mark_data_table(lua_State* L, int index, data_table_kind kind) {
  index = lua_absindex(L, index);
  kinds(L);
  lua_pushvalue(L, index);
  lua_pushinteger(L, int(kind));
  lua_rawset(L, -3);
  lua_pop(L, 1);
}
std::optional<data_table_kind> data_table_type(lua_State* L, int index) {
  index = lua_absindex(L, index);
  kinds(L);
  lua_pushvalue(L, index);
  lua_rawget(L, -2);
  std::optional<data_table_kind> result;
  if (lua_isinteger(L, -1)) result = data_table_kind(lua_tointeger(L, -1));
  lua_pop(L, 2);
  return result;
}
void push_data_null(lua_State* L) {
  lua_pushlightuserdata(L, &null_key);
}
bool is_data_null(lua_State* L, int index) {
  return lua_type(L, index) == LUA_TLIGHTUSERDATA && lua_touserdata(L, index) == &null_key;
}
void push_data_uint64(lua_State* L, uint64_t value) {
  if (std::in_range<lua_Integer>(value)) {
    lua_pushinteger(L, value);
    return;
  }
  *static_cast<uint64_t*>(lua_newuserdatauv(L, sizeof(uint64_t), 0)) = value;
  if (luaL_newmetatable(L, uint64_metatable)) {
    lua_pushliteral(L, "immutable uint64 data");
    lua_setfield(L, -2, "__metatable");
  }
  lua_setmetatable(L, -2);
}
std::optional<uint64_t> data_uint64(lua_State* L, int index) {
  const auto* value = static_cast<const uint64_t*>(luaL_testudata(L, index, uint64_metatable));
  if (value) return *value;
  return std::nullopt;
}
sol::object tavl_to_lua(sol::state_view lua, std::string_view text, conversion_limits limits) {
  data_detail::stack_guard stack(lua.lua_state());
  data_detail::require(text.size() <= limits.string_bytes, "$", "TAVL input byte limit exceeded");
  reader input{lua.lua_state(), {}, {limits}};
  input.parser.add_default_operator();
  input.parser.flush(text);
  input.parser.finish();
  input.budget.enter(0, "$");
  input.object(tavl::event_type::eof, "$", 0);
  return sol::stack::get<sol::object>(lua.lua_state(), -1);
}
std::string lua_to_tavl(const sol::object& object, conversion_limits limits) {
  auto* L = object.lua_state();
  data_detail::require(L != nullptr, "$", "Lua object has no owning state");
  data_detail::stack_guard stack(L);
  object.push();
  data_detail::require(lua_type(L, -1) == LUA_TTABLE, "$", "TAVL document root must be a record");
  writer output{L, {limits}, {}};
  output.value(-1, "$", 0, true);
  return std::move(output.output);
}
} // namespace devils_engine::bindings
