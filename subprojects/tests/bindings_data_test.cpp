#include <doctest/doctest.h>
#include "devils_engine/bindings/data_conversion.h"
#include "devils_engine/bindings/critical_error.h"
#include "devils_engine/bindings/env.h"

using namespace devils_engine;
namespace {
enum class mode : unsigned { active = 3 };
struct record {
  std::string text;
  uint64_t key;
  int64_t negative;
  std::vector<std::optional<int>> values;
  std::array<bool, 2> flags;
  std::map<std::string, double> weights;
  std::optional<std::string> absent;
  mode state;
};
void fatal_native() {
  utils::error{}("native critical test");
}
void ordinary_native() {
  throw std::runtime_error("ordinary test");
}
sol::object evaluate(sol::state& lua, const std::string& text) {
  auto result = lua.safe_script(text, sol::script_pass_on_error);
  REQUIRE(result.valid());
  return result.get<sol::object>();
}
} // namespace
TEST_CASE("reflect Lua data and TAVL round trips preserve owned values") {
  sol::state lua;
  lua.open_libraries(sol::lib::base);
  const record input{"quotes \" slash \\ newline\n UTF-8: привет", UINT64_MAX, INT64_MIN, {1, std::nullopt, 3}, {true, false}, {{"a", 1.25}}, {}, mode::active};
  const int top = lua_gettop(lua.lua_state());
  auto object = bindings::to_lua(lua, input);
  auto output = bindings::from_lua<record>(object);
  CHECK(output.text == input.text);
  CHECK(output.key == input.key);
  CHECK(output.negative == input.negative);
  CHECK(output.values == input.values);
  CHECK(output.flags == input.flags);
  CHECK(output.weights == input.weights);
  CHECK_FALSE(output.absent);
  CHECK(output.state == input.state);
  auto text = bindings::lua_to_tavl(object);
  auto parsed = bindings::tavl_to_lua(lua, text);
  CHECK(bindings::from_lua<record>(parsed).text == input.text);
  CHECK(bindings::from_lua<record>(parsed).key == UINT64_MAX);
  CHECK(bindings::lua_to_tavl(parsed) == text);
  CHECK(lua_gettop(lua.lua_state()) == top);
  const std::string binary("a\0b", 3);
  CHECK(bindings::from_lua<std::string>(bindings::to_lua(lua, binary)) == binary);
}
TEST_CASE("Lua data conversions reject coercion malformed records and bounded recursion") {
  sol::state lua;
  lua.open_libraries(sol::lib::base);
  struct flag {
    bool enabled;
  };
  const int top = lua_gettop(lua.lua_state());
  for (const auto& source : {"return {enabled=1}", "return {}", "return {enabled=true, extra=1}"}) {
    auto object = evaluate(lua, source);
    CHECK_THROWS_AS(bindings::from_lua<flag>(object), utils::critical_error);
    CHECK(lua_gettop(lua.lua_state()) == top);
  }
  for (const auto& source : {"return {[1]=1,[3]=3}", "return {[1]=1,x=2}"}) {
    auto object = evaluate(lua, source);
    CHECK_THROWS_AS(bindings::from_lua<std::vector<int>>(object), utils::critical_error);
  }
  auto cycle = evaluate(lua, "local t = {}; t.self = t; return t");
  CHECK_THROWS_AS(bindings::lua_to_tavl(cycle, {.depth = 4}), utils::critical_error);
  auto plain = evaluate(lua, "return setmetatable({enabled=true}, {__index=function() error('metamethod') end, __pairs=function() error('pairs') end})");
  CHECK(bindings::from_lua<flag>(plain).enabled);
  CHECK_THROWS_AS(bindings::to_lua(lua, std::vector<int>{1, 2, 3}, {.values = 2}), utils::critical_error);
  CHECK_THROWS_AS(bindings::to_lua(lua, std::string("long"), {.string_bytes = 3}), utils::critical_error);
  CHECK_THROWS_AS(bindings::from_lua<unsigned>(evaluate(lua, "return -1")), utils::critical_error);
  CHECK_THROWS_AS(bindings::from_lua<int>(evaluate(lua, "return '123'")), utils::critical_error);
  CHECK(lua_gettop(lua.lua_state()) == top);
}
TEST_CASE("TAVL Lua data preserves empty kinds null tuples and rejects expressions") {
  sol::state lua;
  auto object = bindings::tavl_to_lua(lua, "empty = []\nobject = {}\ntuple = (1, null, \"x\")\n");
  const auto text = bindings::lua_to_tavl(object);
  CHECK(text.find("[]") != std::string::npos);
  CHECK(text.find("{}") != std::string::npos);
  CHECK(text.find("(1, null, \"x\")") != std::string::npos);
  CHECK(bindings::lua_to_tavl(bindings::tavl_to_lua(lua, text)) == text);
  struct object_as_sequence { std::vector<int> value; };
  auto wrong_shape = bindings::tavl_to_lua(lua, "value = {}");
  CHECK_THROWS_AS(bindings::from_lua<object_as_sequence>(wrong_shape), utils::critical_error);
  for (const auto& invalid : {"a = 1\na = 2", "a = [", "a = 1 + 2", "a = 1 junk", "a = { b = 2"}) {
    CHECK_THROWS_AS(bindings::tavl_to_lua(lua, invalid), utils::critical_error);
    CHECK(lua_gettop(lua.lua_state()) == 0);
  }
}
TEST_CASE("base exposes TAVL data conversion without evaluating Lua text") {
  sol::state lua;
  lua.open_libraries(sol::lib::base);
  bindings::basic_functions(lua.globals());
  const auto result = lua.safe_script(R"(
    local data = base.parse_tavl('x = [1, null, "quoted"]')
    assert(data.x[2] == base.null)
    local copy = base.parse_tavl(base.write_tavl(data))
    assert(copy.x[3] == 'quoted' and copy.x[2] == base.null)
  )",
                                      sol::script_pass_on_error);
  bindings::rethrow_critical_error(lua.lua_state());
  REQUIRE(result.valid());
}
TEST_CASE("Lua pcall and xpcall preserve normal semantics including continuations") {
  sol::state lua;
  lua.open_libraries(sol::lib::base, sol::lib::coroutine);
  bindings::install_critical_error_policy(lua.lua_state());
  lua.set_function("ordinary", ordinary_native);
  auto result = lua.safe_script(R"(
    local ok, a, b, c = pcall(function(x) return x, nil, 3 end, 7)
    assert(ok and a == 7 and b == nil and c == 3)
    local failed, err = pcall(error, 'normal'); assert(not failed and err == 'normal')
    local failed2, err2 = pcall(ordinary); assert(not failed2)
    local ok2, x, y = xpcall(function(v) return v, 2 end, tostring, 9)
    assert(ok2 and x == 9 and y == 2)
    local ok3, handled = xpcall(function() error('x') end, function() return 42 end)
    assert(not ok3 and handled == 42)
    local co = coroutine.create(function()
      return pcall(function() coroutine.yield('paused'); return 'done' end)
    end)
    local r, v = coroutine.resume(co); assert(r and v == 'paused')
    local r2, p, v2 = coroutine.resume(co); assert(r2 and p and v2 == 'done')
    local cx = coroutine.create(function()
      return xpcall(function() coroutine.yield('x'); return 19 end, tostring)
    end)
    assert(coroutine.resume(cx))
    local r3, p3, v3 = coroutine.resume(cx); assert(r3 and p3 and v3 == 19)
  )",
                                sol::script_pass_on_error);
  REQUIRE(result.valid());
  CHECK_NOTHROW(bindings::rethrow_critical_error(lua.lua_state()));
}
TEST_CASE("critical native error crosses protected calls and is restored at host boundary") {
  for (const auto& source : {
         "pcall(function() pcall(fatal); reached=true end); after=true",
         "xpcall(fatal, function() handled=true; return 'hidden' end); after=true",
         "local co=coroutine.create(fatal); coroutine.resume(co); after=true"}) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine);
    bindings::install_critical_error_policy(lua.lua_state());
    lua.set_function("fatal", fatal_native);
    auto result = lua.safe_script(source, sol::script_pass_on_error);
    CHECK_THROWS_AS(bindings::rethrow_critical_error(lua.lua_state()), utils::critical_error);
    CHECK_NOTHROW(bindings::rethrow_critical_error(lua.lua_state()));
    CHECK_FALSE(lua["handled"].valid());
    CHECK_FALSE(lua["reached"].valid());
    // coroutine.resume is an additional Lua boundary; host must check even a valid result.
    if (std::string_view(source).find("coroutine") == std::string_view::npos) {
      CHECK_FALSE(result.valid());
      CHECK_FALSE(lua["after"].valid());
    }
  }
}
