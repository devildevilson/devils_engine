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

brain_config load_required_brain_config(
  demiurg::resource_system& resources,
  const std::string_view script_id,
  const std::string_view fsm_id,
  const std::string_view goap_id,
  const std::string_view prefab_prefix) {
  auto& script = require_resource<act::script_resource>(resources, script_id, "script");
  if (script.category() != act::category::predicate) {
    utils::error{}("tile_frontier: required actor script '{}' must be a predicate", script_id);
  }

  auto& fsm = require_resource<mood::fsm_resource>(resources, fsm_id, "FSM");
  if (fsm.transitions().empty()) {
    utils::error{}("tile_frontier: required FSM '{}' has no transitions", fsm_id);
  }

  if (resources.get<acumen::goap_resource>(goap_id) == nullptr) {
    utils::error{}("tile_frontier: required GOAP resource '{}' was not found", goap_id);
  }
  auto goap = std::make_shared<acumen::goap_config>(
    acumen::resolve_goap_config(resources, goap_id));
  if (goap->metrics.empty() || goap->actions.empty() || goap->goals.empty()) {
    utils::error{}("tile_frontier: required GOAP '{}' must define metrics, actions, and goals", goap_id);
  }

  std::vector<prefab::prefab_resource*> prefab_resources;
  resources.filter<prefab::prefab_resource>(prefab_prefix, prefab_resources);
  if (prefab_resources.empty()) {
    utils::error{}("tile_frontier: required prefab set '{}' is empty", prefab_prefix);
  }

  brain_config config;
  config.is_hungry_program = script.program();
  config.fsm_transitions = &fsm.transitions();
  config.goap = std::move(goap);
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
