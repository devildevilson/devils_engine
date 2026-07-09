#ifndef DEVILS_ENGINE_SIMUL_LUA_RESOURCE_BINDINGS_H
#define DEVILS_ENGINE_SIMUL_LUA_RESOURCE_BINDINGS_H

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <lua.hpp>

#include <devils_engine/bindings/lua_header.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/lua_script_resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>

namespace devils_engine {
namespace simul {

inline demiurg::resource_handle lookup_resource_handle(
  const demiurg::resource_system* engine_registry,
  const demiurg::resource_system* assets_registry,
  const std::string_view id
) {
  if (engine_registry != nullptr) {
    const auto h = engine_registry->handle(id);
    if (h.get() != nullptr) return h;
  }

  if (assets_registry != nullptr) {
    const auto h = assets_registry->handle(id);
    if (h.get() != nullptr) return h;
  }

  return {};
}

inline std::string resource_parent_path(const std::string_view id) {
  const size_t slash = id.rfind('/');
  if (slash == std::string_view::npos) return {};
  return std::string(id.substr(0, slash));
}

inline std::string absolute_resource_path(const std::string_view current_module, std::string_view path) {
  while (!path.empty() && (path.front() == ' ' || path.front() == '\t' || path.front() == '\n' || path.front() == '\r')) path.remove_prefix(1);
  while (!path.empty() && (path.back() == ' ' || path.back() == '\t' || path.back() == '\n' || path.back() == '\r')) path.remove_suffix(1);
  if (path.empty()) return {};

  std::string p(path);
  std::replace(p.begin(), p.end(), '\\', '/');

  std::string selector;
  const size_t colon = p.rfind(':');
  if (colon != std::string::npos) {
    selector = p.substr(colon);
    p.resize(colon);
  }

  const bool explicit_root = !p.empty() && p.front() == '/';
  while (!p.empty() && p.front() == '/') p.erase(p.begin());

  if (!explicit_root && p.starts_with(".")) {
    const std::string parent = resource_parent_path(current_module);
    if (!parent.empty()) p = parent + "/" + p;
  }

  const size_t slash = p.rfind('/');
  const size_t dot = p.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    p.resize(dot);
  }

  std::vector<std::string_view> segments;
  size_t pos = 0;
  while (pos <= p.size()) {
    size_t end = p.find('/', pos);
    if (end == std::string::npos) end = p.size();
    std::string_view segment(p.data() + pos, end - pos);
    pos = end + 1;

    if (segment.empty() || segment == ".") continue;
    if (segment == "..") {
      if (segments.empty()) return {};
      segments.pop_back();
      continue;
    }
    segments.push_back(segment);
  }

  std::string out;
  for (const auto segment : segments) {
    if (!out.empty()) out += '/';
    out += segment;
  }

  if (out.empty()) return {};
  out += selector;
  return out;
}

inline void append_find_handles(sol::table& out, int& index, const demiurg::resource_system* const reg, const std::string_view prefix) {
  if (reg == nullptr) return;
  const auto view = reg->find(prefix);
  for (auto* res : view) {
    if (res == nullptr) continue;
    out[++index] = reg->handle(res->id);
  }
}

inline void append_filter_handles(sol::table& out, int& index, const demiurg::resource_system* const reg, const std::string_view filter) {
  if (reg == nullptr) return;
  std::vector<demiurg::resource_interface*> resources;
  reg->filter<demiurg::resource_interface>(filter, resources);
  for (auto* res : resources) {
    if (res == nullptr) continue;
    out[++index] = reg->handle(res->id);
  }
}

inline void install_resource_lua_bindings(
  sol::state& lua,
  sol::environment env,
  const demiurg::resource_system* engine_registry,
  const demiurg::resource_system* assets_registry
) {
  lua.new_usertype<demiurg::resource_handle>("resource_handle",
    sol::no_constructor,
    "valid", [](const demiurg::resource_handle& h) -> bool { return h.get() != nullptr; },
    "id", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
      auto* res = h.get();
      if (res == nullptr) return sol::nil;
      return sol::make_object(s, std::string(res->id));
    },
    "hash", [](const demiurg::resource_handle& h) -> uint64_t { return h.hash; },
    "state", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
      auto* res = h.get();
      if (res == nullptr) return sol::nil;
      return sol::make_object(s, res->state());
    },
    "usable", [](const demiurg::resource_handle& h) -> bool {
      auto* res = h.get();
      return res != nullptr && res->usable();
    },
    "final_state", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
      auto* res = h.get();
      if (res == nullptr) return sol::nil;
      return sol::make_object(s, res->final_state());
    },
    "top_state", [](sol::this_state s, const demiurg::resource_handle& h) -> sol::object {
      auto* res = h.get();
      if (res == nullptr) return sol::nil;
      return sol::make_object(s, res->top_state());
    });

  sol::table require_cache = lua.create_table();
  auto require_stack = std::make_shared<std::vector<std::string>>();

  env.set_function("request", [engine_registry, assets_registry, require_stack](sol::this_state s, const std::string& id) -> sol::object {
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_id = absolute_resource_path(current, id);
    if (abs_id.empty()) return sol::nil;
    const auto h = lookup_resource_handle(engine_registry, assets_registry, abs_id);
    if (h.get() == nullptr) return sol::nil;
    return sol::make_object(s, h);
  });

  env.set_function("find", [engine_registry, assets_registry, require_stack](sol::this_state s, const std::string& prefix) -> sol::table {
    sol::state_view lua_view(s);
    sol::table out = lua_view.create_table();
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_prefix = absolute_resource_path(current, prefix);
    if (abs_prefix.empty()) return out;
    int index = 0;
    append_find_handles(out, index, engine_registry, abs_prefix);
    append_find_handles(out, index, assets_registry, abs_prefix);
    return out;
  });

  env.set_function("filter", [engine_registry, assets_registry, require_stack](sol::this_state s, const std::string& text) -> sol::table {
    sol::state_view lua_view(s);
    sol::table out = lua_view.create_table();
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_text = absolute_resource_path(current, text);
    if (abs_text.empty()) return out;
    int index = 0;
    append_filter_handles(out, index, engine_registry, abs_text);
    append_filter_handles(out, index, assets_registry, abs_text);
    return out;
  });

  env.set_function("require", [engine_registry, assets_registry, env, require_cache, require_stack](sol::this_state s, const std::string& id) mutable -> sol::object {
    sol::state_view lua_view(s);
    const std::string current = require_stack->empty() ? std::string{} : require_stack->back();
    const std::string abs_id = absolute_resource_path(current, id);
    if (abs_id.empty()) {
      luaL_error(s, "require('%s') failed: invalid demiurg resource path", id.c_str());
      return sol::nil;
    }

    sol::object cached = require_cache[abs_id];
    if (cached.valid() && cached != sol::nil) return cached;

    const auto h = lookup_resource_handle(engine_registry, assets_registry, abs_id);
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

    if (!script->usable()) script->load(utils::safe_handle_t{});
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
    if (!result.valid() || result == sol::nil) result = sol::make_object(lua_view, true);
    require_cache[abs_id] = result;
    return result;
  });
}

}
}

#endif
