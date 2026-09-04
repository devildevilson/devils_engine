#ifndef DEVILS_ENGINE_SIMUL_LUA_RESOURCE_BINDINGS_H
#define DEVILS_ENGINE_SIMUL_LUA_RESOURCE_BINDINGS_H

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

// Адресация ресурса (снятие расширения, `.`/`..`, хвост `:name`) живёт в demiurg: правило одно на
// весь движок, иначе lua и остальные потребители разойдутся в том, что значит один и тот же путь.
using demiurg::absolute_resource_path;
using demiurg::resource_parent_path;

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

inline void install_resource_lua_bindings(
  sol::state& lua,
  sol::environment env,
  const demiurg::resource_system* engine_registry,
  const demiurg::resource_system* assets_registry,
  std::shared_ptr<const resource_access_scope> scope = {}) {
  lua.new_usertype<demiurg::resource_handle>("resource_handle", sol::no_constructor, "valid", [](const demiurg::resource_handle& h) -> bool {
    return h.get() != nullptr;
  },
                                             "id", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
                                               auto* res = h.get();
                                               if (res == nullptr) {
                                                 return sol::nil;
                                               }
                                               return sol::make_object(s, std::string(res->id));
                                             },
                                             "hash", [](const demiurg::resource_handle& h) -> uint64_t {
                                               return h.hash;
                                             },
                                             "state", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
                                               auto* res = h.get();
                                               if (res == nullptr) {
                                                 return sol::nil;
                                               }
                                               return sol::make_object(s, res->state());
                                             },
                                             "usable", [](const demiurg::resource_handle& h) -> bool {
                                               auto* res = h.get();
                                               return res != nullptr && res->usable();
                                             },
                                             "final_state", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
                                               auto* res = h.get();
                                               if (res == nullptr) {
                                                 return sol::nil;
                                               }
                                               return sol::make_object(s, res->final_state());
                                             },
                                             "top_state", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
                                               auto* res = h.get();
                                               if (res == nullptr) {
                                                 return sol::nil;
                                               }
                                               return sol::make_object(s, res->top_state());
                                             });

  sol::table require_cache = lua.create_table();
  auto require_stack = std::make_shared<std::vector<std::string>>();

  const auto resolve_resource = [engine_registry, assets_registry, require_stack, scope](sol::this_state s, const std::string& id) -> sol::object {
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_id = absolute_resource_path(current, id);
    if (abs_id.empty()) {
      return sol::nil;
    }
    const auto h = lookup_resource_handle(engine_registry, assets_registry, abs_id, scope);
    if (h.get() == nullptr) {
      return sol::nil;
    }
    return sol::make_object(s, h);
  };
  env.set_function("resource", resolve_resource);
  env.set_function("request", resolve_resource); // compatibility alias; this function never loads.

  env.set_function("find", [engine_registry, assets_registry, require_stack, scope](sol::this_state s, const std::string& prefix) -> sol::table {
    sol::state_view lua_view(s);
    sol::table out = lua_view.create_table();
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_prefix = absolute_resource_path(current, prefix);
    if (abs_prefix.empty()) {
      return out;
    }
    int index = 0;
    append_find_handles(out, index, engine_registry, abs_prefix, scope);
    append_find_handles(out, index, assets_registry, abs_prefix, scope);
    return out;
  });

  env.set_function("filter", [engine_registry, assets_registry, require_stack, scope](sol::this_state s, const std::string& text) -> sol::table {
    sol::state_view lua_view(s);
    sol::table out = lua_view.create_table();
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_text = absolute_resource_path(current, text);
    if (abs_text.empty()) {
      return out;
    }
    int index = 0;
    append_filter_handles(out, index, engine_registry, abs_text, scope);
    append_filter_handles(out, index, assets_registry, abs_text, scope);
    return out;
  });

  env.set_function("require", [engine_registry, assets_registry, env, require_cache, require_stack, scope](sol::this_state s, const std::string& id) mutable -> sol::object {
    sol::state_view lua_view(s);
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_id = absolute_resource_path(current, id);
    if (abs_id.empty()) {
      luaL_error(s, "require('%s') failed: invalid demiurg resource path", id.c_str());
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
    require_stack->push_back(script_id);
    auto ret = lua_view.safe_script(script->text, env, sol::script_pass_on_error, chunk_name);
    require_stack->pop_back();
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
  });
}

} // namespace simul
} // namespace devils_engine

#endif
