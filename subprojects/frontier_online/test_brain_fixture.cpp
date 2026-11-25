#include <utility>

#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/synchronous_loader.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/prefab/resource.h>

#include "core/brain_config_loader.h"
#include "test_brain_fixture.h"

using namespace devils_engine;

test_brain_fixture::test_brain_fixture(std::string resource_root)
  : modules_(std::move(resource_root)) {
  modules_.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});
  resources_.register_type<act::script_resource>("scripts", "tavl", &scripts_);
  namespace fo = frontier_online::core;
  resources_.register_type<mood::fsm_resource>(std::string(fo::fsm_resource_type), "tavl");
  resources_.register_type<acumen::goap_resource>(std::string(fo::goap_resource_type), "tavl", &scripts_);
  resources_.register_type<prefab::prefab_resource>(std::string(fo::prefab_resource_type), "tavl");
  resources_.parse_resources(&modules_);
  // Потока ассетов у теста нет: владелец реестра загружает каталоги сам.
  demiurg::synchronous_loader loader;
  for (const auto directory : {fo::fsm_resource_type, fo::goap_resource_type, fo::prefab_resource_type}) {
    loader.load_directory(resources_, std::string(directory) + "/");
  }
  config_ = frontier_online::core::load_brain_config(resources_);
}
