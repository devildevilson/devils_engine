#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/log.h>
#include <tavl/deserialize.h>

#include "core/world_scene_resource.h"

namespace frontier_online {
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
    utils::warn("world scene resource '{}': {} tavl diagnostics", id, ctx.diagnostics.size());
    for (const auto& d : ctx.diagnostics) {
      utils::warn("  tavl diagnostic '{}' at {}:{} field '{}'",
                  tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
    }
    utils::error{}("world scene resource '{}': {} tavl diagnostics", id, ctx.diagnostics.size());
  }
  if (config_.chunk_size == 0 || config_.chunks_x == 0 || config_.chunks_y == 0 || config_.actor_count == 0) {
    utils::error{}("world scene resource '{}': chunk/grid/actor counts must be non-zero", id);
  }
  if (config_.tile_size <= 0.0f) {
    utils::error{}("world scene resource '{}': tile_size must be positive", id);
  }
  simul::validate_camera_config(config_.camera, "world scene resource '" + std::string(id) + "'");
  if (config_.terrain_generator.empty()) {
    utils::error{}("world scene resource '{}': terrain_generator is required — мир без генератора "
                   "земли не существует, а молчаливая пустая карта выглядела бы как загрузка", id);
  }
  if (config_.tile_texture_group.empty() || config_.sound_group.empty()) {
    utils::error{}("world scene resource '{}': tile_texture_group and sound_group are required", id);
  }
}

void world_scene_resource::load_warm(const utils::safe_handle_t&) {}
void world_scene_resource::unload_hot(const utils::safe_handle_t&) {}
void world_scene_resource::unload_warm(const utils::safe_handle_t&) {
  config_ = {};
}

} // namespace core
} // namespace frontier_online
