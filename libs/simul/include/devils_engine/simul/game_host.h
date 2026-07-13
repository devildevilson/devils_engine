#ifndef DEVILS_ENGINE_SIMUL_GAME_HOST_H
#define DEVILS_ENGINE_SIMUL_GAME_HOST_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/input/core.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/utils/timeline.h>

#include "lifecycle.h"
#include "loading_runtime.h"
#include "main_runtime.h" // frame_time_from_fps + setup_logging
#include "messages.h"     // generate_task_id + command_sound_devices
#include "pause.h"
#include "standard_broker.h"
#include "startup_resources.h"
#include "systems.h"
#include "window_runtime.h"

// Общий движковый host главного/gameplay потока: владеет lifecycle-скелетом (boot→loading→game),
// runtime-состоянием загрузки, main-frame циклом (окно/ввод/визаж) и runtime-настройками, а всю
// проектную специфику выносит в набор CRTP-хуков `Derived::*`. Форма — продолжение стиля
// shared-state + свободные хелперы `simul` (window_runtime/loading_runtime duck-typed по state):
// generic-методы гоняют состояние проекта через уже существующие хелперы.
//
// Владение состоянием — у ПРОЕКТА (Derived), потому что его тип обычно определён только в .cpp
// (полный тип не виден там, где инстанцируется этот шаблон). Derived держит `state()` и разрушает
// состояние в своём деструкторе; host лишь обращается через `derived().state()`. Контракт state
// (duck-typed): поля standard_loading_state + окно (window/monitor/in/fb_width/fb_height/policy/
// window_active/is_fullscreen/windowed_*), br/owned_br, часы (clocks/calendar/pause), ui/ui_rng/
// ui_logged, tick и engine-sound поля (sound_devices/…). Проект добавляет мир/сцену рядом.
//
// Проект (Derived) реализует хуки: project_init (аллоцирует состояние + резолвит подсистемы +
// календарь), asset_registry, begin_project_loading, project_loading_complete, start_project_ui,
// on_framebuffer_resize, update_gameplay, on_visage_before_update, project_settings_reloaded.
// Разбиение движковых↔проектных lua-биндингов (шаг 3), phase-гейтинг (шаг 4) и scene-resource helper
// (шаг 5) — отдельные последующие срезы; сейчас соответствующий код живёт в проектных хуках verbatim.

namespace devils_engine {
namespace simul {

// Присутствие движковых worker-подсистем (= топология потоков из engine boot config).
struct system_presence {
  bool sound = false;
  bool render = false;
  bool assets = false;
};

template <typename Derived, typename Bootstrap, typename Broker>
class game_host : public main_system<Broker> {
public:
  explicit game_host(Bootstrap* boot, const size_t frame_time) noexcept
    : main_system<Broker>(frame_time), bootstrap_(boot) {}

  // ── generic реализации main_system/advancer контракта. НЕ virtual override: их дёргают тонкие
  //    out-of-line форвардеры Derived (simulation::init → host_init и т.д.). Так vtable и
  //    инстанцирование этих трогающих state методов остаются в TU проекта, где его тип полный
  //    (иначе inline-виртуали шаблона инстанцировались бы в TU пользователя при forward-declared state). ──
  void host_init() {
    if (bootstrap_ == nullptr) {
      utils::error{}("game_host: runtime_bootstrap is not set");
    }
    // Проект аллоцирует состояние (тип известен только ему) и резолвит подсистемы/календарь.
    derived().project_init();
    auto& c = derived().state();

    // Единый broker: приходит из runtime (set_broker до init) ИЛИ создаётся локально.
    if (this->broker() != nullptr) {
      c.br = this->broker();
    } else {
      c.owned_br = std::make_unique<Broker>();
      c.br = c.owned_br.get();
    }

    this->set_frame_time(frame_time_from_fps(bootstrap_->engine.main_fps));
    c.clocks.set_game_scale(utils::game_time_scale::from_seconds(
      bootstrap_->settings.time.game_seconds,
      bootstrap_->settings.time.real_seconds));

    // Присутствие подсистем = топология engine boot config (worker создан ⇔ флаг включён).
    systems_.sound = bootstrap_->engine.sound_enabled;
    systems_.render = bootstrap_->engine.render_enabled;
    systems_.assets = bootstrap_->engine.assets_enabled;

    // Стартовый размер фреймбуфера = из конфига (до создания окна); коллбэк ресайза уточнит.
    c.fb_width = std::max(bootstrap_->settings.window.width, 1u);
    c.fb_height = std::max(bootstrap_->settings.window.height, 1u);
  }

  void host_workers_started() {
    if (!systems_.assets) {
      utils::warn("game_host: assets subsystem is disabled, but gameplay boot requires it");
      request_quit(1);
      return;
    }
    derived().state().lifecycle.start(*this);
  }

  void host_runtime_settings_reloaded() {
    simul::setup_logging(bootstrap_->settings.logging);
    this->set_frame_time(frame_time_from_fps(bootstrap_->engine.main_fps));
    derived().state().clocks.set_game_scale(utils::game_time_scale::from_seconds(
      bootstrap_->settings.time.game_seconds,
      bootstrap_->settings.time.real_seconds));
    // calendar source/policy — проектная топология: runtime reload намеренно её не заменяет.
    derived().project_settings_reloaded();
  }

  bool host_stop_predicate() const {
    if (quit_requested_.load(std::memory_order_acquire)) {
      return true;
    }
    const auto& c = derived().state();
    return c.window != nullptr && input::should_close(c.window);
  }

  void host_update(const size_t time) {
    auto& c = derived().state();
    c.lifecycle.update(*this, time);
    const bool outside_game = c.lifecycle.phase() != app_state::game;
    c.clocks.set_game_paused(outside_game || c.pause.paused(pause_domain::gameplay));
    c.clocks.set_presentation_paused(outside_game || c.pause.paused(pause_domain::presentation));
    const auto game_before = c.clocks.game_now();
    c.clocks.advance(time);
    const uint64_t game_dt = c.clocks.game_now().ticks - game_before.ticks;

    begin_main_frame(c, time, systems_.sound, systems_.render,
                     [this](const uint32_t w, const uint32_t h) { derived().on_framebuffer_resize(w, h); });

    derived().update_gameplay(time, game_dt);

    run_visage_frame(c, time, systems_.render, [this]() { derived().on_visage_before_update(); });
  }

  int exit_code() const noexcept override {
    return app_exit_code_;
  }

  // ── lifecycle_controller host contract (публичны — контроллер их зовёт по шаблону) ──
  void on_lifecycle_enter(const app_state phase) {
    DE_LOG(catalogue::log_domain::main, flow, "lifecycle: enter {}", simul::to_string(phase));
    switch (phase) {
      case app_state::boot: begin_boot(); break;
      case app_state::loading: begin_loading(); break;
      case app_state::game: break;
    }
  }
  void on_lifecycle_tick(const app_state phase, const size_t) {
    auto& c = derived().state();
    if (phase == app_state::loading && !c.target_ui_committed && standard_ui_resources_ready(c, systems_.render)) {
      derived().start_project_ui();
      c.target_ui_committed = true;
    }
  }
  bool lifecycle_phase_complete(const app_state phase) const {
    const auto& c = derived().state();
    switch (phase) {
      case app_state::boot: return standard_ui_boot_resources_prepared(c);
      case app_state::loading:
        return c.target_ui_committed && standard_startup_resources_ready(c, systems_.render) && derived().project_loading_complete();
      case app_state::game: return false;
    }
    return false;
  }
  void on_lifecycle_leave(const app_state phase) {
    DE_LOG(catalogue::log_domain::main, flow, "lifecycle: leave {}", simul::to_string(phase));
  }

  // game→loading по смене runtime-состояния (из UI-биндинга). target резолвится из assets-реестра.
  bool request_runtime_state(const std::string& id) {
    auto* reg = derived().asset_registry();
    if (reg == nullptr) {
      return false;
    }
    return standard_request_runtime_state(derived().state(), reg->handle(id));
  }

protected:
  Derived& derived() noexcept {
    return static_cast<Derived&>(*this);
  }
  const Derived& derived() const noexcept {
    return static_cast<const Derived&>(*this);
  }
  void request_quit(const int code = 0) noexcept {
    app_exit_code_ = code;
    quit_requested_.store(true, std::memory_order_release);
  }
  Bootstrap* bootstrap() noexcept {
    return bootstrap_;
  }
  const Bootstrap* bootstrap() const noexcept {
    return bootstrap_;
  }
  const system_presence& systems() const noexcept {
    return systems_;
  }

private:
  // boot: запрос звуковых устройств (engine) + подготовка стартового runtime-состояния до
  // pre-external целей. Проектная специфика мира сюда не входит.
  void begin_boot() {
    auto& c = derived().state();
    auto* reg = derived().asset_registry();
    if (reg == nullptr) {
      utils::error{}("game_host: assets registry is required before startup resource binding");
    }

    if (systems_.sound) {
      command_sound_devices devices;
      devices.request_id = generate_task_id();
      devices.out = &c.sound_devices;
      devices.ready = &c.sound_devices_ready;
      c.br->sound_devices.try_push(devices);
      c.sound_devices_requested = true;
      DE_LOG(catalogue::log_domain::sound, flow, "main: requested sound playback devices");
    }

    const auto initial_state = standard_boot_initial_state(*reg);
    standard_prepare_runtime_state(c, *reg, *c.br, initial_state, true);
  }

  // loading: движковый каркас (generation/gate/allowlist до final) + окно, затем проектная сцена.
  void begin_loading() {
    auto& c = derived().state();
    auto* reg = derived().asset_registry();
    if (reg == nullptr) {
      utils::error{}("game_host: assets registry is required at loading");
    }
    standard_begin_loading(c, *reg, *c.br);

    // Окно появляется только после минимального boot-набора (UI + первый шрифт готовы).
    if (bootstrap_->settings.window.create_on_start && systems_.render && !bootstrap_->settings.render.headless) {
      create_window_and_notify_render(c, bootstrap_->settings, this->frame_time());
    }

    derived().begin_project_loading();
  }

  Bootstrap* bootstrap_ = nullptr;
  system_presence systems_;
  std::atomic_bool quit_requested_{false};
  int app_exit_code_ = 0;
};

} // namespace simul
} // namespace devils_engine

#endif
