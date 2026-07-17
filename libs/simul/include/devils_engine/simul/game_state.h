#ifndef DEVILS_ENGINE_SIMUL_GAME_STATE_H
#define DEVILS_ENGINE_SIMUL_GAME_STATE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/input/core.h>
#include <devils_engine/utils/prng.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/timeline.h>
#include <devils_engine/visage/system.h>

#include "loading_runtime.h"
#include "lua_app_bindings.h"
#include "pause.h"
#include "window_runtime.h"

namespace devils_engine {
namespace simul {

// Standard main/game-thread state consumed by game_host, window_runtime and the engine Lua/UI
// bindings. Projects inherit this container and append only their world/scene state. Broker remains
// a template because projects extend standard_broker with their own SPSC channels.
template <typename Broker>
struct standard_game_state : standard_loading_state {
  GLFWwindow* window = nullptr;
  GLFWmonitor* monitor = nullptr;
  uint32_t fb_width = 1;
  uint32_t fb_height = 1;
  window_policy policy;
  bool window_active = true;
  int32_t windowed_x = 0;
  int32_t windowed_y = 0;
  uint32_t windowed_w = 0;
  uint32_t windowed_h = 0;
  bool is_fullscreen = false;
  std::unique_ptr<input::init> in;

  std::unique_ptr<visage::system> ui;
  bool ui_logged = false;
  utils::xoshiro256starstar::state ui_rng =
    utils::xoshiro256starstar::init(utils::string_hash("visage_ui"));
  utils::timelines clocks;
  utils::calendar_clock calendar;
  pause_state pause;

  // Every active runtime-state font exposes CPU metrics to visage. The bool records that its
  // render-owned gpu_index has already been copied into font_t::texture_id.
  demiurg::resource_handle ui_font_h;
  std::vector<std::pair<demiurg::resource_handle, bool>> ui_fonts;

  // Runtime normally owns the broker; owned_br preserves the standalone main-system path.
  std::unique_ptr<Broker> owned_br;
  Broker* br = nullptr;

  std::vector<std::string> sound_devices;
  std::atomic_bool sound_devices_ready = false;
  bool sound_devices_requested = false;
  bool sound_devices_logged = false;
  std::vector<ui_sound_state_entry> sound_state;
  std::vector<ui_sound_state_entry> sound_state_next;
  size_t sound_frame = 0;

  size_t tick = 0;
};

} // namespace simul
} // namespace devils_engine

#endif
