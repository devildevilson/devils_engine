#ifndef TILE_FRONTIER_CORE_WORLD_SCENE_RESOURCE_H
#define TILE_FRONTIER_CORE_WORLD_SCENE_RESOURCE_H

#include <cstdint>
#include <string>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>

namespace tile_frontier {
namespace core {

// Project-owned часть scene manifest. simul грузит этот CPU-only descriptor до вызова
// begin_project_loading(), но смысл полей (чанки, actor slice, brain ids) знает только игра.
struct world_scene_config {
  std::string tile_texture_group;
  std::string sound_group;
  float tile_size = 0.0f;
  uint32_t chunk_size = 0;
  uint32_t chunks_x = 0;
  uint32_t chunks_y = 0;
  uint32_t actor_count = 0;
  float camera_half_width = 0.0f;
  std::string actor_script;
  // Префиксы НАБОРОВ мозгов: каждый ресурс под префиксом = именованный GOAP/FSM (per-entity refs
  // через префабы: goap = prey). Прежние поля actor_fsm/actor_goap (один мозг на слайс) удалены.
  std::string fsm_prefix;
  std::string goap_prefix;
  std::string prefab_prefix;
  // Микс актёрных префабов при стартовом спавне: i-й актор = cycle[i % size]. Пусто ⇒ все «actor».
  std::vector<std::string> actor_prefab_cycle;
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
