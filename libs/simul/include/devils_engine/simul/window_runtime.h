#ifndef DEVILS_ENGINE_SIMUL_WINDOW_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_WINDOW_RUNTIME_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#  include <unistd.h>
#endif

#include <devils_engine/bindings/lua_header.h>
#include <devils_engine/catalogue/logging.h>
#include <devils_engine/input/bindings.h>
#include <devils_engine/input/core.h>
#include <devils_engine/input/events.h>
#include <devils_engine/simul/lifecycle.h>
#include <devils_engine/simul/messages.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/prng.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/utils/timeline.h>
#include <devils_engine/visage/system.h>

namespace devils_engine {
namespace simul {

struct window_policy {
  bool draw_when_unfocused = true;
  bool draw_when_minimized = false;
  bool mute_when_unfocused = true;
  float focused_master_gain = 1.0f;
  float unfocused_master_gain = 0.0f;
};

struct ui_input_state {
  bool mouse_left = false, mouse_middle = false, mouse_right = false;
  float scroll_x = 0.0f, scroll_y = 0.0f;
  std::vector<uint32_t> text;
  bool shift = false, ctrl = false;
  bool backspace = false, del = false, enter = false, tab = false;
  bool left = false, right = false, up = false, down = false;
};

struct window_events_state {
  bool resized = false;
  uint32_t fb_w = 0, fb_h = 0;
  bool focused = true;
  bool iconified = false;
  bool state_changed = false;
};

inline ui_input_state g_ui_input;
inline window_events_state g_window_events;

inline void default_window_error_callback(int, const char* msg) noexcept {
  utils::warn("GLFW error: {}", msg);
}

inline size_t read_process_rss_bytes() {
#if defined(__linux__)
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) {
    return 0;
  }
  long total = 0, resident = 0;
  const int got = std::fscanf(f, "%ld %ld", &total, &resident);
  std::fclose(f);
  if (got != 2) {
    return 0;
  }
  return size_t(resident) * size_t(::sysconf(_SC_PAGESIZE));
#else
  return 0;
#endif
}

namespace glfw_const {
enum { release = 0,
       press = 1,
       repeat = 2 };
enum { mouse_left = 0,
       mouse_right = 1,
       mouse_middle = 2 };
enum { mod_shift = 0x0001,
       mod_control = 0x0002 };
enum {
  key_enter = 257,
  key_tab = 258,
  key_backspace = 259,
  key_delete = 261,
  key_right = 262,
  key_left = 263,
  key_down = 264,
  key_up = 265
};
} // namespace glfw_const

inline void ui_mouse_button_cb(GLFWwindow*, int button, int action, int) noexcept {
  const bool down = (action != glfw_const::release);
  switch (button) {
    case glfw_const::mouse_left: g_ui_input.mouse_left = down; break;
    case glfw_const::mouse_right: g_ui_input.mouse_right = down; break;
    case glfw_const::mouse_middle: g_ui_input.mouse_middle = down; break;
    default: break;
  }
  input::events::update_mouse_button(button, action);
}

inline void ui_scroll_cb(GLFWwindow*, double x, double y) noexcept {
  g_ui_input.scroll_x += float(x);
  g_ui_input.scroll_y += float(y);
}

inline void ui_char_cb(GLFWwindow*, unsigned int codepoint) noexcept {
  g_ui_input.text.push_back(uint32_t(codepoint));
}

inline void ui_key_cb(GLFWwindow*, int key, int scancode, int action, int mods) noexcept {
  const bool down = (action != glfw_const::release);
  g_ui_input.shift = (mods & glfw_const::mod_shift) != 0;
  g_ui_input.ctrl = (mods & glfw_const::mod_control) != 0;
  input::events::update_key(scancode, action);
  switch (key) {
    case glfw_const::key_backspace: g_ui_input.backspace = down; break;
    case glfw_const::key_delete: g_ui_input.del = down; break;
    case glfw_const::key_enter: g_ui_input.enter = down; break;
    case glfw_const::key_tab: g_ui_input.tab = down; break;
    case glfw_const::key_left: g_ui_input.left = down; break;
    case glfw_const::key_right: g_ui_input.right = down; break;
    case glfw_const::key_up: g_ui_input.up = down; break;
    case glfw_const::key_down: g_ui_input.down = down; break;
    default: break;
  }
}

inline void window_framebuffer_size_cb(GLFWwindow*, int w, int h) noexcept {
  g_window_events.fb_w = w < 0 ? 0u : uint32_t(w);
  g_window_events.fb_h = h < 0 ? 0u : uint32_t(h);
  g_window_events.resized = true;
}

inline void window_focus_cb(GLFWwindow*, int focused) noexcept {
  g_window_events.focused = (focused != 0);
  g_window_events.state_changed = true;
}

inline void window_iconify_cb(GLFWwindow*, int iconified) noexcept {
  g_window_events.iconified = (iconified != 0);
  g_window_events.state_changed = true;
}

inline void initialize_input_events(const size_t main_frame_time) {
  input::events::init();
  input::events::set_engine_tick_time(main_frame_time);
  input::events::set_long_press_duration(utils::round(double(utils::global_time_resolution) * 0.3));
  input::events::set_double_press_duration(utils::round(double(utils::global_time_resolution) * 0.25));
}

inline GLFWmonitor* resolve_monitor(const std::string_view requested) {
  GLFWmonitor* primary = input::primary_monitor();
  if (requested.empty()) {
    return primary;
  }
  for (GLFWmonitor* monitor : input::monitors()) {
    if (monitor != nullptr && input::monitor_name(monitor) == requested) {
      return monitor;
    }
  }
  utils::warn("window: monitor '{}' was not found; using primary monitor", requested);
  return primary;
}

template <typename State, typename Settings>
void apply_fullscreen(State& c, Settings& settings, bool enable);

template <typename State, typename Settings>
void create_window_and_notify_render(
  State& c,
  Settings& settings,
  const std::string& app_name,
  const size_t main_frame_time) {
  if (c.window != nullptr) {
    return;
  }

  if (!c.in) {
    c.in = std::make_unique<input::init>(&default_window_error_callback);
  }

  c.monitor = resolve_monitor(settings.window.monitor);
  settings.window.monitor = std::string(input::monitor_name(c.monitor));
  c.window = input::create_window(settings.window.width, settings.window.height, app_name);
  if (c.window == nullptr) {
    utils::error{}(
      "Could not create window '{}' {}x{}",
      app_name,
      settings.window.width,
      settings.window.height);
  }

  if (c.monitor != nullptr) {
    const auto monitor_name = input::monitor_name(c.monitor);
    utils::info("Using monitor '{}'", monitor_name);
  }

  input::set_window_callback(c.window, &ui_mouse_button_cb);
  input::set_window_callback(c.window, &ui_scroll_cb);
  input::set_window_callback(c.window, &ui_char_cb);
  input::set_window_callback(c.window, &ui_key_cb);

  input::set_framebuffer_size_callback(c.window, &window_framebuffer_size_cb);
  input::set_window_focus_callback(c.window, &window_focus_cb);
  input::set_window_iconify_callback(c.window, &window_iconify_cb);

  initialize_input_events(main_frame_time);
  if (settings.window.fullscreen) {
    apply_fullscreen(c, settings, true);
  }

  const auto [fw, fh] = input::framebuffer_size(c.window);
  if (fw != 0 && fh != 0) {
    c.fb_width = fw;
    c.fb_height = fh;
  }
  g_window_events.fb_w = c.fb_width;
  g_window_events.fb_h = c.fb_height;
  g_window_events.focused = input::window_focused(c.window);
  g_window_events.iconified = input::window_iconified(c.window);
  c.window_active = g_window_events.focused && !g_window_events.iconified;

  if (c.br) {
    c.br->window_recreation.write_slot() = command_window_recreation{
      c.window, c.monitor, settings.window.width, settings.window.height};
    c.br->window_recreation.publish();
  }
}

template <typename State, typename Settings>
void apply_fullscreen(State& c, Settings& settings, const bool enable) {
  if (c.window == nullptr) {
    return;
  }
  if (enable && !c.is_fullscreen) {
    const auto [x, y] = input::window_pos(c.window);
    const auto [w, h] = input::window_size(c.window);
    c.windowed_x = x;
    c.windowed_y = y;
    c.windowed_w = w;
    c.windowed_h = h;
    GLFWmonitor* m = c.monitor != nullptr ? c.monitor : input::primary_monitor();
    if (m == nullptr) {
      utils::warn("main: no monitor for fullscreen");
      settings.window.fullscreen = false;
      return;
    }
    settings.window.monitor = std::string(input::monitor_name(m));
    const auto [mw, mh, refresh] = input::primary_video_mode(m);
    input::set_window_monitor(c.window, m, 0, 0, mw, mh, int32_t(refresh));
    c.is_fullscreen = true;
    settings.window.fullscreen = true;
    DE_LOG(catalogue::log_domain::main, flow, "main: fullscreen on ({}x{}@{})", mw, mh, refresh);
  } else if (!enable && c.is_fullscreen) {
    const uint32_t w = c.windowed_w != 0 ? c.windowed_w : settings.window.width;
    const uint32_t h = c.windowed_h != 0 ? c.windowed_h : settings.window.height;
    input::set_window_monitor(c.window, nullptr, c.windowed_x, c.windowed_y, w, h, DEVILS_ENGINE_INPUT_DONT_CARE);
    c.is_fullscreen = false;
    settings.window.fullscreen = false;
    DE_LOG(catalogue::log_domain::main, flow, "main: fullscreen off ({}x{})", w, h);
  } else {
    settings.window.fullscreen = c.is_fullscreen;
  }
}

template <typename State, typename Settings>
void apply_window_settings(State& c, Settings& settings) {
  if (c.window == nullptr) {
    return;
  }

  GLFWmonitor* desired_monitor = resolve_monitor(settings.window.monitor);
  const bool monitor_changed = desired_monitor != nullptr && desired_monitor != c.monitor;
  c.monitor = desired_monitor;
  settings.window.monitor = std::string(input::monitor_name(c.monitor));

  if (settings.window.fullscreen) {
    if (!c.is_fullscreen) {
      apply_fullscreen(c, settings, true);
    } else if (monitor_changed) {
      const auto [mw, mh, refresh] = input::primary_video_mode(c.monitor);
      input::set_window_monitor(c.window, c.monitor, 0, 0, mw, mh, int32_t(refresh));
      DE_LOG(catalogue::log_domain::main, flow,
             "main: fullscreen monitor changed to '{}' ({}x{}@{})",
             settings.window.monitor, mw, mh, refresh);
    }
    return;
  }

  if (c.is_fullscreen) {
    apply_fullscreen(c, settings, false);
  } else {
    input::set_window_size(c.window, settings.window.width, settings.window.height);
  }
}

template <typename State, typename OnResize>
void drain_window_events(State& c, const bool sound_enabled, const bool render_enabled, OnResize&& on_resize) {
  if (c.window == nullptr) {
    return;
  }

  if (g_window_events.resized) {
    g_window_events.resized = false;
    if (g_window_events.fb_w != 0 && g_window_events.fb_h != 0) {
      c.fb_width = g_window_events.fb_w;
      c.fb_height = g_window_events.fb_h;
      on_resize(c.fb_width, c.fb_height);
      if (c.br) {
        c.br->window_resize.write_slot() = command_window_resize{c.fb_width, c.fb_height};
        c.br->window_resize.publish();
      }
      DE_LOG(catalogue::log_domain::main, flow, "main: window resized to {}x{}", c.fb_width, c.fb_height);
    }
  }

  if (g_window_events.state_changed) {
    g_window_events.state_changed = false;
    const bool focused = g_window_events.focused;
    const bool iconified = g_window_events.iconified;
    const bool active = focused && !iconified;
    c.window_active = active;

    const auto& pol = c.policy;
    const float gain = (active || !pol.mute_when_unfocused) ? pol.focused_master_gain : pol.unfocused_master_gain;
    const bool draw = active ? true : (iconified ? pol.draw_when_minimized : pol.draw_when_unfocused);

    if (sound_enabled && c.br) {
      c.br->sound_master_gain.try_push(command_sound_set_master_gain{gain});
    }
    if (render_enabled && c.br) {
      c.br->render_set_active.write_slot() = command_render_set_active{draw};
      c.br->render_set_active.publish();
    }
    DE_LOG(catalogue::log_domain::main, flow, "window focus={} iconified={} -> draw={} master_gain={:.2f}", focused, iconified, draw, gain);
  }
}

inline void poll_window_events(GLFWwindow* window, const size_t time) {
  if (window != nullptr) {
    input::poll_events();
    input::events::update(time);
  }
}

template <typename State, typename OnResize>
void begin_main_frame(
  State& c,
  const size_t time,
  const bool sound_enabled,
  const bool render_enabled,
  OnResize&& on_resize) {
  c.tick += 1;
  poll_window_events(c.window, time);
  drain_window_events(c, sound_enabled, render_enabled, std::forward<OnResize>(on_resize));
}

inline void fill_ui_input_snapshot(GLFWwindow* window, visage::input_snapshot_t& snap) {
  if (window != nullptr) {
    const auto [mx, my] = input::cursor_pos(window);
    snap.mouse_x = float(mx);
    snap.mouse_y = float(my);
  }
  snap.mouse_left = g_ui_input.mouse_left;
  snap.mouse_middle = g_ui_input.mouse_middle;
  snap.mouse_right = g_ui_input.mouse_right;
  snap.scroll_x = g_ui_input.scroll_x;
  snap.scroll_y = g_ui_input.scroll_y;
  snap.text = g_ui_input.text.data();
  snap.text_count = g_ui_input.text.size();
  snap.key_shift = g_ui_input.shift;
  snap.key_ctrl = g_ui_input.ctrl;
  snap.key_backspace = g_ui_input.backspace;
  snap.key_delete = g_ui_input.del;
  snap.key_enter = g_ui_input.enter;
  snap.key_tab = g_ui_input.tab;
  snap.key_left = g_ui_input.left;
  snap.key_right = g_ui_input.right;
  snap.key_up = g_ui_input.up;
  snap.key_down = g_ui_input.down;
}

inline void consume_ui_input_frame() {
  g_ui_input.scroll_x = 0.0f;
  g_ui_input.scroll_y = 0.0f;
  g_ui_input.text.clear();
}

template <typename State, typename BeforeUpdate>
void run_visage_frame(State& c, const size_t time, const bool render_enabled, BeforeUpdate&& before_update) {
  if (!c.ui) {
    return;
  }

  visage::input_snapshot_t snap;
  fill_ui_input_snapshot(c.window, snap);
  c.ui->input(snap);

  before_update();

  c.ui_rng = utils::xoshiro256starstar::next(c.ui_rng);
  uint64_t ui_timestamp = 0;
  if constexpr (requires { c.clocks.engine_now(); }) {
    // Engine timeline продвигается главным update даже без UI и не зависит от gameplay pause.
    ui_timestamp = c.clocks.engine_now().ticks;
  } else {
    // Совместимость для project-state, ещё не мигрировавших на utils::timelines.
    c.ui_timestamp += time;
    ui_timestamp = c.ui_timestamp;
  }
  c.ui->update(time, ui_timestamp, utils::xoshiro256starstar::value(c.ui_rng));
  c.ui->convert();

  {
    static uint64_t probe_tick = 0;
    if (++probe_tick % 20 == 0) {
      lua_State* L = c.ui->script_state().lua_state();
      const int64_t lua_kib = int64_t(lua_gc(L, LUA_GCCOUNT, 0));
      const int64_t rss_kib = int64_t(read_process_rss_bytes() / 1024);
      static int64_t prev_rss = 0, prev_lua = 0;
      DE_LOG(catalogue::log_domain::main, flow,
             "mem: RSS {} MiB (d{:+.2f}) | lua {} KiB (d{:+})",
             rss_kib / 1024, double(rss_kib - prev_rss) / 1024.0,
             lua_kib, lua_kib - prev_lua);
      prev_rss = rss_kib;
      prev_lua = lua_kib;
    }
  }

  consume_ui_input_frame();

  if (render_enabled && c.br) {
    const auto verts = c.ui->vertices();
    const auto inds = c.ui->indices();
    const auto cmds = c.ui->commands();

    static const uint64_t ui_vertices_hash = utils::string_hash("ui_vertices");
    static const uint64_t ui_indices_hash = utils::string_hash("ui_indices");
    static const uint64_t ui_commands_hash = utils::string_hash("ui_commands");
    c.br->write_buffer.write(ui_vertices_hash, verts);
    c.br->write_buffer.write(ui_indices_hash, inds);

    const uint32_t count = uint32_t(cmds.size());
    c.br->write_buffer.write(ui_commands_hash,
                             std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&count), sizeof(uint32_t)),
                             std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(cmds.data()), cmds.size() * sizeof(visage::gui_draw_command_t)));
  }

  if (!c.ui_logged) {
    DE_LOG(catalogue::log_domain::ui, flow, "visage: ui buffers — {} vtx bytes, {} idx bytes, {} draw commands",
           c.ui->vertices().size(), c.ui->indices().size(), c.ui->commands().size());
    c.ui_logged = true;
  }
}

template <typename State, typename Settings, typename QuitFn>
void install_window_lua_bindings(
  sol::table app,
  State& c,
  Settings& settings,
  const bool sound_enabled,
  QuitFn&& quit) {
  auto* cptr = &c;
  auto* settings_ptr = &settings;
  app.set_function("quit_game", std::forward<QuitFn>(quit));
  app.set_function("maximize", [cptr]() {
    if (cptr->window != nullptr) {
      input::maximize_window(cptr->window);
    }
  });
  app.set_function("restore", [cptr]() {
    if (cptr->window != nullptr) {
      input::restore_window(cptr->window);
    }
  });
  app.set_function("set_fullscreen", [cptr, settings_ptr](bool enable) {
    apply_fullscreen(*cptr, *settings_ptr, enable);
  });
  app.set_function("is_fullscreen", [cptr]() -> bool {
    return cptr->is_fullscreen;
  });

  app.set_function("set_master_volume", [cptr, settings_ptr, sound_enabled](double v) {
    if (!sound_enabled || cptr->br == nullptr) {
      return;
    }
    const float gain = float(std::clamp(v, 0.0, 1.0));
    if constexpr (requires { settings_ptr->sound.master; }) {
      settings_ptr->sound.master = gain;
    }
    cptr->policy.focused_master_gain = gain;
    cptr->br->sound_master_gain.try_push(command_sound_set_master_gain{gain});
  });

  app.set_function("set_resolution", [cptr, settings_ptr](int w, int h) {
    if (cptr->window == nullptr || w <= 0 || h <= 0) {
      return;
    }
    settings_ptr->window.width = uint32_t(w);
    settings_ptr->window.height = uint32_t(h);
    input::set_window_size(cptr->window, uint32_t(w), uint32_t(h));
  });

  app.set_function("action_pressed", [](const std::string& name) -> bool {
    return input::events::check_event(std::string_view(name), input::event_state::press_mask);
  });
  app.set_function("action_clicked", [](const std::string& name) -> bool {
    return input::events::check_event(std::string_view(name), input::event_state::click_mask);
  });
}

template <typename State>
void destroy_window_runtime(State& c) noexcept {
  if (c.window != nullptr) {
    input::destroy(c.window);
    c.window = nullptr;
  }
  c.monitor = nullptr;
  c.in.reset();
}

} // namespace simul
} // namespace devils_engine

#endif
