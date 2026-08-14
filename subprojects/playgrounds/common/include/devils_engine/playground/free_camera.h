#ifndef DEVILS_ENGINE_PLAYGROUND_FREE_CAMERA_H
#define DEVILS_ENGINE_PLAYGROUND_FREE_CAMERA_H

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace devils_engine::playground {

struct camera_motion {
  float forward = 0.0f;
  float right = 0.0f;
  float up = 0.0f;
  glm::vec2 look_delta{};
  bool fast = false;
};

// Shared laboratory camera: no input policy and no renderer ownership. A playground maps its own
// events into camera_motion and asks the camera only for stable world/view transforms.
class free_camera {
public:
  glm::vec3 position{0.0f, 0.0f, 4.0f};
  float yaw = -1.57079632679f;
  float pitch = 0.0f;
  float move_speed = 3.5f;
  float fast_multiplier = 3.0f;
  float look_sensitivity = 0.0022f;

  void update(const camera_motion& motion, float dt);
  glm::vec3 forward() const noexcept;
  glm::vec3 right() const noexcept;
  glm::mat4 view() const noexcept;
};

// Vulkan [0,1] depth, right-handed, infinite far plane and reversed Z (near=1, infinity=0).
glm::mat4 infinite_reverse_z_projection(float vertical_fov_radians, float aspect, float near_plane) noexcept;

} // namespace devils_engine::playground

#endif
