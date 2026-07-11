#include "assets_system.h"

#include "messages.h"
#include "broker.h"
#include "tile_map.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::standard_assets_system<::tile_frontier::core::broker>(frame_time) {}

void assets_simulation::update_project(const size_t, ::tile_frontier::core::broker& br) {
  // mock world streaming: CPU-чанк генерируется на assets thread и возвращается main через broker.
  command_load_chunk cmd{};
  while (br.load_chunk.try_pop(cmd)) {
    if (cmd.size == 0 || cmd.textures.empty()) continue;
    const tile_chunk chunk = generate_mock_chunk(chunk_coord{cmd.x, cmd.y}, cmd.size, cmd.textures);
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
