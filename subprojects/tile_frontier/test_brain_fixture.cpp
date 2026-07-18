#include <utility>

#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/prefab/resource.h>

#include "core/brain_config_loader.h"
#include "test_brain_fixture.h"

using namespace devils_engine;

test_brain_fixture::test_brain_fixture(std::string resource_root)
  : modules_(std::move(resource_root)) {
  modules_.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});
  resources_.register_type<act::script_resource>("scripts", "tavl", &scripts_);
  resources_.register_type<mood::fsm_resource>("fsm", "tavl");
  resources_.register_type<acumen::goap_resource>("goap", "tavl", &scripts_);
  resources_.register_type<prefab::prefab_resource>("prefab", "tavl");
  resources_.parse_resources(&modules_);
  // Префиксы наборов: подхватываются ВСЕ fsm/* и goap/* мозги (как в живом scene config).
  config_ = tile_frontier::core::load_required_brain_config(
    resources_, "scripts/actor_is_hungry", "fsm", "goap", "prefab/");
}
