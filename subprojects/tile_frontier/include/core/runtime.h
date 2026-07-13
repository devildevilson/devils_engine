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

struct runtime_bootstrap : public devils_engine::simul::standard_runtime_bootstrap<app_config> {};

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
