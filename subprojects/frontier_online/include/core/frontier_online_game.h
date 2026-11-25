#ifndef FRONTIER_ONLINE_CORE_FRONTIER_ONLINE_GAME_H
#define FRONTIER_ONLINE_CORE_FRONTIER_ONLINE_GAME_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/loading_runtime.h>
#include <devils_engine/simul/pause.h>
#include <gtl/phmap.hpp>

#include "core/actor_batch.h"
#include "core/actor_simulation.h"
#include "core/config.h"
#include "core/net_client.h"
#include "core/texture_set.h"
#include "core/tile_batch.h"
#include "core/tile_map.h"
#include "core/world_scene_resource.h"

namespace devils_engine {
namespace thread {
class atomic_pool;
}
namespace visage {
class system;
}
} // namespace devils_engine

namespace frontier_online {
namespace core {

struct broker;
class net_client;
struct session_world;

// Project-only gameplay scene. It consumes generic host outputs (resolved scene bindings,
// phase gates and framebuffer size), but owns every frontier_online policy: chunks/map/camera,
// actor phases, render snapshots, presentation sound culling and project metrics/UI values.
class frontier_online_game {
public:
  frontier_online_game();
  ~frontier_online_game();
  frontier_online_game(const frontier_online_game&) = delete;
  frontier_online_game& operator=(const frontier_online_game&) = delete;

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
    // `--join` из опций запуска; пустой endpoint — локальная симуляция.
    join_request_t join;
  };

  struct presentation_context {
    size_t time = 0;
    uint64_t generation = 0;
    uint32_t framebuffer_width = 1;
    uint32_t framebuffer_height = 1;
    uint32_t window_width = 1;
    uint32_t window_height = 1;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    devils_engine::simul::phase_gate gate;
    const app_config& settings;
    broker& messages;
    bool render_available = false;
    bool sound_available = false;
  };

  struct simulation_context {
    devils_engine::utils::simulation_tick tick{};
    devils_engine::utils::game_duration game_delta{};
    const app_config& settings;
    broker& messages;
    devils_engine::thread::atomic_pool& pool;
    bool run_gameplay = false;
    bool sound_available = false;
  };

  void begin_scene(const scene_start_context& context);
  void begin_frame(const presentation_context& context);
  void update_simulation(const simulation_context& context);
  void end_frame(const presentation_context& context);
  void framebuffer_resized(uint32_t width, uint32_t height) noexcept;

  bool loading_complete() const noexcept;
  std::pair<std::size_t, std::size_t> loading_progress() const noexcept;

  void register_ui_bindings(devils_engine::visage::system& ui);
  void before_ui_update(devils_engine::visage::system& ui) const;

private:
  struct lua_bindings; // Lua-биндинги (реализация в .cpp)
  void drain_loaded_chunks(broker& messages, uint64_t generation);
  // Доложить очередные запросы чанков, сколько примет очередь (её ёмкость меньше большого мира).
  void request_chunks(broker& messages);
  // Сетка из chunks_x × chunks_y чанков; request — сразу заказать их генерацию.
  void build_world(broker& messages, uint32_t chunk_size, uint32_t chunks_x, uint32_t chunks_y,
                   uint64_t world_seed, const std::string& generator, bool request);
  void build_session_world(broker& messages, const session_world& world);
  // WASD-движение камеры: скорость выражена в реальном времени кадра, но изменение разрешено
  // только пока active-gameplay timeline идёт; точка клампится в [0, world_extent_].
  void move_camera(const presentation_context& context);
  void collect_player_intents(const presentation_context& context);
  void publish_camera_and_tiles(const presentation_context& context);
  void publish_actor_snapshot(const presentation_context& context);
  void publish_sound_listener(broker& messages, bool sound_available);
  void update_actors(const simulation_context& context);
  // Живая сессия (`--join`): ведомый вместо локальной симуляции; actor_batch_ читает мир его
  // проверенного тика.
  void update_network(const simulation_context& context);
  void publish_actor_sounds(broker& messages, bool sound_available,
                            std::span<const sound_emit> emits);
  void update_metrics(const metrics_config& config, bool active);
  void reset_metrics() noexcept;

  gtl::flat_hash_map<uint64_t, devils_engine::demiurg::resource_handle> sound_by_name_;

  texture_set textures_;
  tile_grid grid_;
  uint32_t chunk_size_ = 0;
  uint32_t chunks_x_ = 0;
  uint32_t chunks_y_ = 0;
  // Заказ чанков текущей постройки сетки: следующий не доложенный в очередь и мир, которому он
  // принадлежит. `world_build_` метит чанки, чтобы постройка нового мира сессии не засчитала
  // чанки прежнего, ещё стоящие в очереди.
  uint64_t world_build_ = 0;
  size_t next_chunk_request_ = 0;
  size_t chunk_request_count_ = 0;
  uint64_t chunk_request_seed_ = 0;
  std::string chunk_request_generator_;
  std::vector<bool> chunks_loaded_;
  uint32_t chunks_loaded_count_ = 0;
  camera2d camera_;
  devils_engine::simul::camera_config camera_config_; // тип камеры сцены
  // Опубликованные камеры, которые рендер ещё может показать: тайлы отбираются по их объединению.
  devils_engine::simul::published_cameras published_cameras_;
  camera2d last_published_camera_{{0.0f, 0.0f}, 0.0f, 0.0f}; // последняя ушедшая в рендер (пауза шлёт только изменения)
  glm::vec2 world_extent_{0.0f, 0.0f}; // бокс тайлового мира — кламп камеры
  tile_batch tile_batch_;
  bool tiles_logged_ = false;
  bool chunks_logged_ = false;

  actor_world_slice actors_;
  join_request_t join_;                 // опции сессии сцены (из опций запуска)
  bool networked_ = false;              // клиент запущен с --join: локальный мир не считается
  bool net_refused_ = false;            // connect отказал (причина в логе): сцена остаётся пустой
  std::unique_ptr<net_client> net_;     // создаётся на первом тике: ему нужен пул кадра
  uint64_t net_shown_tick_ = 0;         // тик, который сейчас лежит в actor_batch_
  uint64_t scene_generation_ = 0;       // поколение сцены: метка заказанных чанков
  bool assets_available_ = false;       // есть ли кому генерировать чанки (без ассетов — нет)
  bool net_world_built_ = false;        // сетка сессии построена по раскладке авторитета
  // Раскладка, по которой построена сетка: повторный вход может попасть в другую сессию (сервер
  // перезапущен с другим зерном или боксом), и тогда сетка строится заново.
  std::unique_ptr<session_world> net_world_;
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
} // namespace frontier_online

#endif
