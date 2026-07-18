#ifndef TILE_FRONTIER_CORE_TILE_FRONTIER_GAME_H
#define TILE_FRONTIER_CORE_TILE_FRONTIER_GAME_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/loading_runtime.h>
#include <devils_engine/simul/pause.h>
#include <gtl/phmap.hpp>

#include "actor_simulation.h"
#include "config.h"
#include "texture_set.h"
#include "tile_batch.h"
#include "tile_map.h"
#include "world_scene_resource.h"

namespace devils_engine {
namespace thread {
class atomic_pool;
}
namespace visage {
class system;
}
} // namespace devils_engine

namespace tile_frontier {
namespace core {

struct broker;

// Project-only gameplay scene. It consumes generic host outputs (resolved scene bindings,
// phase gates and framebuffer size), but owns every tile_frontier policy: chunks/map/camera,
// actor phases, render snapshots, presentation sound culling and project metrics/UI values.
class tile_frontier_game {
public:
  struct scene_start_context {
    std::string_view scene_id;
    const world_scene_config& config;
    std::span<const devils_engine::simul::standard_loading_state::scene_binding> resources;
    devils_engine::demiurg::resource_system& asset_registry;
    broker& messages;
    uint64_t generation = 0;
    uint32_t viewport_width = 1;
    uint32_t viewport_height = 1;
    bool assets_available = false;
  };

  struct frame_context {
    size_t time = 0;
    uint64_t game_delta_ticks = 0;
    size_t host_tick = 0;
    uint64_t generation = 0;
    uint32_t framebuffer_width = 1;
    uint32_t framebuffer_height = 1;
    devils_engine::simul::phase_gate gate;
    const app_config& settings;
    broker& messages;
    devils_engine::thread::atomic_pool& pool;
    bool render_available = false;
    bool sound_available = false;
  };

  void begin_scene(const scene_start_context& context);
  void update(const frame_context& context);
  void framebuffer_resized(uint32_t width, uint32_t height) noexcept;

  bool loading_complete() const noexcept;
  std::pair<std::size_t, std::size_t> loading_progress() const noexcept;

  void register_ui_bindings(devils_engine::visage::system& ui);
  void before_ui_update(devils_engine::visage::system& ui) const;

private:
  void drain_loaded_chunks(broker& messages, uint64_t generation);
  // WASD-движение камеры (presentation: реальное время кадра, работает и на gameplay-паузе);
  // точка камеры клампится в бокс тайлового мира [0, world_extent_].
  void move_camera(const frame_context& context);
  void publish_camera_and_tiles(const frame_context& context);
  void update_actors(const frame_context& context);
  void publish_actor_sounds(broker& messages, bool sound_available);
  void update_metrics(const metrics_config& config, uint64_t update_us);
  void reset_metrics() noexcept;

  gtl::flat_hash_map<uint64_t, devils_engine::demiurg::resource_handle> sound_by_name_;

  texture_set textures_;
  tile_grid grid_;
  uint32_t chunk_size_ = 0;
  uint32_t chunks_x_ = 0;
  uint32_t chunks_y_ = 0;
  std::vector<bool> chunks_requested_;
  std::vector<bool> chunks_loaded_;
  uint32_t chunks_loaded_count_ = 0;
  camera2d camera_;
  glm::vec2 world_extent_{0.0f, 0.0f}; // бокс тайлового мира — кламп камеры
  tile_batch tile_batch_;
  bool tiles_logged_ = false;
  bool chunks_logged_ = false;

  actor_world_slice actors_;
  actor_batch actor_batch_;
  bool actors_logged_ = false;
  actor_metrics actors_last_metrics_;
  uint64_t metrics_frames_ = 0;
  uint64_t metrics_actor_ticks_ = 0;
  uint64_t metrics_intents_ = 0;
  uint64_t metrics_instances_ = 0;
  uint64_t metrics_actor_update_us_ = 0;
  double ui_main_fps_ = 0.0;
  double ui_intents_per_sec_ = 0.0;
  double ui_instances_per_sec_ = 0.0;
  double ui_actor_update_avg_us_ = 0.0;
  std::chrono::steady_clock::time_point metrics_last_log_ = std::chrono::steady_clock::now();
};

} // namespace core
} // namespace tile_frontier

#endif
