#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h>
#include <tavl/deserialize.h>

#include "world_scene_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

world_scene_resource::world_scene_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void world_scene_resource::load_cold(const utils::safe_handle_t&) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(module->load_text(path));
  parser.finish();

  tavl::ct_context ctx;
  config_ = world_scene_config{};
  tavl::deserialize(parser, ctx, config_);
  if (!ctx.diagnostics.empty()) {
    utils::error{}("world scene resource '{}': {} tavl diagnostics", id, ctx.diagnostics.size());
  }
  if (config_.chunk_size == 0 || config_.chunks_x == 0 || config_.chunks_y == 0 || config_.actor_count == 0) {
    utils::error{}("world scene resource '{}': chunk/grid/actor counts must be non-zero", id);
  }
}

void world_scene_resource::load_warm(const utils::safe_handle_t&) {}
void world_scene_resource::unload_hot(const utils::safe_handle_t&) {}
void world_scene_resource::unload_warm(const utils::safe_handle_t&) {
  config_ = {};
}

} // namespace core
} // namespace tile_frontier
