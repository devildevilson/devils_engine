#ifndef DEVILS_ENGINE_SIMUL_GAME_HOST_H
#define DEVILS_ENGINE_SIMUL_GAME_HOST_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/input/core.h>
#include <devils_engine/visage/font.h>
#include <devils_engine/visage/font_resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/utils/timeline.h>

#include "game_state.h"
#include "lifecycle.h"
#include "loading_runtime.h"
#include "lua_app_bindings.h"
#include "lua_resource_bindings.h"
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
// состояние в своём деструкторе; host лишь обращается через `derived().state()`. Проектный state
// наследует `standard_game_state<Broker>` и добавляет мир/сцену рядом.
//
// Проект (Derived) реализует хуки: project_init (аллоцирует standard_game_state-наследника + резолвит
// подсистемы/календарь), asset_registry, begin_project_loading, project_loading_complete,
// on_framebuffer_resize, update_gameplay, on_visage_before_update, project_settings_reloaded;
// optional register_project_ui_bindings дополняет gameplay/UI API. Стандартный UI
// startup, engine Lua bindings,
// font GPU-view и sound-state merge принадлежат host; scene-resource composition остаётся проектным.

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
    // Движок централизованно решает ворота фаз (game/pause) — проект их не пересчитывает.
    const phase_gate gate = compute_phase_gate(c.lifecycle.phase(), c.pause);
    c.clocks.set_game_paused(!gate.run_gameplay);
    c.clocks.set_presentation_paused(!gate.run_presentation);
    const auto game_before = c.clocks.game_now();
    c.clocks.advance(time);
    const uint64_t game_dt = c.clocks.game_now().ticks - game_before.ticks;

    begin_main_frame(c, time, systems_.sound, systems_.render,
                     [this](const uint32_t w, const uint32_t h) { derived().on_framebuffer_resize(w, h); });

    update_engine_services();
    derived().update_gameplay(time, game_dt, gate);

    run_visage_frame(c, time, systems_.render, [this]() {
      advance_sound_state(derived().state());
      derived().on_visage_before_update();
    });
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
      start_standard_ui();
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

  // Прогресс загрузки [0,1] для UI: доля достигших target ресурсов + проектный вклад
  // (напр. mock-чанки) через хук project_loading_progress() -> {done, total}.
  double loading_progress() const {
    const auto& c = derived().state();
    const std::pair<size_t, size_t> project = derived().project_loading_progress();
    const size_t total = c.startup_resources.size() + project.second;
    if (total == 0) {
      return 1.0;
    }
    const size_t done = project.first + standard_startup_resources_done(c);
    return double(done) / double(total);
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
  void start_standard_ui() {
    auto& c = derived().state();
    auto* reg = derived().asset_registry();
    if (reg == nullptr) {
      utils::error{}("game_host: assets registry is required to start UI");
    }

    standard_commit_runtime_state(c);
    c.ui_fonts.clear();
    c.ui_font_h = {};

    for (const auto& ref : c.ui_resources) {
      const auto handle = ref.handle;
      auto* font_resource = handle.template get<visage::font_resource>();
      if (font_resource == nullptr) {
        continue;
      }
      if (font_resource->font() == nullptr) {
        utils::error{}("visage: font resource '{}' produced no font metrics", font_resource->id);
      }
      c.ui_fonts.emplace_back(handle, false);
      DE_LOG(catalogue::log_domain::ui, flow,
             "visage: font '{}' CPU-ready ({} glyphs)",
             font_resource->id,
             font_resource->font()->glyphs.size());
    }

    const std::string_view default_font_id = c.ui_default_font;
    c.ui_font_h = default_font_id.empty()
                    ? (c.ui_fonts.empty() ? demiurg::resource_handle{} : c.ui_fonts.front().first)
                    : reg->handle(default_font_id);
    auto* default_font = c.ui_font_h.template get<visage::font_resource>();
    if (default_font == nullptr || default_font->font() == nullptr ||
        !c.ui_resource_scope->contains(c.ui_font_h)) {
      utils::error{}("visage: default ui font '{}' is missing, has no metrics, or is outside the active UI scope",
                     default_font_id);
    }

    c.ui = std::make_unique<visage::system>(default_font->font());
    DE_LOG(catalogue::log_domain::ui, flow,
           "visage: system created (default font '{}', {} fonts total)",
           default_font->id,
           c.ui_fonts.size());

    auto& lua = c.ui->script_state();
    sol::environment env = c.ui->script_env();
    sol::table app = env["app"].get_or_create<sol::table>();
    install_resource_lua_bindings(lua, env, nullptr, reg, c.ui_resource_scope);
    install_sound_lua_bindings(app, c, systems_.sound);
    install_window_lua_bindings(
      app,
      c,
      bootstrap_->settings,
      systems_.sound,
      [this]() { request_quit(); });
    install_image_lua_binding(app, c);
    install_app_lifecycle_bindings(app, derived());
    if constexpr (requires { derived().register_project_ui_bindings(); }) {
      derived().register_project_ui_bindings();
    }

    sol::protected_function require = env["require"];
    const auto ret = require(c.ui_entry_script);
    if (!ret.valid()) {
      const sol::error err = ret;
      utils::error{}("visage: could not require entry module '{}': {}", c.ui_entry_script, err.what());
    }
    sol::object entry = ret.return_count() > 0 ? ret.template get<sol::object>() : sol::nil;
    c.ui->set_entry_point(entry);
    DE_LOG(catalogue::log_domain::ui, flow,
           "visage: entry point loaded from demiurg resource '{}'", c.ui_entry_script);
  }

  void update_engine_services() {
    auto& c = derived().state();
    if (c.sound_devices_requested &&
        !c.sound_devices_logged &&
        c.sound_devices_ready.load(std::memory_order_acquire)) {
      DE_LOG(catalogue::log_domain::sound, flow,
             "main: sound playback devices count {}", c.sound_devices.size());
      for (size_t i = 0; i < c.sound_devices.size(); ++i) {
        DE_LOG(catalogue::log_domain::sound, flow,
               "main: sound device[{}] '{}'", i, c.sound_devices[i]);
      }
      c.sound_devices_logged = true;
    }

    for (auto& [handle, committed] : c.ui_fonts) {
      if (committed) {
        continue;
      }
      auto* font_resource = handle.template get<visage::font_resource>();
      if (font_resource == nullptr || !font_resource->usable()) {
        continue;
      }
      if (auto* font = font_resource->font()) {
        font->set_texture_id(font_resource->gpu_index);
      }
      DE_LOG(catalogue::log_domain::ui, flow,
             "main: font atlas '{}' reached GPU (usable), texture slot={}",
             font_resource->id,
             font_resource->gpu_index);
      committed = true;
    }
  }

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
    standard_begin_loading(c, *reg, *c.br, systems_.render);

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
