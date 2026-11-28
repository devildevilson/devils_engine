#pragma once

#include "simulation.h"

#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

namespace JPH {
class RagdollSettings;
}

namespace devils_engine::phy03 {

std::array<transform, part_count> animated_pose(JPH::RVec3Arg feet, float yaw, float stride, float lean);
JPH::Ref<JPH::RagdollSettings> make_recipe(JPH::ObjectLayer layer);

}
