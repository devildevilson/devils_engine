#include <algorithm>
#include <string>
#include <vector>

#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/prefab/resource.h>
#include <devils_engine/utils/core.h>

#include "brain_config_loader.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

namespace {
template <typename T>
T& require_resource(demiurg::resource_system& resources,
                    const std::string_view id,
                    const std::string_view role) {
  auto* resource = resources.get<T>(id);
  if (resource == nullptr) {
    utils::error{}("tile_frontier: required {} resource '{}' was not found", role, id);
  }
  while (!resource->usable()) {
    resource->load(utils::safe_handle_t{});
  }
  return *resource;
}
} // namespace

namespace {
// «actor» из «goap/actor»: короткое имя мозга = id без префикса; его пишут префабы в goap=/fsm=
// и хешируют компоненты goap_ref/fsm_ref.
std::string_view brain_name(const std::string_view id, const std::string_view prefix) {
  auto name = id;
  if (name.starts_with(prefix)) {
    name.remove_prefix(prefix.size());
  }
  if (!name.empty() && name.front() == '/') {
    name.remove_prefix(1);
  }
  return name;
}
} // namespace

brain_config load_required_brain_config(
  demiurg::resource_system& resources,
  const std::string_view script_id,
  const std::string_view fsm_prefix,
  const std::string_view goap_prefix,
  const std::string_view prefab_prefix) {
  auto& script = require_resource<act::script_resource>(resources, script_id, "script");
  if (script.category() != act::category::predicate) {
    utils::error{}("tile_frontier: required actor script '{}' must be a predicate", script_id);
  }

  brain_config config;
  config.is_hungry_program = script.program();

  // ВСЕ fsm/* — каждый ресурс = отдельный именованный FSM-мозг (per-entity fsm_ref).
  std::vector<mood::fsm_resource*> fsm_resources;
  resources.filter<mood::fsm_resource>(fsm_prefix, fsm_resources);
  if (fsm_resources.empty()) {
    utils::error{}("tile_frontier: required FSM set '{}' is empty", fsm_prefix);
  }
  for (auto* resource : fsm_resources) {
    while (!resource->usable()) {
      resource->load(utils::safe_handle_t{});
    }
    if (resource->transitions().empty()) {
      utils::error{}("tile_frontier: required FSM '{}' has no transitions", resource->id);
    }
    config.fsms.push_back(named_fsm{
      std::string(brain_name(resource->id, fsm_prefix)), &resource->transitions()});
  }

  // ВСЕ goap/* — каждый flatten-ится (single-base overlay/disable) в отдельный именованный GOAP-мозг.
  // Базовые конфиги (actor_base) тоже становятся выбираемыми мозгами — это фича, не побочка.
  std::vector<acumen::goap_resource*> goap_resources;
  resources.filter<acumen::goap_resource>(goap_prefix, goap_resources);
  if (goap_resources.empty()) {
    utils::error{}("tile_frontier: required GOAP set '{}' is empty", goap_prefix);
  }
  for (auto* resource : goap_resources) {
    auto goap = std::make_shared<acumen::goap_config>(
      acumen::resolve_goap_config(resources, resource->id));
    if (goap->metrics.empty() || goap->actions.empty() || goap->goals.empty()) {
      utils::error{}("tile_frontier: required GOAP '{}' must define metrics, actions, and goals", resource->id);
    }
    config.goaps.push_back(named_goap{
      std::string(brain_name(resource->id, goap_prefix)), std::move(goap)});
  }

  std::vector<prefab::prefab_resource*> prefab_resources;
  resources.filter<prefab::prefab_resource>(prefab_prefix, prefab_resources);
  if (prefab_resources.empty()) {
    utils::error{}("tile_frontier: required prefab set '{}' is empty", prefab_prefix);
  }

  config.prefabs.reserve(prefab_resources.size());
  for (auto* resource : prefab_resources) {
    while (!resource->usable()) {
      resource->load(utils::safe_handle_t{});
    }
    config.prefabs.push_back(prefab_def{
      std::string(resource->prefab_name()), std::string(resource->text())});
  }
  std::sort(config.prefabs.begin(), config.prefabs.end(), [](const auto& a, const auto& b) {
    return a.name < b.name;
  });
  const auto has_prefab = [&config](const std::string_view name) {
    return std::any_of(config.prefabs.begin(), config.prefabs.end(), [name](const auto& prefab) {
      return prefab.name == name;
    });
  };
  if (!has_prefab("actor") || !has_prefab("food")) {
    utils::error{}("tile_frontier: prefab set '{}' must contain both 'actor' and 'food'", prefab_prefix);
  }
  return config;
}

} // namespace core
} // namespace tile_frontier
