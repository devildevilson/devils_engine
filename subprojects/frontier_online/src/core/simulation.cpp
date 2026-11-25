#include "devils_engine/utils/numeric.h"
#include <memory>

#include <devils_engine/simul/window_runtime.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>

#include "core/assets_system.h"
#include "core/broker.h"
#include "core/config.h"
#include "core/render_system.h"
#include "core/runtime.h"
#include "core/simulation.h"
#include "core/frontier_online_game.h"
#include "core/world_scene_resource.h"

namespace frontier_online {
namespace core {

using namespace devils_engine;

// Начальный шаг до чтения настроек: host_init заменяет его частотой из конфигурации движка.
constexpr size_t main_frame_time = utils::round(
  double(utils::global_time_resolution) * (1.0 / 20.0));

// Generic host state stays flat and engine-readable. All project scene/gameplay state is folded
// behind frontier_online_game; only the assets worker seam remains beside it.
struct simulation_init : public simul::standard_game_state<broker> {
  assets_simulation* assets_sim = nullptr;
  frontier_online_game game;
};

simulation::simulation(runtime_bootstrap* boot) noexcept
  : simul::game_host<simulation, runtime_bootstrap, ::frontier_online::core::broker>(
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
  // Снимок по --dump заканчивает процесс через request_quit (on_frame_captured).
  return host_stop_predicate();
}

void simulation::update(const size_t time) {
  host_update(time);
}

void simulation::workers_started() {
  host_workers_started();
  const auto& capture = bootstrap()->launch.capture;
  if (capture.wanted() && !request_frame_capture(capture.path, capture.at_frame)) {
    utils::error{}("frontier_online: the capture queue refused the --dump request at startup");
  }
}

void simulation::on_frame_captured(const simul::command_frame_captured& result) {
  const auto& capture = bootstrap()->launch.capture;
  if (!capture.wanted() || !capture.exit_after || result.path != capture.path) {
    return; // снимок клавишей или --stay: лог уже написан производителем
  }
  request_quit(result.ok ? 0 : 1);
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

void runtime_traits::init_bootstrap(bootstrap_type& boot) {
  simul::standard_app_runtime_traits<app_config_resource>::init_bootstrap(boot);
  std::string problem;
  if (!parse_launch_options(boot.arguments, boot.launch, problem)) {
    // main проверяет аргументы до запуска; сюда доходят только уже проверенные.
    utils::error{}("frontier_online: launch arguments refused after main accepted them: {}", problem);
  }
}

simul::worker_systems<runtime_traits::broker_type> runtime_traits::make_workers(
  bootstrap_type& boot) {
  return simul::make_standard_workers<render_simulation, assets_simulation, sound_simulation>(
    boot, boot.engine.app_name);
}

void simulation::project_settings_reloaded() {
  // metrics читается непосредственно каждый frame; logging/window/sound применяет generic host.
  // simulation/render/time и активная сцена остаются project topology.
}

void simulation::register_project_ui_bindings() {
  auto& state = this->state();
  state.game.register_ui_bindings(*state.ui);
}

void simulation::begin_project_loading() {
  auto& state = this->state();
  auto* descriptor = state.pending_project_scene.get<world_scene_resource>();
  if (descriptor == nullptr || !descriptor->usable()) {
    utils::error{}("frontier_online: scene manifest '{}' has no usable world descriptor",
                   state.pending_scene);
  }
  auto* resources = asset_registry();
  if (resources == nullptr) {
    utils::error{}("frontier_online: gameplay config requires the assets subsystem");
  }

  state.game.begin_scene(frontier_online_game::scene_start_context{
    .scene_id = state.pending_scene,
    .config = descriptor->config(),
    .resources = state.pending_scene_resources,
    .asset_registry = *resources,
    .messages = *state.br,
    .generation = state.state_generation,
    .viewport_width = state.fb_width,
    .viewport_height = state.fb_height,
    .assets_available = systems().assets,
    .join = bootstrap()->launch.join,
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

void simulation::begin_simulation_frame(
  const size_t time,
  const simul::phase_gate& gate) {
  auto& state = this->state();
  const auto [window_width, window_height] = state.window != nullptr
                                               ? input::window_size(state.window)
                                               : std::tuple<uint32_t, uint32_t>{1u, 1u};
  const auto [mouse_x, mouse_y] = state.window != nullptr
                                    ? input::cursor_pos(state.window)
                                    : std::tuple<double, double>{0.0, 0.0};
  state.game.begin_frame(frontier_online_game::presentation_context{
    .time = time,
    .generation = state.state_generation,
    .framebuffer_width = state.fb_width,
    .framebuffer_height = state.fb_height,
    .window_width = window_width,
    .window_height = window_height,
    .mouse_x = float(mouse_x),
    .mouse_y = float(mouse_y),
    .gate = gate,
    .settings = bootstrap()->settings,
    .messages = *state.br,
    .render_available = systems().render,
    .sound_available = systems().sound,
  });
}

void simulation::update_simulation(
  const utils::simulation_tick tick,
  const utils::game_duration game_dt,
  const simul::phase_gate& gate) {
  auto& state = this->state();
  state.game.update_simulation(frontier_online_game::simulation_context{
    .tick = tick,
    .game_delta = game_dt,
    .settings = bootstrap()->settings,
    .messages = *state.br,
    .pool = *bootstrap()->pool,
    .run_gameplay = gate.run_gameplay,
    .sound_available = systems().sound,
  });
}

void simulation::end_simulation_frame(
  const size_t time,
  const simul::phase_gate& gate) {
  auto& state = this->state();
  const auto [window_width, window_height] = state.window != nullptr
                                               ? input::window_size(state.window)
                                               : std::tuple<uint32_t, uint32_t>{1u, 1u};
  const auto [mouse_x, mouse_y] = state.window != nullptr
                                    ? input::cursor_pos(state.window)
                                    : std::tuple<double, double>{0.0, 0.0};
  state.game.end_frame(frontier_online_game::presentation_context{
    .time = time,
    .generation = state.state_generation,
    .framebuffer_width = state.fb_width,
    .framebuffer_height = state.fb_height,
    .window_width = window_width,
    .window_height = window_height,
    .mouse_x = float(mouse_x),
    .mouse_y = float(mouse_y),
    .gate = gate,
    .settings = bootstrap()->settings,
    .messages = *state.br,
    .render_available = systems().render,
    .sound_available = systems().sound,
  });
}

void simulation::on_visage_before_update() {
  auto& state = this->state();
  state.game.before_ui_update(*state.ui);
}

} // namespace core
} // namespace frontier_online
