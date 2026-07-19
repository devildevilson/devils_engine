#ifndef TILE_FRONTIER_CORE_RUNTIME_H
#define TILE_FRONTIER_CORE_RUNTIME_H

// Собирает project-specific bootstrap state поверх общего simul runtime.

#include <cstddef>
#include <memory>

#include <devils_engine/simul/app_runtime.h>
#include <devils_engine/simul/boot_config.h>
#include <devils_engine/simul/main_runtime.h>

#include "app_config_resource.h"
#include "assets_system.h"
#include "broker.h"
#include "config.h"
#include "render_system.h"
#include "simulation.h"

namespace tile_frontier {
namespace core {

struct runtime_bootstrap
  : public devils_engine::simul::standard_runtime_bootstrap<app_config, user_settings> {
  runtime_bootstrap() {
    engine.app_name = "tile_frontier";
  }

  void export_persisted_settings() {
    persisted_settings.window.width = settings.window.width;
    persisted_settings.window.height = settings.window.height;
    persisted_settings.window.fullscreen = settings.window.fullscreen;
    persisted_settings.window.monitor = settings.window.monitor;
    persisted_settings.sound = settings.sound;
    persisted_settings.metrics = settings.metrics;
    persisted_settings.logging = settings.logging;
    persisted_settings.ui = settings.ui;
  }

  void import_persisted_settings() {
    settings.window.width = persisted_settings.window.width;
    settings.window.height = persisted_settings.window.height;
    settings.window.fullscreen = persisted_settings.window.fullscreen;
    settings.window.monitor = persisted_settings.window.monitor;
    settings.sound = persisted_settings.sound;
    settings.metrics = persisted_settings.metrics;
    settings.logging = persisted_settings.logging;
    settings.ui = persisted_settings.ui;
  }
};

struct runtime_traits : public devils_engine::simul::standard_app_runtime_traits<app_config_resource> {
  using bootstrap_type = runtime_bootstrap;
  using broker_type = broker;
  using main_type = simulation;
  static devils_engine::simul::worker_systems<broker_type> make_workers(bootstrap_type& boot);
};

using runtime = devils_engine::simul::app_runtime<runtime_traits>;

} // namespace core
} // namespace tile_frontier

#endif
