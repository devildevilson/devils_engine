#ifndef DEVILS_ENGINE_SIMUL_LUA_RESOURCE_BINDINGS_H
#define DEVILS_ENGINE_SIMUL_LUA_RESOURCE_BINDINGS_H

#include "devils_engine/bindings/critical_error.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <devils_engine/bindings/lua_header.h>
#include <devils_engine/demiurg/resource_path.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/lua_script_resource.h>
#include <devils_engine/simul/resource_access_scope.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <lua.hpp>

namespace devils_engine {
namespace simul {

inline demiurg::resource_handle lookup_resource_handle(
  const demiurg::resource_system* engine_registry,
  const demiurg::resource_system* assets_registry,
  const std::string_view id,
  const std::shared_ptr<const resource_access_scope>& scope = {}) {
  if (engine_registry != nullptr) {
    const auto h = engine_registry->handle(id);
    if (h.get() != nullptr && resource_is_visible(scope, h)) {
      return h;
    }
  }

  if (assets_registry != nullptr) {
    const auto h = assets_registry->handle(id);
    if (h.get() != nullptr && resource_is_visible(scope, h)) {
      return h;
    }
  }

  return {};
}

// Lua получает ID ресурса, а не путь к файлу: demiurg уже снял расширение с имени файла, и
// `fonts/abc.def.ttf` на диске — это id `fonts/abc.def`. Поэтому id берётся как написан: никаких
// снятий «расширения» (точка в id допустима — прежде `fonts/crimson.italic` превращался в
// `fonts/crimson`, и шрифт молча подменялся умолчанием) и никаких относительных путей. Перевод
// человеческих путей с расширением (`./values.tavl`) — дело конфигов генератора
// (`demiurg::absolute_resource_path`), не скриптов.

inline void append_find_handles(
  sol::table& out,
  int& index,
  const demiurg::resource_system* const reg,
  const std::string_view prefix,
  const std::shared_ptr<const resource_access_scope>& scope) {
  if (reg == nullptr) {
    return;
  }
  const auto view = reg->find(prefix);
  for (auto* res : view) {
    if (res == nullptr) {
      continue;
    }
    const auto handle = reg->handle(res->id);
    if (resource_is_visible(scope, handle)) {
      out[++index] = handle;
    }
  }
}

inline void append_filter_handles(
  sol::table& out,
  int& index,
  const demiurg::resource_system* const reg,
  const std::string_view filter,
  const std::shared_ptr<const resource_access_scope>& scope) {
  if (reg == nullptr) {
    return;
  }
  std::vector<demiurg::resource_interface*> resources;
  reg->filter<demiurg::resource_interface>(filter, resources);
  for (auto* res : resources) {
    if (res == nullptr) {
      continue;
    }
    const auto handle = reg->handle(res->id);
    if (resource_is_visible(scope, handle)) {
      out[++index] = handle;
    }
  }
}

// Методы resource_handle для Lua — именованные функции (не лямбды: у state/final_state/top_state одна
// сигнатура, и лямбды одной функции делили бы метатаблицу __gc в sol2).
struct resource_handle_lua {
  static bool valid(const demiurg::resource_handle& h) {
    return h.get() != nullptr;
  }
  static sol::object id(sol::this_state s, const demiurg::resource_handle& h) {
    auto* res = h.get();
    return res == nullptr ? sol::object(sol::nil) : sol::make_object(s, std::string(res->id));
  }
  static uint64_t hash(const demiurg::resource_handle& h) {
    return h.hash;
  }
  static sol::object state(sol::this_state s, const demiurg::resource_handle& h) {
    auto* res = h.get();
    return res == nullptr ? sol::object(sol::nil) : sol::make_object(s, res->state());
  }
  static bool usable(const demiurg::resource_handle& h) {
    auto* res = h.get();
    return res != nullptr && res->usable();
  }
  static sol::object final_state(sol::this_state s, const demiurg::resource_handle& h) {
    auto* res = h.get();
    return res == nullptr ? sol::object(sol::nil) : sol::make_object(s, res->final_state());
  }
  static sol::object top_state(sol::this_state s, const demiurg::resource_handle& h) {
    auto* res = h.get();
    return res == nullptr ? sol::object(sol::nil) : sol::make_object(s, res->top_state());
  }
};

inline void install_resource_lua_bindings(
  sol::state& lua,
  sol::environment env,
  const demiurg::resource_system* engine_registry,
  const demiurg::resource_system* assets_registry,
  std::shared_ptr<const resource_access_scope> scope = {}) {
  lua.new_usertype<demiurg::resource_handle>("resource_handle", sol::no_constructor,
                                             "valid", &resource_handle_lua::valid,
                                             "id", &resource_handle_lua::id,
                                             "hash", &resource_handle_lua::hash,
                                             "state", &resource_handle_lua::state,
                                             "usable", &resource_handle_lua::usable,
                                             "final_state", &resource_handle_lua::final_state,
                                             "top_state", &resource_handle_lua::top_state);

  sol::table require_cache = lua.create_table();
  // Реализация биндингов — именованные методы, регистрация ниже. Не лямбды: одинаковые сигнатуры
  // лямбд одной функции делят метатаблицу __gc в sol2.
  using engine_registry_type = std::remove_cvref_t<decltype(engine_registry)>;
  using assets_registry_type = std::remove_cvref_t<decltype(assets_registry)>;
  using scope_type = std::remove_cvref_t<decltype(scope)>;
  using env_type = std::remove_cvref_t<decltype(env)>;
  using require_cache_type = std::remove_cvref_t<decltype(require_cache)>;
  struct lua_impl {
    engine_registry_type engine_registry;
    assets_registry_type assets_registry;
    scope_type scope;
    env_type env;
    require_cache_type require_cache;
    sol::object resource(sol::this_state s, const std::string& id) const {
      if (id.empty()) {
        return sol::nil;
      }
      const auto h = lookup_resource_handle(engine_registry, assets_registry, id, scope);
      if (h.get() == nullptr) {
        return sol::nil;
      }
      return sol::make_object(s, h);
    }
    sol::table find(sol::this_state s, const std::string& prefix) const {
      sol::state_view lua_view(s);
      sol::table out = lua_view.create_table();
      if (prefix.empty()) {
        return out;
      }
      int index = 0;
      append_find_handles(out, index, engine_registry, prefix, scope);
      append_find_handles(out, index, assets_registry, prefix, scope);
      return out;
    }
    sol::table filter(sol::this_state s, const std::string& text) const {
      sol::state_view lua_view(s);
      sol::table out = lua_view.create_table();
      if (text.empty()) {
        return out;
      }
      int index = 0;
      append_filter_handles(out, index, engine_registry, text, scope);
      append_filter_handles(out, index, assets_registry, text, scope);
      return out;
    }
    sol::object require(sol::this_state s, const std::string& id) {
      sol::state_view lua_view(s);
      const std::string& abs_id = id;
      if (abs_id.empty()) {
        luaL_error(s, "require('%s') failed: empty demiurg resource id", id.c_str());
        return sol::nil;
      }

      sol::object cached = require_cache[abs_id];
      if (cached.valid() && cached != sol::nil) {
        return cached;
      }

      const auto h = lookup_resource_handle(engine_registry, assets_registry, abs_id, scope);
      auto* base = h.get();
      if (base == nullptr) {
        luaL_error(s, "require('%s') failed: demiurg resource '%s' was not found", id.c_str(), abs_id.c_str());
        return sol::nil;
      }

      auto* script = h.get<lua_script_resource>();
      if (script == nullptr) {
        const std::string resource_id(base->id);
        luaL_error(s, "require('%s') failed: resource '%s' is not a lua script", id.c_str(), resource_id.c_str());
        return sol::nil;
      }

      if (!script->usable()) {
        luaL_error(s, "require('%s') failed: script '%s' is allowed but not loaded", id.c_str(), abs_id.c_str());
        return sol::nil;
      }
      require_cache[abs_id] = true;

      const std::string script_id(script->id);
      const std::string chunk_name = "@" + script_id;
      auto ret = lua_view.safe_script(script->text, env, sol::script_pass_on_error, chunk_name);
      try {
        bindings::rethrow_critical_error(lua_view.lua_state());
      } catch (const utils::critical_error&) {
        require_cache[abs_id] = sol::nil;
        throw;
      }
      if (!ret.valid()) {
        require_cache[abs_id] = sol::nil;
        const sol::error err = ret;
        luaL_error(s, "require('%s') failed while loading lua module '%s': %s", id.c_str(), script_id.c_str(), err.what());
        return sol::nil;
      }

      sol::object result = ret.return_count() > 0 ? ret.get<sol::object>() : sol::make_object(lua_view, true);
      if (!result.valid() || result == sol::nil) {
        result = sol::make_object(lua_view, true);
      }
      require_cache[abs_id] = result;
      return result;
    }
  };
  const lua_impl impl{engine_registry, assets_registry, scope, env, require_cache};
  env.set_function("resource", &lua_impl::resource, impl);
  env.set_function("request", &lua_impl::resource, impl); // compatibility alias; this function never loads.
  env.set_function("find", &lua_impl::find, impl);

  env.set_function("filter", &lua_impl::filter, impl);

  env.set_function("require", &lua_impl::require, impl);
}

} // namespace simul
} // namespace devils_engine

#endif
