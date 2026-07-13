#ifndef TILE_FRONTIER_CORE_SIMULATION_H
#define TILE_FRONTIER_CORE_SIMULATION_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/lifecycle.h>
#include <devils_engine/simul/standard_sound_system.h>
#include <devils_engine/simul/systems.h>

namespace tile_frontier {
namespace core {

struct simulation_init;
struct broker;
struct runtime_bootstrap;
struct runtime_traits;
class render_simulation;
class assets_simulation;

using sound_simulation = devils_engine::simul::standard_sound_system<broker>;

struct system_presence {
  bool sound = false;
  bool render = false;
  bool assets = false;
};

// Главная/gameplay система. Runtime создаёт остальные системы и thread pool,
// а main владеет окном как поздним ресурсом и публикует данные через broker.
class simulation : public devils_engine::simul::main_system<broker> {
public:
  explicit simulation(runtime_bootstrap* boot = nullptr) noexcept;
  ~simulation() noexcept;
  void init() override;
  void set_broker(struct broker* b) override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

private:
  friend struct runtime_traits;
  friend class devils_engine::simul::lifecycle_controller;

  void bind_systems(sound_simulation* sound, render_simulation* render, assets_simulation* assets);
  void after_workers_started();
  void on_lifecycle_enter(devils_engine::simul::app_state phase);
  void on_lifecycle_tick(devils_engine::simul::app_state phase, size_t time);
  bool lifecycle_phase_complete(devils_engine::simul::app_state phase) const;
  void on_lifecycle_leave(devils_engine::simul::app_state phase);
  void begin_boot();
  bool request_runtime_state(const std::string& id);
  void start_ui();
  void begin_loading();
  int exit_code() const noexcept;
  simulation_init& state();
  const simulation_init& state() const;

  std::unique_ptr<simulation_init> container;
  runtime_bootstrap* bootstrap_ = nullptr;
  system_presence systems;

  // выход из игры (app.quit_game из UI). stop_predicate() читает его вместо тестовой заглушки.
  // atomic на случай, если позже флаг начнут выставлять не из main-потока (сейчас — из UI в main).
  std::atomic_bool quit_requested{false};
};

} // namespace core
} // namespace tile_frontier

#endif
