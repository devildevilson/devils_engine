#ifndef TILE_FRONTIER_CORE_SIMULATION_H
#define TILE_FRONTIER_CORE_SIMULATION_H

#include <cstddef>
#include <memory>
#include <atomic>

#include <devils_engine/simul/systems.h>

#include "actors.h"

/*
что тут? тут мы наверное хотим определить систему которую запустим в main.cpp
для этой системы будет определен actor_ref и где то он должен быть глобально доступен
как же это все соединить
*/

namespace tile_frontier {
namespace core {

struct simulation_init;
struct broker;
class sound_simulation;
class render_simulation;
class assets_simulation;

// Главный/геймплейный актор-оркестратор. Поднимает остальные системы
// (sound/render/assets) + thread pool, владеет окном как поздним ресурсом
// и крутит главный цикл. Контракты с системами — через их actor_ref.
class simulation : public simul::main_system<broker> {
public:
  simulation() noexcept;
  ~simulation() noexcept;
  void init() override;
  void set_broker(struct broker* b) override;
  std::unique_ptr<sound_simulation> create_sound_system();
  std::unique_ptr<render_simulation> create_render_system();
  std::unique_ptr<assets_simulation> create_assets_system();
  void bind_systems(sound_simulation* sound, render_simulation* render, assets_simulation* assets);
  void after_workers_started();
  size_t sound_thread_wait(const sound_simulation& sound) const;
  size_t render_thread_wait(const render_simulation& render) const;
  size_t assets_thread_wait(const assets_simulation& assets) const;
  int exit_code() const noexcept;
  bool stop_predicate() const override;
  void update(const size_t time) override;
private:
  std::unique_ptr<simulation_init> container;
  simulation_actor actor;

  sound_actor* sactor;
  graphics_actor* gactor;
  assets_actor* aactor;

  // выход из игры (app.quit_game из UI). stop_predicate() читает его вместо тестовой заглушки.
  // atomic на случай, если позже флаг начнут выставлять не из main-потока (сейчас — из UI в main).
  std::atomic_bool quit_requested{false};
};

}
}

#endif
