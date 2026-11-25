#ifndef FRONTIER_ONLINE_CORE_RUNTIME_H
#define FRONTIER_ONLINE_CORE_RUNTIME_H

// Собирает project-specific bootstrap state поверх общего simul runtime.

#include <cstddef>
#include <memory>

#include <devils_engine/simul/app_runtime.h>
#include <devils_engine/simul/boot_config.h>
#include <devils_engine/simul/main_runtime.h>

#include "core/app_config_resource.h"
#include "core/assets_system.h"
#include "core/broker.h"
#include "core/config.h"
#include "core/launch_options.h"
#include "core/render_system.h"
#include "core/simulation.h"

namespace frontier_online {
namespace core {

struct runtime_bootstrap
  : public devils_engine::simul::standard_runtime_bootstrap<app_config, user_settings> {
  // Опции клиента из аргументов запуска (init_bootstrap); системы получают свою часть при создании.
  launch_options launch;

  runtime_bootstrap() {
    engine.app_name = "frontier_online";
    // Проект знает свою топологию и свои правила мира сам. Документы движка config/engine,
    // config/project и config/debug — предварительные числа для тестов и площадок, у которых своих
    // настроек нет; в проект они не копируются, а пустой id движок не ищет.
    engine.engine_config_id.clear();
    engine.project_config_id.clear();
    engine.debug_config_id.clear();
    settings.simulation = simulation_config{
      .main_fps = 20,
      .tick_rate = 60,
      .max_steps_per_frame = 8,
      .sound_fps = 60,
      .sound_mixing = "owner",
      .sound_output_buffer_ms = 50,
      .render_fps = 60,
      .assets_fps = 60,
      .sound_enabled = true,
      .assets_enabled = true,
      .worker_threads_reserved = 4,
      .min_worker_threads = 1,
      .thread_start_gap_divisor = 4,
    };
    settings.render = render_config{
      .enabled = true,
      .headless = false,
      .config_folder = "render_config", // resources/engine/render_config
      .shader_folder = "shaders",       // resources/engine/shaders
      .cache_folder = "cache",
    };
    settings.time = world_time_rules(); // правило мира, а не настройка: модули его не меняют
  }

  void export_persisted_settings() {
    persisted_settings.window.width = settings.window.width;
    persisted_settings.window.height = settings.window.height;
    persisted_settings.window.fullscreen = settings.window.fullscreen;
    persisted_settings.window.monitor = settings.window.monitor;
    persisted_settings.sound = settings.sound;
    persisted_settings.ui = settings.ui;
  }

  void import_persisted_settings() {
    settings.window.width = persisted_settings.window.width;
    settings.window.height = persisted_settings.window.height;
    settings.window.fullscreen = persisted_settings.window.fullscreen;
    settings.window.monitor = persisted_settings.window.monitor;
    settings.sound = persisted_settings.sound;
    settings.ui = persisted_settings.ui;
  }
};

struct runtime_traits : public devils_engine::simul::standard_app_runtime_traits<app_config_resource> {
  using bootstrap_type = runtime_bootstrap;
  using broker_type = broker;
  using main_type = simulation;
  static void init_bootstrap(bootstrap_type& boot);
  static devils_engine::simul::worker_systems<broker_type> make_workers(bootstrap_type& boot);
};

using runtime = devils_engine::simul::app_runtime<runtime_traits>;

} // namespace core
} // namespace frontier_online

#endif
