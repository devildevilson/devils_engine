#include "assets_system.h"

#include <devils_engine/demiurg/resource_system.h>

#include "messages.h"
#include "broker.h"
#include "tile_map.h"
#include "script_resource.h"
#include "fsm_resource.h"
#include "goap_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::standard_assets_system<::tile_frontier::core::broker>(frame_time) {}

void assets_simulation::register_project_resource_types(demiurg::resource_system& resources) {
  // scripts/*.tavl → script_resource, компилируется через script_env_.sys (натив-функции уже в нём).
  resources.register_type<script_resource>("scripts", "tavl", &script_env_.sys);
  // fsm/*.tavl → fsm_resource (строки переходов mood; резолв гвардов/действий — в setup_brain_registry).
  resources.register_type<fsm_resource>("fsm", "tavl");
  // goap/*.tavl → goap_resource: метрики инлайн-компилируются через script_env_.sys (co-parse);
  // действия/цели ссылаются на метрики по ключу. Резолв ключ→функция/бит — в setup_brain_registry.
  resources.register_type<goap_resource>("goap", "tavl", &script_env_.sys);
}

void assets_simulation::update_project(const size_t, ::tile_frontier::core::broker& br) {
  // mock world streaming: CPU-чанк генерируется на assets thread и возвращается main через broker.
  command_load_chunk cmd{};
  while (br.load_chunk.try_pop(cmd)) {
    if (cmd.size == 0 || cmd.textures.empty()) continue;
    const tile_chunk chunk = generate_mock_chunk(chunk_coord{cmd.x, cmd.y}, cmd.size, cmd.textures);
    command_chunk_loaded out;
    out.generation = cmd.generation;
    out.x = chunk.coord.x;
    out.y = chunk.coord.y;
    out.size = chunk.size;
    out.textures.reserve(chunk.tiles.size());
    for (const auto& t : chunk.tiles) out.textures.push_back(t.texture);
    br.chunk_loaded.try_push(std::move(out));
  }
}

}
}
