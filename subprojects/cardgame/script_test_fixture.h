#ifndef DEVILS_ENGINE_CARDGAME_SCRIPT_TEST_FIXTURE_H
#define DEVILS_ENGINE_CARDGAME_SCRIPT_TEST_FIXTURE_H

#include <stdexcept>
#include <string>
#include <string_view>

#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/utils/core.h>

#include "cardgame/combat_script.h"

namespace cardgame::test {

// Loads the shipped CPU-only combat scripts exactly as a project assets system would. Tests use
// the same external catalogue for fresh combats and resumed snapshots.
struct script_fixture {
  core::combat_effect_script_compiler compiler;
  devils_engine::demiurg::module_system modules;
  devils_engine::demiurg::resource_system resources;
  core::combat_effect_script_resources scripts;

  explicit script_fixture(const std::string_view root = CARDGAME_RESOURCE_ROOT)
    : modules(std::string(root)), scripts(resources) {
    modules.load_modules({devils_engine::demiurg::module_system::list_entry{"core/", "", ""}});
    resources.register_type<devils_engine::act::script_resource>(
      "scripts", "tavl", &compiler);
    resources.parse_resources(&modules);
    load("scripts/scripted_strike");
    load("scripts/scripted_guard");
    load("scripts/thorns_retaliation");
  }

private:
  void load(const std::string_view id) {
    auto* resource = resources.get<devils_engine::act::script_resource>(id);
    if (resource == nullptr) {
      throw std::runtime_error("cardgame test script resource is missing");
    }
    resource->load(devils_engine::utils::safe_handle_t{});
    if (resource->category() != devils_engine::act::category::effect ||
        resource->program()->cmds.empty()) {
      throw std::runtime_error("cardgame test script resource did not compile");
    }
  }
};

} // namespace cardgame::test

#endif
