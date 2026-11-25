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

brain_config load_required_brain_config(
  demiurg::resource_system& resources,
  const std::string_view script_id,
  const std::string_view fsm_set,
  const std::string_view goap_set,
  const std::string_view prefab_set) {
  // Наборы мозгов и префабов — каталоги. Ресурсы грузит не этот код: в контексте с потоком
  // ассетов их запрашивает состояние загрузки (каталог в манифесте сцены, prepare = true), без
  // потока — synchronous_loader владельца реестра.
  const std::string_view fsm_prefix = fsm_set;
  const std::string_view goap_prefix = goap_set;
  const std::string_view prefab_prefix = prefab_set;
  auto& script = resources.require<act::script_resource>(script_id, "tile_frontier actor script");
  if (script.category() != act::category::predicate) {
    utils::error{}("tile_frontier: required actor script '{}' must be a predicate", script_id);
  }

  brain_config config;
  config.is_hungry_program = script.program();

  // ВСЕ fsm/* — каждый ресурс = отдельный именованный FSM-мозг (per-entity fsm_ref).
  std::vector<mood::fsm_resource*> fsm_resources;
  resources.directory<mood::fsm_resource>(fsm_prefix, fsm_resources);
  if (fsm_resources.empty()) {
    utils::error{}("tile_frontier: required FSM set '{}' is empty", fsm_prefix);
  }
  for (auto* resource : fsm_resources) {
    demiurg::resource_system::require_usable(*resource, "tile_frontier FSM");
    if (resource->transitions().empty()) {
      utils::error{}("tile_frontier: required FSM '{}' has no transitions", resource->id);
    }
    config.fsms.push_back(named_fsm{
      std::string(demiurg::resource_system::name_in_directory(resource->id, fsm_prefix)), &resource->transitions()});
  }

  // ВСЕ goap/* — каждый flatten-ится (single-base overlay/disable) в отдельный именованный GOAP-мозг.
  // Базовые конфиги (actor_base) тоже становятся выбираемыми мозгами — это фича, не побочка.
  std::vector<acumen::goap_resource*> goap_resources;
  resources.directory<acumen::goap_resource>(goap_prefix, goap_resources);
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
      std::string(demiurg::resource_system::name_in_directory(resource->id, goap_prefix)), std::move(goap)});
  }

  std::vector<prefab::prefab_resource*> prefab_resources;
  resources.directory<prefab::prefab_resource>(prefab_prefix, prefab_resources);
  if (prefab_resources.empty()) {
    utils::error{}("tile_frontier: required prefab set '{}' is empty", prefab_prefix);
  }

  config.prefabs.reserve(prefab_resources.size());
  for (auto* resource : prefab_resources) {
    demiurg::resource_system::require_usable(*resource, "tile_frontier prefab");
    config.prefabs.push_back(prefab_def{
      std::string(resource->prefab_name()), std::string(resource->text())});
  }
  std::sort(config.prefabs.begin(), config.prefabs.end(), [](const auto& a, const auto& b) {
    return a.name < b.name;
  });
  // prefab_registry::add_prefab заменяет одноимённый префаб: при повторе имени выжил бы тот, кого
  // std::sort поставил последним среди равных, — а это решает реализация STL, не содержимое.
  if (const auto dup = std::adjacent_find(config.prefabs.begin(), config.prefabs.end(),
                                          [](const auto& a, const auto& b) { return a.name == b.name; });
      dup != config.prefabs.end()) {
    utils::error{}("tile_frontier: prefab set '{}' declares prefab '{}' more than once", prefab_prefix,
                   dup->name);
  }
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
