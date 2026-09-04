#include "devils_engine/playground/free_camera.h"

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_transform.hpp>

namespace devils_engine::playground {

glm::vec3 free_camera::forward() const noexcept {
  const float cp = std::cos(pitch);
  return glm::normalize(glm::vec3{cp * std::cos(yaw), std::sin(pitch), cp * std::sin(yaw)});
}

glm::vec3 free_camera::right() const noexcept {
  return glm::normalize(glm::cross(forward(), glm::vec3{0.0f, 1.0f, 0.0f}));
}

void free_camera::look(const camera_motion& motion) noexcept {
  yaw += motion.look_delta.x * look_sensitivity;
  pitch = std::clamp(pitch - motion.look_delta.y * look_sensitivity, -1.55334306f, 1.55334306f);
}

glm::vec3 free_camera::displacement(const camera_motion& motion, const float dt) const noexcept {
  const float speed = move_speed * (motion.fast ? fast_multiplier : 1.0f) * std::max(dt, 0.0f);
  const auto world_up = glm::vec3{0.0f, 1.0f, 0.0f};
  glm::vec3 delta = forward() * motion.forward + right() * motion.right + world_up * motion.up;
  const float length = glm::length(delta);
  if (length > 1.0f) {
    delta /= length;
  }
  return delta * speed;
}

void free_camera::update(const camera_motion& motion, const float dt) {
  // Порядок важен и сохранён: взгляд применяется ДО движения, поэтому «вперёд» считается уже по
  // новому направлению. Потребитель, складывающий позицию сам, обязан сохранить тот же порядок.
  look(motion);
  position += displacement(motion, dt);
}

glm::mat4 free_camera::view() const noexcept {
  return glm::lookAtRH(position, position + forward(), glm::vec3{0.0f, 1.0f, 0.0f});
}

glm::mat4 infinite_reverse_z_projection(
  const float vertical_fov_radians,
  const float aspect,
  const float near_plane) noexcept {
  const float f = 1.0f / std::tan(vertical_fov_radians * 0.5f);
  glm::mat4 projection{0.0f};
  projection[0][0] = f / std::max(aspect, 0.001f);
  projection[1][1] = -f; // Vulkan framebuffer Y points down.
  projection[2][3] = -1.0f;
  projection[3][2] = std::max(near_plane, 0.0001f);
  return projection;
}

} // namespace devils_engine::playground
