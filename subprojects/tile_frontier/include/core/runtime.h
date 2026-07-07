#ifndef TILE_FRONTIER_CORE_RUNTIME_H
#define TILE_FRONTIER_CORE_RUNTIME_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/app_runtime.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/thread/atomic_pool.h>

#include "assets_system.h"
#include "broker.h"
#include "config.h"
#include "render_system.h"
#include "simulation.h"
#include "sound_system.h"

namespace tile_frontier {
namespace core {

struct runtime_bootstrap {
  app_config config;

  std::unique_ptr<devils_engine::demiurg::module_system> engine_modules;
  std::unique_ptr<devils_engine::demiurg::resource_system> engine_resources;
  std::unique_ptr<devils_engine::demiurg::module_system> cache_modules;

  std::unique_ptr<devils_engine::thread::atomic_pool> pool_container;
  devils_engine::thread::atomic_pool* pool = nullptr;
};

struct runtime_traits {
  using bootstrap_type = runtime_bootstrap;
  using broker_type = broker;
  using main_type = simulation;
  using render_type = render_simulation;
  using assets_type = assets_simulation;
  using sound_type = sound_simulation;

  static std::unique_ptr<bootstrap_type> make_bootstrap();
  static void init_bootstrap(bootstrap_type& boot);
  static std::unique_ptr<broker_type> make_broker(bootstrap_type& boot);
  static std::unique_ptr<main_type> make_main(bootstrap_type& boot);
  static std::unique_ptr<sound_type> make_sound(bootstrap_type& boot);
  static std::unique_ptr<render_type> make_render(bootstrap_type& boot);
  static std::unique_ptr<assets_type> make_assets(bootstrap_type& boot);

  template <typename System>
  static void set_broker(System& system, broker_type& br) {
    system.set_broker(&br);
  }

  static void bind_systems(main_type& main, bootstrap_type& boot, sound_type* sound, render_type* render, assets_type* assets);
  static void after_workers_started(main_type& main);
  static size_t main_wait_mcs(const main_type& main);
  static size_t sound_wait_mcs(const bootstrap_type& boot, const sound_type& sound);
  static size_t render_wait_mcs(const bootstrap_type& boot, const render_type& render);
  static size_t assets_wait_mcs(const bootstrap_type& boot, const assets_type& assets);
  static int exit_code(const main_type& main);
};

using runtime = devils_engine::simul::app_runtime<runtime_traits>;

}
}

#endif
