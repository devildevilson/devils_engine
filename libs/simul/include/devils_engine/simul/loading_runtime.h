#ifndef DEVILS_ENGINE_SIMUL_LOADING_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_LOADING_RUNTIME_H

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>

#include "lifecycle.h"
#include "messages.h"
#include "resource_access_scope.h"
#include "standard_broker.h"
#include "startup_resources.h"

// Движковый каркас загрузки runtime-состояния: startup_entry -> runtime_state_resource -> allowlist
// ресурсов, грузимых через канал load_resource, огороженный resource_access_scope. Здесь ТОЛЬКО
// движково-generic состояние и логика (одинаковые для любого внешнего проекта). Мир/сцена/чанки —
// проектные: проект держит их у себя и AND-ит свои условия готовности к предикатам ниже. Форма —
// shared-state + свободные хелперы (как standard_render_state + standard_render_*), проектные
// host-методы lifecycle остаются в проекте и дёргают эти хелперы.

namespace devils_engine {
namespace simul {

// Generic состояние машины загрузки. Внешний проект НАСЛЕДУЕТ его своим init-контейнером и добавляет
// мировые/сценовые поля рядом — тогда доступ к полям остаётся плоским, а хелперы принимают базу.
struct standard_loading_state {
  // Явный lifecycle движка (boot->loading->game + повторные game->loading->game).
  lifecycle_controller lifecycle;

  // Стартовый allowlist + требуемые ступени, от которых зависит переход loading->game. Движок
  // засевает его UI-набором и startup=true entries активного scene manifest.
  std::vector<resource_ref> startup_resources;
  std::vector<int32_t> startup_targets;

  demiurg::resource_handle current_runtime_state;
  demiurg::resource_handle target_runtime_state;
  uint64_t state_generation = 0;

  // committed (активный) вид runtime-состояния
  std::vector<resource_ref> ui_resources;
  std::shared_ptr<resource_access_scope> ui_resource_scope = std::make_shared<resource_access_scope>();
  std::string ui_entry_script;
  std::string ui_default_font;
  std::string current_scene;

  struct scene_binding {
    resource_ref resource;
    std::string group;
    std::string alias;
    int32_t target = 0;
    bool startup = false;
  };
  std::vector<scene_binding> scene_resources;
  demiurg::resource_handle project_scene;

  // pending (целевой) вид, который сейчас готовится
  std::vector<resource_ref> pending_ui_resources;
  std::vector<std::pair<resource_ref, int32_t>> pending_boot_targets;
  std::shared_ptr<resource_access_scope> pending_ui_scope;
  std::string pending_ui_entry_script;
  std::string pending_ui_default_font;
  std::string pending_scene;
  std::vector<scene_binding> pending_scene_resources;
  demiurg::resource_handle pending_project_scene;
  bool target_prepared = false;
  bool target_ui_committed = false;
};

// Первая external/GPU-ступень ресурса (или final_state, если внешних шагов нет) — до неё boot доводит
// набор синхронно, не запуская собственно external-загрузку.
inline int32_t pre_external_target(const demiurg::resource_interface& resource) {
  int32_t target = 0;
  while (target < resource.final_state() && !resource.is_external_step(target)) {
    target += 1;
  }
  return target;
}

// Ресурс «готов»: usable() ИЛИ (при отключённых external-шагах) достиг первой external-ступени.
inline bool resource_ready(const resource_ref& ref, const bool external_steps_available) {
  auto* r = ref.get();
  if (r == nullptr) {
    return false;
  }
  if (r->usable()) {
    return true;
  }
  return !external_steps_available && r->is_external_step(r->state());
}

inline int32_t scene_request_target(
  const scene_resource_request& request,
  const demiurg::resource_interface& resource) {
  const std::string_view text = request.target.empty() ? std::string_view{"final"} : std::string_view{request.target};
  int32_t target = resource.final_state();
  if (text == "cold") {
    target = static_cast<int32_t>(demiurg::state::cold);
  } else if (text == "warm") {
    target = static_cast<int32_t>(demiurg::state::warm);
  } else if (text == "hot") {
    target = static_cast<int32_t>(demiurg::state::hot);
  } else if (text != "final") {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, target);
    if (parsed.ec != std::errc{} || parsed.ptr != end || target < 0) {
      utils::error{}("scene manifest: resource '{}' has invalid target '{}'", request.id, request.target);
    }
  }
  return std::min(target, resource.final_state());
}

inline bool resource_reached(const resource_ref& ref, const int32_t target) {
  auto* resource = ref.get();
  return resource != nullptr && resource->state() >= target;
}

inline void track_startup_resource(
  standard_loading_state& s,
  const resource_ref ref,
  const int32_t target) {
  auto* resource = ref.get();
  for (size_t i = 0; i < s.startup_resources.size(); ++i) {
    if (s.startup_resources[i].get() == resource) {
      s.startup_targets[i] = std::max(s.startup_targets[i], target);
      return;
    }
  }
  s.startup_resources.push_back(ref);
  s.startup_targets.push_back(target);
}

// boot: прочитать активный startup/entry и вернуть handle начального runtime-состояния.
inline demiurg::resource_handle standard_boot_initial_state(demiurg::resource_system& registry) {
  auto* entry = registry.get<startup_entry_resource>("startup/entry");
  if (entry == nullptr) {
    utils::error{}("startup: required entry resource 'startup/entry' was not found");
  }
  while (!entry->usable()) {
    entry->load(utils::safe_handle_t{});
  }

  const auto& entry_cfg = entry->config();
  if (entry_cfg.state.empty()) {
    utils::error{}("startup/entry: state is empty");
  }
  const auto initial_state = registry.handle(entry_cfg.state);
  if (initial_state.get<runtime_state_resource>() == nullptr) {
    utils::error{}("startup/entry: runtime state '{}' was not found", entry_cfg.state);
  }
  return initial_state;
}

// Подготовить целевое runtime-состояние: собрать allowlist (scope) + запросить загрузку каждого
// ресурса через канал load_resource. pre_external_only=true (boot) доводит до первой external-ступени
// и копит boot-targets; false (loading) запрашивает final_state.
inline void standard_prepare_runtime_state(
  standard_loading_state& s,
  demiurg::resource_system& registry,
  standard_broker& br,
  const demiurg::resource_handle state_handle,
  const bool pre_external_only) {
  auto* runtime_state = state_handle.get<runtime_state_resource>();
  if (runtime_state == nullptr) {
    utils::error{}("runtime state: invalid target handle");
  }
  while (!runtime_state->usable()) {
    runtime_state->load(utils::safe_handle_t{});
  }

  const auto& cfg = runtime_state->config();
  if (cfg.script.empty()) {
    utils::error{}("runtime state '{}': script is empty", runtime_state->id);
  }
  if (std::find(cfg.resources.begin(), cfg.resources.end(), cfg.script) == cfg.resources.end()) {
    utils::error{}("runtime state '{}': entry script '{}' is not present in resources allowlist", runtime_state->id, cfg.script);
  }
  if (!cfg.default_font.empty() &&
      std::find(cfg.resources.begin(), cfg.resources.end(), cfg.default_font) == cfg.resources.end()) {
    utils::error{}("runtime state '{}': default font '{}' is not present in resources allowlist",
                   runtime_state->id, cfg.default_font);
  }

  s.target_runtime_state = state_handle;
  s.pending_ui_resources.clear();
  s.pending_boot_targets.clear();
  s.pending_ui_scope = std::make_shared<resource_access_scope>();
  s.pending_ui_entry_script = cfg.script;
  s.pending_ui_default_font = cfg.default_font;
  s.pending_scene = cfg.scene;
  s.pending_scene_resources.clear();
  s.pending_project_scene = {};
  s.target_prepared = true;

  for (const auto& id : cfg.resources) {
    const auto handle = registry.handle(id);
    auto* resource = handle.get();
    if (resource == nullptr) {
      utils::error{}("runtime state '{}': resource '{}' was not found", runtime_state->id, id);
    }
    if (s.pending_ui_scope->contains(handle)) {
      continue;
    }

    s.pending_ui_scope->grant(handle);
    const resource_ref ref = resource_ref::from_handle(handle);
    s.pending_ui_resources.push_back(ref);
    const int32_t target = pre_external_only ? pre_external_target(*resource) : resource->final_state();
    if (pre_external_only) {
      s.pending_boot_targets.emplace_back(ref, target);
    }
    if (!br.load_resource.try_push(command_load_resource{ref, target})) {
      utils::error{}("runtime state '{}': load_resource queue overflow while requesting '{}'", runtime_state->id, id);
    }
  }

  if (!cfg.scene.empty()) {
    const auto manifest_handle = registry.handle(cfg.scene);
    auto* manifest = manifest_handle.get<scene_manifest_resource>();
    if (manifest == nullptr) {
      utils::error{}("runtime state '{}': scene manifest '{}' was not found", runtime_state->id, cfg.scene);
    }
    while (!manifest->usable()) {
      manifest->load(utils::safe_handle_t{});
    }
    s.pending_ui_scope->grant(manifest_handle);

    const auto& scene_cfg = manifest->config();
    if (!scene_cfg.project.empty()) {
      const auto project_handle = registry.handle(scene_cfg.project);
      auto* project = project_handle.get();
      if (project == nullptr) {
        utils::error{}("scene manifest '{}': project descriptor '{}' was not found", manifest->id, scene_cfg.project);
      }
      if (pre_external_target(*project) != project->final_state()) {
        utils::error{}("scene manifest '{}': project descriptor '{}' must be a CPU-only resource",
                       manifest->id, scene_cfg.project);
      }
      while (!project->usable()) {
        project->load(utils::safe_handle_t{});
      }
      s.pending_project_scene = project_handle;
      s.pending_ui_scope->grant(project_handle);
    }

    for (const auto& request : scene_cfg.resources) {
      if (request.id.empty()) {
        utils::error{}("scene manifest '{}': resource id is empty", manifest->id);
      }
      const auto handle = registry.handle(request.id);
      auto* resource = handle.get();
      if (resource == nullptr) {
        utils::error{}("scene manifest '{}': resource '{}' was not found", manifest->id, request.id);
      }
      if (std::any_of(s.pending_scene_resources.begin(), s.pending_scene_resources.end(),
                      [resource](const standard_loading_state::scene_binding& binding) {
                        return binding.resource.get() == resource;
                      })) {
        utils::error{}("scene manifest '{}': resource '{}' is requested more than once", manifest->id, request.id);
      }
      s.pending_ui_scope->grant(handle);
      s.pending_scene_resources.push_back(standard_loading_state::scene_binding{
        resource_ref::from_handle(handle), request.group, request.alias,
        scene_request_target(request, *resource), request.startup});
    }
  }
  DE_LOG(catalogue::log_domain::ui, flow,
         "runtime state target='{}' script='{}' resources={} scene='{}' scene_resources={} pre_external_only={}",
         runtime_state->id, s.pending_ui_entry_script, s.pending_ui_resources.size(), s.pending_scene,
         s.pending_scene_resources.size(), pre_external_only);
}

// Вход в loading: новая generation, сброс gate/commit, подготовка target (если ещё не готов) до
// final_state и доведение UI-allowlist до final_state. Проект после этого создаёт окно и мир.
inline void standard_begin_loading(
  standard_loading_state& s,
  demiurg::resource_system& registry,
  standard_broker& br,
  const bool external_steps_available) {
  s.state_generation += 1;
  s.target_ui_committed = false;
  s.startup_resources.clear();
  s.startup_targets.clear();

  const bool needs_final_request = s.target_prepared;
  if (!s.target_prepared) {
    if (s.target_runtime_state.get<runtime_state_resource>() == nullptr) {
      utils::error{}("loading: no valid target runtime state");
    }
    standard_prepare_runtime_state(s, registry, br, s.target_runtime_state, false);
  }

  // Тот же allowlist доводится до полной готовности. UI script не исполняется, пока каждый ресурс не
  // достиг final_state() (или первой external-ступени при отключённом render).
  for (const auto& ref : s.pending_ui_resources) {
    auto* resource = ref.get();
    if (resource == nullptr) {
      continue;
    }
    const int32_t target = external_steps_available ? resource->final_state() : pre_external_target(*resource);
    track_startup_resource(s, ref, target);
    if (needs_final_request && !br.load_resource.try_push(command_load_resource{ref, target})) {
      utils::error{}("loading: load_resource queue overflow while finalizing UI resource '{}'", resource->id);
    }
  }


  for (const auto& binding : s.pending_scene_resources) {
    auto* resource = binding.resource.get();
    if (resource == nullptr) {
      continue;
    }
    const int32_t target = external_steps_available
                             ? binding.target
                             : std::min(binding.target, pre_external_target(*resource));
    if (!br.load_resource.try_push(command_load_resource{binding.resource, target})) {
      utils::error{}("loading: load_resource queue overflow while requesting scene resource '{}'", resource->id);
    }
    if (binding.startup) {
      track_startup_resource(s, binding.resource, target);
    }
  }
}

// Зафиксировать целевой вид как активный (зовётся при старте UI). Проект отдельно сбрасывает свои
// UI-производные (шрифты и т.п.) вокруг этого вызова.
inline void standard_commit_runtime_state(standard_loading_state& s) {
  s.ui_resources = std::move(s.pending_ui_resources);
  s.ui_resource_scope = std::move(s.pending_ui_scope);
  s.ui_entry_script = std::move(s.pending_ui_entry_script);
  s.ui_default_font = std::move(s.pending_ui_default_font);
  s.current_scene = std::move(s.pending_scene);
  s.scene_resources = std::move(s.pending_scene_resources);
  s.project_scene = s.pending_project_scene;

  s.current_runtime_state = s.target_runtime_state;
  s.target_runtime_state = {};
  s.target_prepared = false;
}

// game -> loading по смене состояния. target уже резолвнут вызывающим (из своего реестра). Возврат
// false, если фаза не game / handle не runtime-состояние / состояние то же / loading уже запрошен.
inline bool standard_request_runtime_state(standard_loading_state& s, const demiurg::resource_handle target) {
  if (s.lifecycle.phase() != app_state::game) {
    return false;
  }
  if (target.get<runtime_state_resource>() == nullptr) {
    return false;
  }
  if (target.system == s.current_runtime_state.system && target.hash == s.current_runtime_state.hash) {
    return false;
  }

  if (!s.lifecycle.request_loading()) {
    return false;
  }
  s.target_runtime_state = target;
  return true;
}

// --- предикаты готовности (проект AND-ит свои мировые условия) ---

// Весь pending UI-набор готов (boot->loading gate: пора запускать UI script).
inline bool standard_ui_resources_ready(const standard_loading_state& s, const bool external_steps_available) {
  return std::all_of(s.pending_ui_resources.begin(), s.pending_ui_resources.end(),
                     [external_steps_available](const resource_ref& ref) {
                       return resource_ready(ref, external_steps_available);
                     });
}

// Boot-набор дошёл до своих pre-external целей (boot->loading gate).
inline bool standard_ui_boot_resources_prepared(const standard_loading_state& s) {
  return std::all_of(s.pending_boot_targets.begin(), s.pending_boot_targets.end(), [](const auto& entry) {
    auto* resource = entry.first.get();
    return resource != nullptr && resource->state() >= entry.second;
  });
}

// Весь стартовый allowlist достиг manifest target (ресурсная часть loading->game gate).
inline bool standard_startup_resources_ready(const standard_loading_state& s, const bool external_steps_available) {
  (void)external_steps_available;
  for (size_t i = 0; i < s.startup_resources.size(); ++i) {
    if (!resource_reached(s.startup_resources[i], s.startup_targets[i])) {
      return false;
    }
  }
  return true;
}

inline size_t standard_startup_resources_done(const standard_loading_state& s) {
  size_t done = 0;
  for (size_t i = 0; i < s.startup_resources.size(); ++i) {
    done += resource_reached(s.startup_resources[i], s.startup_targets[i]) ? 1u : 0u;
  }
  return done;
}

} // namespace simul
} // namespace devils_engine

#endif
