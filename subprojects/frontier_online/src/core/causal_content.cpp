#include "core/causal_content.h"

#include <utility>

#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/originator/generator_resource.h>
#include <devils_engine/prefab/resource.h>
#include <devils_engine/utils/core.h>

#include "core/brain_config_loader.h"

namespace frontier_online {
namespace core {

namespace acumen = devils_engine::acumen;
namespace act = devils_engine::act;
namespace demiurg = devils_engine::demiurg;
namespace mood = devils_engine::mood;
namespace originator = devils_engine::originator;
namespace prefab = devils_engine::prefab;
namespace utils = devils_engine::utils;

causal_content::causal_content(std::string resource_root, std::string module_name)
  : resource_root_(std::move(resource_root)), module_name_(std::move(module_name)),
    modules_(resource_root_) {
  modules_.load_modules({demiurg::module_system::list_entry{module_name_ + "/", "", ""}});
  resources_.register_type<act::script_resource>("scripts", "tavl", &scripts_);
  resources_.register_type<mood::fsm_resource>("fsm", "tavl");
  resources_.register_type<acumen::goap_resource>("goap", "tavl", &scripts_);
  resources_.register_type<prefab::prefab_resource>("prefab", "tavl");
  originator::register_generator_resources(resources_);
  resources_.parse_resources(&modules_);
  config_ = load_required_brain_config(resources_, "scripts/actor_is_hungry", "fsm", "goap",
                                       "prefab/");

  // Отпечаток считается ЗДЕСЬ, вместе с загрузкой, а не по требованию сетевого слоя: он обязан
  // описывать ровно то дерево, из которого выросли мозги, лежащие рядом в этом же объекте.
  std::string detail;
  if (!build_causal_content_root(resource_root_, module_name_, manifest_, detail)) {
    utils::error{}("frontier_online: causal content root refused: {}", detail);
  }
}

std::unique_ptr<terrain_source> causal_content::make_terrain(const std::string& generator,
                                                             const uint32_t chunk_size,
                                                             const uint64_t world_seed) const {
  return std::make_unique<terrain_source>(resources_, generator, chunk_size, world_seed);
}

} // namespace core
} // namespace frontier_online
