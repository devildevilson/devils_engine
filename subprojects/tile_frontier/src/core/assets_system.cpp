#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/prefab/resource.h>

#include "assets_system.h"
#include "broker.h"
#include "messages.h"
#include "tile_map.h"
#include "world_scene_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::standard_assets_system<::tile_frontier::core::broker>(frame_time) {}

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
}

void assets_simulation::update_project(const size_t, ::tile_frontier::core::broker& br) {
  // mock world streaming: CPU-чанк генерируется на assets thread и возвращается main через broker.
  command_load_chunk cmd{};
  while (br.load_chunk.try_pop(cmd)) {
    if (cmd.size == 0 || cmd.textures.empty()) {
      continue;
    }
    const tile_chunk chunk = generate_mock_chunk(chunk_coord{cmd.x, cmd.y}, cmd.size, cmd.textures);
    command_chunk_loaded out;
    out.generation = cmd.generation;
    out.x = chunk.coord.x;
    out.y = chunk.coord.y;
    out.size = chunk.size;
    out.textures.reserve(chunk.tiles.size());
    for (const auto& t : chunk.tiles) {
      out.textures.push_back(t.texture);
    }
    br.chunk_loaded.try_push(std::move(out));
  }
}

} // namespace core
} // namespace tile_frontier
