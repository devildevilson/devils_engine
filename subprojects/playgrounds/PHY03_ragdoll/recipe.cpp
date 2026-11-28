#include "recipe.h"

#include <cmath>

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>

#include "devils_engine/utils/error.h"

namespace devils_engine::phy03 {
namespace {

using JPH::Vec3;

struct joint_recipe {
  const char* name;
  int parent;
  Vec3 anchor_parent;
  Vec3 anchor_child;
  float mass;
  float hinge_sign;
  float swing_or_hinge_degrees;
  float twist_degrees;
};

const std::array<joint_recipe, part_count> joints{{
  {"pelvis", -1, {0, 0, 0}, {0, 0, 0}, 12, 0, 0, 0},
  {"chest", 0, {0, .12f, 0}, {0, -.20f, 0}, 19, 0, 30, 24},
  {"head", 1, {0, .22f, 0}, {0, -.18f, 0}, 5, 0, 43, 45},
  {"left arm", 1, {0, .14f, -.235f}, {0, .14f, 0}, 3, 0, 75, 50},
  {"left forearm", 3, {0, -.15f, 0}, {0, .125f, 0}, 2, 1, 135, 0},
  {"right arm", 1, {0, .14f, .235f}, {0, .14f, 0}, 3, 0, 75, 50},
  {"right forearm", 5, {0, -.15f, 0}, {0, .125f, 0}, 2, 1, 135, 0},
  {"left thigh", 0, {0, -.10f, -.115f}, {0, .195f, 0}, 8, 0, 66, 38},
  {"left shin", 7, {0, -.205f, 0}, {0, .18f, 0}, 4, -1, 115, 0},
  {"right thigh", 0, {0, -.10f, .115f}, {0, .195f, 0}, 8, 0, 66, 38},
  {"right shin", 9, {0, -.205f, 0}, {0, .18f, 0}, 4, -1, 115, 0},
}};

constexpr uint32_t cloth = 0x427d9c;
constexpr uint32_t leg = 0x344956;
constexpr uint32_t skin = 0xe7a978;
const std::array<geometry, part_count> shapes{{
  {primitive::box, {.123f, .115f, .192f}, leg},
  {primitive::box, {.135f, .205f, .172f}, cloth},
  {primitive::sphere, {.145f, 0, .145f}, skin},
  {primitive::capsule, {.070f, .080f, .070f}, cloth},
  {primitive::capsule, {.059f, .085f, .059f}, skin},
  {primitive::capsule, {.070f, .080f, .070f}, cloth},
  {primitive::capsule, {.059f, .085f, .059f}, skin},
  {primitive::capsule, {.089f, .110f, .089f}, leg},
  {primitive::capsule, {.075f, .108f, .075f}, leg},
  {primitive::capsule, {.089f, .110f, .089f}, leg},
  {primitive::capsule, {.075f, .108f, .075f}, leg},
}};

JPH::RefConst<JPH::Shape> make_shape(const geometry& shape) {
  switch (shape.kind) {
    case primitive::box: return new JPH::BoxShape(shape.size, .025f);
    case primitive::sphere: return new JPH::SphereShape(shape.size.GetX());
    case primitive::capsule: return new JPH::CapsuleShape(shape.size.GetY(), shape.size.GetX());
  }
  utils::error{}("PHY03 recipe: unknown primitive {}", int(shape.kind));
}

}

const std::array<geometry, part_count>& ragdoll_geometry() { return shapes; }

std::array<transform, part_count> animated_pose(const JPH::RVec3Arg feet, const float yaw,
                                                const float stride, const float lean) {
  std::array<transform, part_count> pose;
  pose[0] = {feet + Vec3(0, .875f + .015f * std::abs(stride), 0),
             JPH::Quat::sRotation(Vec3::sAxisY(), yaw)};
  const std::array<float, part_count> bend{0, lean, -lean * .4f, -stride * .24f, 0,
    stride * .24f, 0, stride * .26f, 0, -stride * .26f, 0};
  for (size_t i = 1; i < pose.size(); ++i) {
    const auto& joint = joints[i];
    const auto& parent = pose[size_t(joint.parent)];
    auto& child = pose[i];
    child.rotation = parent.rotation * JPH::Quat::sRotation(Vec3::sAxisZ(), bend[i]);
    child.position = parent.position + parent.rotation * joint.anchor_parent - child.rotation * joint.anchor_child;
  }
  return pose;
}

JPH::Ref<JPH::RagdollSettings> make_recipe(const JPH::ObjectLayer layer) {
  JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;
  settings->mSkeleton = new JPH::Skeleton;
  const auto rest = animated_pose(JPH::RVec3::sZero(), 0, 0, 0);
  std::array<JPH::Mat44, part_count> rest_matrices;
  for (size_t i = 0; i < joints.size(); ++i) {
    const auto& joint = joints[i];
    settings->mSkeleton->AddJoint(joint.name, joint.parent);
    auto& body = settings->mParts.emplace_back();
    body.SetShape(make_shape(shapes[i]));
    body.mPosition = rest[i].position;
    body.mRotation = rest[i].rotation;
    body.mObjectLayer = layer;
    body.mMotionType = JPH::EMotionType::Dynamic;
    body.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    body.mMassPropertiesOverride.mMass = joint.mass;
    body.mFriction = .65f;
    body.mLinearDamping = .17f;
    body.mAngularDamping = .27f;
    rest_matrices[i] = JPH::Mat44::sRotationTranslation(rest[i].rotation, Vec3(rest[i].position));
    if (joint.parent < 0) continue;

    const auto& parent = rest[size_t(joint.parent)];
    const auto anchor = parent.position + parent.rotation * joint.anchor_parent;
    const auto child_anchor = rest[i].position + rest[i].rotation * joint.anchor_child;
    const auto separation = (anchor - child_anchor).Length();
    if (separation > .0001f)
      utils::error{}("PHY03 recipe joint '{}': bind anchors separated by {} m, limit 0.0001 m",
                     joint.name, separation);

    if (joint.hinge_sign != 0) {
      auto* hinge = new JPH::HingeConstraintSettings;
      hinge->mPoint1 = hinge->mPoint2 = anchor;
      hinge->mHingeAxis1 = hinge->mHingeAxis2 = joint.hinge_sign * Vec3::sAxisZ();
      hinge->mNormalAxis1 = hinge->mNormalAxis2 = Vec3::sAxisY();
      hinge->mLimitsMin = 0;
      hinge->mLimitsMax = JPH::DegreesToRadians(joint.swing_or_hinge_degrees);
      hinge->mMaxFrictionTorque = .8f;
      body.mToParent = hinge;
    } else {
      auto* swing = new JPH::SwingTwistConstraintSettings;
      swing->mPosition1 = swing->mPosition2 = anchor;
      swing->mTwistAxis1 = swing->mTwistAxis2 = i <= size_t(part::head) ? Vec3::sAxisY() : -Vec3::sAxisY();
      swing->mPlaneAxis1 = swing->mPlaneAxis2 = Vec3::sAxisX();
      swing->mNormalHalfConeAngle = swing->mPlaneHalfConeAngle = JPH::DegreesToRadians(joint.swing_or_hinge_degrees);
      swing->mTwistMaxAngle = JPH::DegreesToRadians(joint.twist_degrees);
      swing->mTwistMinAngle = -swing->mTwistMaxAngle;
      swing->mMaxFrictionTorque = 1.4f;
      body.mToParent = swing;
    }
  }

  // Initial intersections at the shoulder/hip seams should not eject limbs on the handoff.
  settings->DisableParentChildCollisions(rest_matrices.data());
  if (!settings->Stabilize())
    utils::error{}("PHY03 ragdoll recipe: Jolt could not stabilize {} linked bodies", settings->mParts.size());
  settings->CalculateBodyIndexToConstraintIndex();
  settings->CalculateConstraintIndexToBodyIdxPair();
  return settings;
}

}
