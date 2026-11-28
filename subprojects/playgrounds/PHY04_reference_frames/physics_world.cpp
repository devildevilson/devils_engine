#include "physics_world.h"

#include <algorithm>

#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include "devils_engine/utils/error.h"

namespace devils_engine::phy04 {

physics_world::physics_world() {
  broad.MapObjectToBroadPhaseLayer(fixed_layer, JPH::BroadPhaseLayer(0));
  broad.MapObjectToBroadPhaseLayer(hull_layer, JPH::BroadPhaseLayer(1));
  broad.MapObjectToBroadPhaseLayer(dynamic_layer, JPH::BroadPhaseLayer(1));
  pairs.EnableCollision(fixed_layer, dynamic_layer);
  pairs.EnableCollision(hull_layer, dynamic_layer);
  pairs.EnableCollision(dynamic_layer, dynamic_layer);
  broad_filter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broad, 2, pairs, 3);
  system.Init(32, 0, 256, 128, broad, *broad_filter, pairs);
  auto settings = system.GetPhysicsSettings();
  settings.mNumVelocitySteps = 16;
  settings.mNumPositionSteps = 8;
  system.SetPhysicsSettings(settings);
}

physics_world::~physics_world() {
  JPH::BodyIDVector remaining;
  system.GetBodies(remaining);
  for (const auto id : remaining) {
    if (bodies().IsAdded(id)) bodies().RemoveBody(id);
    bodies().DestroyBody(id);
  }
}

JPH::BodyID physics_world::add(const JPH::Shape* shape, const JPH::RVec3Arg position, const bool moving) {
  JPH::BodyCreationSettings settings(shape, position, JPH::Quat::sIdentity(),
    moving ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static, moving ? hull_layer : fixed_layer);
  settings.mFriction = .78f;
  settings.mAllowSleeping = !moving;
  const auto id = bodies().CreateAndAddBody(settings,
    moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  if (id.IsInvalid())
    utils::error{}("PHY04 room body at ({}, {}, {}): exhausted capacity of 32 bodies",
      position.GetX(), position.GetY(), position.GetZ());
  return id;
}

void physics_world::step(JPH::TempAllocator& scratch, JPH::JobSystem& jobs,
                         const uint32_t tick, const std::string_view name) {
  const auto error = system.Update(phy03::tick_seconds, 2, &scratch, &jobs);
  if (error != JPH::EPhysicsUpdateError::None)
    utils::error{}("PHY04 {} tick {}: Jolt update flags {}", name, tick, uint32_t(error));
}

body_state read_body(const JPH::BodyInterface& bodies, const JPH::BodyID id) {
  body_state result;
  const auto pose = bodies.GetCenterOfMassTransform(id);
  result.pose = {pose.GetTranslation(), pose.GetQuaternion()};
  bodies.GetLinearAndAngularVelocity(id, result.linear, result.angular);
  return result;
}

void accumulate(continuity& error, const body_state& first, const body_state& second) {
  error.position = std::max(error.position, float((first.pose.position - second.pose.position).Length()));
  const auto a = first.pose.rotation.GetXYZW();
  const auto b = second.pose.rotation.GetXYZW();
  error.rotation = std::max(error.rotation, std::min((a - b).Length(), (a + b).Length()));
  error.linear = std::max(error.linear, (first.linear - second.linear).Length());
  error.angular = std::max(error.angular, (first.angular - second.angular).Length());
}

void require_continuous(const continuity& error, const std::string_view event) {
  if (std::max({error.position, error.rotation, error.linear, error.angular}) > continuity_tolerance)
    utils::error{}("PHY04 {}: jumps position {}, rotation {}, linear {}, angular {}, limit {}",
      event, error.position, error.rotation, error.linear, error.angular, continuity_tolerance);
}

}
