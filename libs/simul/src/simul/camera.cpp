#include "devils_engine/simul/camera.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <devils_engine/utils/core.h>

namespace devils_engine {
namespace simul {

glm::mat4 camera2d::view_proj() const {
  // Ортопроекция мирового прямоугольника во view rect камеры. Z игнорируем (плоская карта).
  const glm::vec2 mn = view_min();
  const glm::vec2 mx = view_max();
  return glm::ortho(mn.x, mx.x, mn.y, mx.y, -1.0f, 1.0f);
}

void validate_camera_config(const camera_config& config, const std::string_view owner) {
  if (config.projection != "orthographic") {
    utils::error{}("{}: camera projection '{}' is unknown (expected 'orthographic')", owner, config.projection);
  }
  if (!(config.half_width > 0.0f)) {
    utils::error{}("{}: camera half_width must be positive, got {}", owner, config.half_width);
  }
  if (config.controller != "pan" && config.controller != "fixed") {
    utils::error{}("{}: camera controller '{}' is unknown (expected 'pan' or 'fixed')", owner, config.controller);
  }
  if (config.controller == "pan" && !(config.pan_speed > 0.0f)) {
    utils::error{}("{}: camera pan_speed must be positive for controller 'pan', got {}", owner, config.pan_speed);
  }
  if (config.limits != "world" && config.limits != "none") {
    utils::error{}("{}: camera limits '{}' are unknown (expected 'world' or 'none')", owner, config.limits);
  }
  if (config.body != "none") {
    utils::error{}("{}: camera body '{}' is not supported yet (only 'none')", owner, config.body);
  }
}

void step_camera(camera2d& camera, const camera_config& config, const glm::vec2 direction, const float dt,
                 const glm::vec2 world_min, const glm::vec2 world_max) {
  if (config.controller == "pan" && (direction.x != 0.0f || direction.y != 0.0f)) {
    const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    const float speed = camera.half_width * config.pan_speed; // мировых единиц в секунду
    camera.center += direction * (speed * dt / len);
  }
  if (config.limits == "world") {
    camera.center = glm::clamp(camera.center, world_min, world_max);
  }
}

} // namespace simul
} // namespace devils_engine
