#include "core/causal_content.h"

#include <utility>

#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/demiurg/synchronous_loader.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/originator/generator_resource.h>
#include <devils_engine/prefab/resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/platform.h>

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

namespace {

std::string directory_root(std::string value) {
  if (!value.empty() && value.back() != '/' && value.back() != '\\') value += '/';
  return value;
}

} // namespace

causal_content::causal_content(std::string resource_root, std::string module_name)
  : resource_root_(directory_root(std::move(resource_root))), module_name_(std::move(module_name)),
    modules_(resource_root_) {
  modules_.load_modules({demiurg::module_system::list_entry{module_name_ + "/", "", ""}});
  // Каталога скриптов у причинного содержимого нет: регистрировать тип значило бы пустить файлы
  // `scripts/` мода в поведение мимо content_root.
  resources_.register_type<mood::fsm_resource>(std::string(fsm_resource_type), "tavl");
  resources_.register_type<acumen::goap_resource>(std::string(goap_resource_type), "tavl", &scripts_);
  resources_.register_type<prefab::prefab_resource>(std::string(prefab_resource_type), "tavl");
  originator::register_generator_resources(resources_);
  // Причинный реестр — подмножество модуля: текстуры, звуки, интерфейс и сцены ему не нужны и не
  // должны предупреждать (их читает реестр ассетов клиента; у сервера их может не быть вовсе).
  resources_.set_registered_segments_only(true);
  resources_.parse_resources(&modules_);
  // Причинный реестр живёт без потока ассетов (сервер, тесты, и клиент держит его отдельно от
  // реестра ассетов): его владелец загружает каталоги мозгов и префабов сам, синхронно.
  demiurg::synchronous_loader loader;
  for (const auto directory : {fsm_resource_type, goap_resource_type, prefab_resource_type}) {
    loader.load_directory(resources_, std::string(directory) + "/");
  }
  config_ = load_brain_config(resources_);

  // Отпечаток считается ЗДЕСЬ, вместе с загрузкой, а не по требованию сетевого слоя: он обязан
  // описывать ровно то дерево, из которого выросли мозги, лежащие рядом в этом же объекте.
  std::string detail;
  if (!build_causal_content_root(resources_, manifest_, detail)) {
    utils::error{}("frontier_online: causal content root refused: {}", detail);
  }
}

std::string project_modules_root() {
  return utils::project_folder() + "resources/modules/";
}

// generator — id точки входа (как его объявил авторитет), берётся как есть: точка в id допустима.
bool causal_content::has_generator(const std::string& generator) const {
  return !generator.empty() && resources_.get<originator::generator_source>(generator) != nullptr;
}

std::unique_ptr<terrain_source> causal_content::make_terrain(const std::string& generator,
                                                             const uint32_t chunk_size,
                                                             const uint64_t world_seed) const {
  return std::make_unique<terrain_source>(resources_, generator, chunk_size, world_seed);
}

} // namespace core
} // namespace frontier_online
