#ifndef TILE_FRONTIER_CORE_RUNTIME_H
#define TILE_FRONTIER_CORE_RUNTIME_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/app_runtime.h>

#include "assets_system.h"
#include "broker.h"
#include "render_system.h"
#include "simulation.h"
#include "sound_system.h"

namespace tile_frontier {
namespace core {

struct runtime_traits {
  using broker_type = broker;
  using main_type = simulation;
  using render_type = render_simulation;
  using assets_type = assets_simulation;
  using sound_type = sound_simulation;

  static std::unique_ptr<broker_type> make_broker() {
    return std::make_unique<broker_type>();
  }

  static std::unique_ptr<main_type> make_main() {
    return std::make_unique<main_type>();
  }

  static std::unique_ptr<sound_type> make_sound(main_type& main) {
    return main.create_sound_system();
  }

  static std::unique_ptr<render_type> make_render(main_type& main) {
    return main.create_render_system();
  }

  static std::unique_ptr<assets_type> make_assets(main_type& main) {
    return main.create_assets_system();
  }

  template <typename System>
  static void set_broker(System& system, broker_type& br) {
    system.set_broker(&br);
  }

  static void bind_systems(main_type& main, sound_type* sound, render_type* render, assets_type* assets) {
    main.bind_systems(sound, render, assets);
  }

  static void after_workers_started(main_type& main) {
    main.after_workers_started();
  }

  static size_t main_wait_mcs(const main_type&) {
    return 0;
  }

  static size_t sound_wait_mcs(const main_type& main, const sound_type& sound) {
    return main.sound_thread_wait(sound);
  }

  static size_t render_wait_mcs(const main_type& main, const render_type& render) {
    return main.render_thread_wait(render);
  }

  static size_t assets_wait_mcs(const main_type& main, const assets_type& assets) {
    return main.assets_thread_wait(assets);
  }

  static int exit_code(const main_type& main) {
    return main.exit_code();
  }
};

using runtime = devils_engine::simul::app_runtime<runtime_traits>;

}
}

#endif
