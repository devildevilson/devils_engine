#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <Jolt/Jolt.h>

#include "devils_engine/aesthetics/common.h"

namespace devils_engine::phy03 {

constexpr int tick_rate = 60;
constexpr float tick_seconds = 1.0f / tick_rate;
constexpr uint32_t weak_push = 16;
constexpr uint32_t strong_push = 42;
constexpr uint32_t max_push = 80;
constexpr uint32_t stagger_threshold = 6;
constexpr uint32_t fall_threshold = 30;

constexpr float push_speed(const uint32_t tenths) noexcept { return float(tenths) / 10.0f; }

enum class movement_mode { walking, staggering, falling, recovering };
enum class brake_test { none, weak, strong };
enum class camera_mode { level, head, overview };
enum class part : size_t {
  pelvis, chest, head, left_arm, left_forearm, right_arm, right_forearm,
  left_thigh, left_shin, right_thigh, right_shin, count
};

constexpr size_t part_count = size_t(part::count);

struct intent {
  float forward = 0;
  float right = 0;
  float yaw_delta = 0;
  float pitch_delta = 0;
  brake_test brake = brake_test::none;
  uint32_t room_push = 0; // Instantaneous change in velocity in tenths of a metre per second.
  bool drop = false;
  bool rescue = false;
};

struct transform {
  JPH::RVec3 position = JPH::RVec3::sZero();
  JPH::Quat rotation = JPH::Quat::sIdentity();
};

enum class primitive { box, sphere, capsule };
struct geometry {
  primitive kind;
  // Box half extents, or (radius, half cylinder height, radius).
  JPH::Vec3 size;
  uint32_t color;
};

struct solid {
  geometry shape;
  transform pose;
};

enum class fixture : size_t {
  floor, step, roof, platform, crate, west_wall, east_wall, north_wall, south_wall, count
};
constexpr size_t fixture_count = size_t(fixture::count);

struct actor_snapshot {
  aesthetics::entityid_t entity = aesthetics::invalid_entityid;
  movement_mode mode = movement_mode::walking;
  std::array<transform, part_count> parts;
  transform eye;
  transform capsule;
  transform query_capsule;
  transform inner_capsule;
  float yaw = 0;
  float pitch = 0;
  float head_weight = 0;
  bool grounded = false;
  bool on_platform = false;
  bool recovery_blocked = false;
  bool stepping = false;
  uint32_t last_impact_tick = 0;
  uint32_t completed_steps = 0;
  uint32_t handoffs = 0;
  uint32_t recoveries = 0;
  uint32_t rescues = 0;
  uint32_t capsule_bodies = 0;
  uint32_t ragdoll_bodies = 0;
  JPH::Vec3 inherited_velocity = JPH::Vec3::sZero();
  JPH::Vec3 pelvis_launch_velocity = JPH::Vec3::sZero();
  JPH::Vec3 motion_velocity = JPH::Vec3::sZero();
};

struct snapshot {
  uint32_t tick = 0;
  actor_snapshot player;
  actor_snapshot dummy;
  std::array<solid, fixture_count> room;
  float platform_speed = 0;
  uint32_t brake_tick = 0;
  JPH::Vec3 crate_velocity = JPH::Vec3::sZero();
};

struct joint_error {
  float anchor_distance = 0;
  float angle = 0;
};

struct camera_pose {
  JPH::RVec3 position;
  JPH::Quat rotation;
  bool clipped = false;
};

struct jolt_runtime {
  jolt_runtime();
  ~jolt_runtime();
  jolt_runtime(const jolt_runtime&) = delete;
  jolt_runtime& operator=(const jolt_runtime&) = delete;
};

// One world on the calling thread. Presentation reads the last two fixed-tick snapshots.
// Camera casts are read-only and exclude both representations of the same actor.
class simulation {
public:
  simulation();
  ~simulation();

  void step(const intent& input);
  const snapshot& previous() const noexcept;
  const snapshot& current() const noexcept;
  camera_pose sample_camera(float alpha, camera_mode mode) const;

  joint_error measure_joints() const;
  void apply_impulse(part target, JPH::Vec3Arg impulse);
  void apply_force(part target, JPH::Vec3Arg force);
  JPH::Vec3 angular_velocity(part target) const;

private:
  struct world;
  std::unique_ptr<world> state;
};

const std::array<geometry, part_count>& ragdoll_geometry();
const geometry& query_capsule_geometry();
const geometry& inner_capsule_geometry();
std::string_view mode_name(movement_mode mode);
transform interpolate(const transform& previous, const transform& current, float alpha);

}
