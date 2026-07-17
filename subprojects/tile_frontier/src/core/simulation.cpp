#include <memory>

#include <devils_engine/simul/window_runtime.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>

#include "assets_system.h"
#include "broker.h"
#include "config.h"
#include "render_system.h"
#include "runtime.h"
#include "simulation.h"
#include "tile_frontier_game.h"
#include "world_scene_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

constexpr size_t main_frame_time = utils::round(
  double(utils::global_time_resolution) * (1.0 / 20.0));

// Generic host state stays flat and engine-readable. All project scene/gameplay state is folded
// behind tile_frontier_game; only the assets worker seam remains beside it.
struct simulation_init : public simul::standard_game_state<broker> {
  assets_simulation* assets_sim = nullptr;
  tile_frontier_game game;
};

simulation::simulation(runtime_bootstrap* boot) noexcept
  : simul::game_host<simulation, runtime_bootstrap, ::tile_frontier::core::broker>(
      boot, main_frame_time) {}

simulation::~simulation() noexcept {
  if (container) {
    simul::destroy_window_runtime(*container);
  }
}

simulation_init& simulation::state() {
  if (!container) {
    utils::error{}("simulation: state accessed before init()");
  }
  return *container;
}

const simulation_init& simulation::state() const {
  if (!container) {
    utils::error{}("simulation: state accessed before init()");
  }
  return *container;
}

void simulation::init() {
  host_init();
}

bool simulation::stop_predicate() const {
  return host_stop_predicate();
}

void simulation::update(const size_t time) {
  host_update(time);
}

void simulation::workers_started() {
  host_workers_started();
}

void simulation::runtime_settings_reloaded() {
  host_runtime_settings_reloaded();
}

void simulation::project_init() {
  container = std::make_unique<simulation_init>();
  auto& state = *container;
  state.assets_sim = runtime_system<assets_simulation>();
  state.calendar = make_calendar_clock(bootstrap()->settings.time);
}

demiurg::resource_system* simulation::asset_registry() {
  auto& state = this->state();
  return state.assets_sim != nullptr ? state.assets_sim->resources() : nullptr;
}

simul::worker_systems<runtime_traits::broker_type> runtime_traits::make_workers(
  bootstrap_type& boot) {
  return simul::make_standard_workers<render_simulation, assets_simulation, sound_simulation>(
    boot, "tile_frontier");
}

void simulation::project_settings_reloaded() {
  // Frame time, logging and game scale are generic host settings. Calendar topology and the active
  // world scene intentionally remain fixed until an explicit runtime-state transition.
}

void simulation::register_project_ui_bindings() {
  auto& state = this->state();
  state.game.register_ui_bindings(*state.ui);
}

void simulation::begin_project_loading() {
  auto& state = this->state();
  auto* descriptor = state.pending_project_scene.get<world_scene_resource>();
  if (descriptor == nullptr || !descriptor->usable()) {
    utils::error{}("tile_frontier: scene manifest '{}' has no usable world descriptor",
                   state.pending_scene);
  }
  auto* resources = asset_registry();
  if (resources == nullptr) {
    utils::error{}("tile_frontier: gameplay config requires the assets subsystem");
  }

  state.game.begin_scene(tile_frontier_game::scene_start_context{
    .scene_id = state.pending_scene,
    .config = descriptor->config(),
    .resources = state.pending_scene_resources,
    .asset_registry = *resources,
    .messages = *state.br,
    .generation = state.state_generation,
    .viewport_width = state.fb_width,
    .viewport_height = state.fb_height,
    .assets_available = systems().assets,
  });
}

void simulation::on_framebuffer_resize(const uint32_t width, const uint32_t height) {
  state().game.framebuffer_resized(width, height);
}

bool simulation::project_loading_complete() const {
  return state().game.loading_complete();
}

std::pair<std::size_t, std::size_t> simulation::project_loading_progress() const {
  return state().game.loading_progress();
}

void simulation::update_gameplay(
  const size_t time,
  const uint64_t game_delta_ticks,
  const simul::phase_gate& gate) {
  auto& state = this->state();
  state.game.update(tile_frontier_game::frame_context{
    .time = time,
    .game_delta_ticks = game_delta_ticks,
    .host_tick = state.tick,
    .generation = state.state_generation,
    .framebuffer_width = state.fb_width,
    .framebuffer_height = state.fb_height,
    .gate = gate,
    .settings = bootstrap()->settings,
    .messages = *state.br,
    .pool = *bootstrap()->pool,
    .render_available = systems().render,
    .sound_available = systems().sound,
  });
}

void simulation::on_visage_before_update() {
  auto& state = this->state();
  state.game.before_ui_update(*state.ui);
}

} // namespace core
} // namespace tile_frontier
