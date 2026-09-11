#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/originator/generator_resource.h>
#include <devils_engine/prefab/resource.h>

#include "assets_system.h"
#include "broker.h"
#include "messages.h"
#include "terrain.h"
#include "world_scene_resource.h"

namespace frontier_online {
namespace core {

using namespace devils_engine;

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::standard_assets_system<::frontier_online::core::broker>(frame_time) {}

void assets_simulation::register_project_resource_types(demiurg::resource_system& resources) {
  // Owner resources receive the project's entity_scope compiler adapter; only registration stays here.
  resources.register_type<act::script_resource>("scripts", "tavl", &script_env_);
  // fsm/*.tavl -> fsm_resource (native TAVL transition rows; guard/action resolution happens later).
  resources.register_type<mood::fsm_resource>("fsm", "tavl");
  // goap/*.tavl: metric/effect expressions co-compile through the same adapter;
  // действия/цели ссылаются на метрики по ключу. Резолв ключ→функция/бит — в setup_brain_registry.
  resources.register_type<acumen::goap_resource>("goap", "tavl", &script_env_);
  // prefab/*.tavl → prefab_resource: сырой текст префаба (форму компонентов + on_construct регистрирует
  // слайс в C++, текст скармливается в prefab_registry.add_prefab). Один файл = один префаб или список //---.
  resources.register_type<prefab::prefab_resource>("prefab", "tavl");
  resources.register_type<world_scene_resource>("worlds", "tavl");
  // Генератор земли как РЕСУРС: точка входа, документы значений/буферов и тела шагов приезжают из
  // модуля, поэтому мод может переопределить мир по логическому id, не трогая C++.
  originator::register_generator_resources(resources);
}

void assets_simulation::update_project(const size_t, ::frontier_online::core::broker& br) {
  // Стриминг земли: чанк считается ЗДЕСЬ, на ассет-потоке, и уходит в main через broker.
  //
  // Источник живёт между чанками, потому что пайплайн переиспользует свои буферы — платить
  // аллокацией за каждый чанк незачем. Он же привязан к ЭТОМУ потоку: второй поток потребовал бы
  // второго источника (что законно — чанки независимы по построению).
  command_load_chunk cmd{};
  while (br.load_chunk.try_pop(cmd)) {
    if (cmd.chunk_size == 0 || cmd.generator.empty()) {
      continue;
    }
    const auto* source = terrain_for(cmd);
    if (source == nullptr) {
      continue;
    }

    const tile_chunk chunk = terrain_->generate(chunk_coord{cmd.x, cmd.y});
    command_chunk_loaded out;
    out.generation = cmd.generation;
    out.x = chunk.coord.x;
    out.y = chunk.coord.y;
    out.size = chunk.size;
    out.terrain.reserve(chunk.tiles.size());
    for (const auto& t : chunk.tiles) {
      out.terrain.push_back(t.terrain);
    }
    br.chunk_loaded.try_push(std::move(out));
  }
}

// Источник под мир, названный запросом. Пересоздаётся только при СМЕНЕ мира: пересборка пайплайна
// на каждый чанк была бы разорительной, а держать источник от прошлой сцены — значит молча считать
// не тот мир.
const terrain_source* assets_simulation::terrain_for(const command_load_chunk& request) {
  if (terrain_ != nullptr && terrain_id_ == request.generator &&
      terrain_->chunk_size() == request.chunk_size &&
      terrain_->world_seed() == request.world_seed) {
    return terrain_.get();
  }

  auto* registry = resources();
  if (registry == nullptr) {
    return nullptr;
  }

  terrain_ = std::make_unique<terrain_source>(*registry, request.generator, request.chunk_size,
                                              request.world_seed);
  terrain_id_ = request.generator;
  DE_LOG(catalogue::log_domain::assets, flow,
         "assets: terrain '{}' ready, chunk {}x{}, seed {}, world fingerprint {:#x}",
         terrain_id_, request.chunk_size, request.chunk_size, request.world_seed,
         terrain_->fingerprint());
  return terrain_.get();
}

uint64_t assets_simulation::world_fingerprint() const noexcept {
  return terrain_ != nullptr ? terrain_->fingerprint() : 0;
}

} // namespace core
} // namespace frontier_online
