#ifndef DEVILS_ENGINE_SIMUL_CAMERA_INPUT_H
#define DEVILS_ENGINE_SIMUL_CAMERA_INPUT_H

#include <glm/glm.hpp>

#include <devils_engine/input/events.h>

// Ввод контроллера камеры "pan": named actions camera_up/down/left/right из стандартной раскладки.
// Отдельно от camera.h, потому что тянет input (как game_host.h); скомпилированная часть simul от
// input не зависит.

namespace devils_engine {
namespace simul {

// Направление pan (не нормировано). Экранный «верх» — это -y мира (Vulkan clip-Y вниз, ortho это не
// компенсирует).
inline glm::vec2 camera_pan_direction() {
  static const auto up = input::events::make_event_id("camera_up");
  static const auto left = input::events::make_event_id("camera_left");
  static const auto down = input::events::make_event_id("camera_down");
  static const auto right = input::events::make_event_id("camera_right");
  glm::vec2 dir{0.0f, 0.0f};
  dir.y -= input::events::is_pressed(up) ? 1.0f : 0.0f;
  dir.y += input::events::is_pressed(down) ? 1.0f : 0.0f;
  dir.x -= input::events::is_pressed(left) ? 1.0f : 0.0f;
  dir.x += input::events::is_pressed(right) ? 1.0f : 0.0f;
  return dir;
}

} // namespace simul
} // namespace devils_engine

#endif
