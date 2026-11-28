#pragma once

#include "frames.h"

#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace devils_engine::phy04 {

constexpr JPH::ObjectLayer fixed_layer = 0;
constexpr JPH::ObjectLayer hull_layer = 1;
constexpr JPH::ObjectLayer dynamic_layer = 2;
constexpr float continuity_tolerance = .0002f;

struct physics_world {
  JPH::BroadPhaseLayerInterfaceTable broad{3, 2};
  JPH::ObjectLayerPairFilterTable pairs{3};
  std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broad_filter;
  JPH::PhysicsSystem system;

  physics_world();
  ~physics_world();
  JPH::BodyInterface& bodies() { return system.GetBodyInterface(); }
  const JPH::BodyInterface& bodies() const { return system.GetBodyInterface(); }

  JPH::BodyID add(const JPH::Shape* shape, JPH::RVec3Arg position, bool moving);
  void step(JPH::TempAllocator& scratch, JPH::JobSystem& jobs, uint32_t tick, std::string_view name);
};

body_state read_body(const JPH::BodyInterface& bodies, JPH::BodyID id);
void accumulate(continuity& error, const body_state& first, const body_state& second);
void require_continuous(const continuity& error, std::string_view event);

}
