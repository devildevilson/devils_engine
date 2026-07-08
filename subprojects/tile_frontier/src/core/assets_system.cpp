#include "assets_system.h"

#include <devils_engine/demiurg/resource_system.h>

#include "messages.h"
#include "broker.h"
#include <devils_engine/painter/mesh_resource.h>
#include <devils_engine/painter/texture_resource.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/visage/font_resource.h>
#include "tile_map.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::standard_assets_system<::tile_frontier::core::broker>(frame_time) {}

void assets_simulation::register_resource_types(demiurg::resource_system& resources) {
  resources.register_type<painter::mesh_resource>("mesh", "mesh");
  // Регистрируем png-текстуру, но loading_type_id = БАЗА gpu_texture_resource: рендер/texture_set
  // работают через базу (нужен лишь gpu_index), не зная про конкретный png-декодер.
  resources.register_type<painter::gpu_texture_resource, painter::texture_resource>("textures", "png");
  // Шрифты — тоже контент (resources/modules/core/fonts/, моддятся как текстуры). Многошаговый
  // ресурс ttf→MSDF→GPU; loading_type_id = база gpu_texture_resource (рендер льёт атлас как
  // текстуру), точный тип достаётся через handle.get<visage::font_resource>() (lua/push_font).
  resources.register_type<painter::gpu_texture_resource, visage::font_resource>("fonts", "ttf");
  // Звуки — игровой контент (resources/modules/core/sounds/); тип матчится на сегмент "sounds".
  resources.register_type<sound::sound_resource>("sounds", "mp3,flac,wav,ogg,opus");
}

void assets_simulation::update_project(const size_t, ::tile_frontier::core::broker& br) {
  // mock world streaming: CPU-чанк генерируется на assets thread и возвращается main через broker.
  command_load_chunk cmd{};
  while (br.load_chunk.try_pop(cmd)) {
    if (cmd.size == 0) continue;
    const tile_chunk chunk = generate_mock_chunk(chunk_coord{cmd.x, cmd.y}, cmd.size, cmd.texture_count);
    command_chunk_loaded out;
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
