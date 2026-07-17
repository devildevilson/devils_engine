#ifndef TILE_FRONTIER_CORE_WORLD_SCENE_RESOURCE_H
#define TILE_FRONTIER_CORE_WORLD_SCENE_RESOURCE_H

#include <cstdint>
#include <string>

#include <devils_engine/demiurg/resource_base.h>

namespace tile_frontier {
namespace core {

// Project-owned часть scene manifest. simul грузит этот CPU-only descriptor до вызова
// begin_project_loading(), но смысл полей (чанки, actor slice, brain ids) знает только игра.
struct world_scene_config {
  std::string tile_texture_group = "tile_textures";
  std::string sound_group = "sounds";
  float tile_size = 1.0f;
  uint32_t chunk_size = 16;
  uint32_t chunks_x = 4;
  uint32_t chunks_y = 4;
  uint32_t actor_count = 4096;
  float camera_half_width = 8.0f;
  std::string actor_script = "scripts/actor_is_hungry";
  std::string actor_fsm = "fsm/actor";
  std::string actor_goap = "goap/actor";
  std::string prefab_prefix = "prefab/";
};

class world_scene_resource : public devils_engine::demiurg::resource_interface {
public:
  world_scene_resource();

  const world_scene_config& config() const noexcept {
    return config_;
  }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  world_scene_config config_;
};

} // namespace core
} // namespace tile_frontier

#endif
