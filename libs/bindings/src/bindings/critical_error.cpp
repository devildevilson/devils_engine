#include "devils_engine/bindings/critical_error.h"

#include <exception>
#include <string>
#include <utility>
#include "devils_engine/utils/core.h"

// Lua использует longjmp: C++ исключение нельзя выпускать через его C frames. Sol2
// ловит исключение, мы сохраняем его идентичность в registry, а pcall/xpcall пропускают
// критический Lua error наружу. Хост восстанавливает исходный critical_error после VM call.
namespace devils_engine::bindings {
namespace {
char state_key;
struct critical_state {
  std::exception_ptr failure;
  std::string message;
};
critical_state* state_at(lua_State* L) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &state_key);
  auto* state = static_cast<critical_state*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return state;
}
int destroy_state(lua_State* L) {
  static_cast<critical_state*>(lua_touserdata(L, 1))->~critical_state();
  return 0;
}
int exception_handler(lua_State* L, sol::optional<const std::exception&>, sol::string_view what) {
  auto* state = state_at(L);
  const auto failure = std::current_exception();
  if (state != nullptr && failure && !state->failure) {
    try {
      std::rethrow_exception(failure);
    } catch (const utils::critical_error& error) {
      state->failure = failure;
      state->message = error.what();
    } catch (...) {
      // Чужое исключение здесь не объявляется критическим по одному лишь имени std::exception.
    }
  }
  lua_pushlstring(L, what.data(), what.size());
  return 1;
}
int propagate_critical(lua_State* L) {
  auto* state = state_at(L);
  lua_pushlstring(L, state->message.data(), state->message.size());
  return lua_error(L);
}
int finish_call(lua_State* L, int status, lua_KContext prefix) {
  if (auto* state = state_at(L); state != nullptr && state->failure) return propagate_critical(L);
  if (status != LUA_OK && status != LUA_YIELD) {
    lua_pushboolean(L, false);
    lua_pushvalue(L, -2);
    return 2;
  }
  return lua_gettop(L) - static_cast<int>(prefix);
}
int protected_call(lua_State* L) {
  if (auto* state = state_at(L); state != nullptr && state->failure) return propagate_critical(L);
  luaL_checkany(L, 1);
  lua_pushboolean(L, true);
  lua_insert(L, 1);
  const int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finish_call);
  return finish_call(L, status, 0);
}
int message_handler(lua_State* L) {
  if (auto* state = state_at(L); state != nullptr && state->failure) {
    lua_pushlstring(L, state->message.data(), state->message.size());
    return 1;
  }
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  return 1;
}
int extended_protected_call(lua_State* L) {
  if (auto* state = state_at(L); state != nullptr && state->failure) return propagate_critical(L);
  const int count = lua_gettop(L);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  lua_pushcclosure(L, message_handler, 1);
  lua_replace(L, 2);
  lua_pushboolean(L, true);
  lua_pushvalue(L, 1);
  lua_rotate(L, 3, 2);
  const int status = lua_pcallk(L, count - 2, LUA_MULTRET, 2, 2, finish_call);
  return finish_call(L, status, 2);
}
} // namespace

void install_critical_error_policy(lua_State* L) {
  if (state_at(L) != nullptr) return;
  auto* state = static_cast<critical_state*>(lua_newuserdatauv(L, sizeof(critical_state), 0));
  new (state) critical_state;
  if (luaL_newmetatable(L, "devils_engine.bindings.critical_state")) {
    lua_pushcfunction(L, destroy_state);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &state_key);
  sol::state_view(L).set_exception_handler(exception_handler);
  lua_pushcfunction(L, protected_call);
  lua_setglobal(L, "pcall");
  lua_pushcfunction(L, extended_protected_call);
  lua_setglobal(L, "xpcall");
}

void rethrow_critical_error(lua_State* L) {
  auto* state = state_at(L);
  if (state == nullptr || !state->failure) return;
  auto failure = std::exchange(state->failure, {});
  state->message.clear();
  std::rethrow_exception(failure);
}
} // namespace devils_engine::bindings
