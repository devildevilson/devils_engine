#include "simulation.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/RegisterTypes.h>

#include "devils_engine/aesthetics/world.h"
#include "devils_engine/utils/error.h"

#include "recipe.h"

namespace devils_engine::phy03 {
namespace {

using JPH::BodyID;
using JPH::Quat;
using JPH::RVec3;
using JPH::RVec3Arg;
using JPH::Vec3;
using JPH::Vec3Arg;

constexpr JPH::ObjectLayer fixed_layer = 0;
constexpr JPH::ObjectLayer moving_layer = 1;
constexpr JPH::ObjectLayer ragdoll_layer = 2;
constexpr JPH::ObjectLayer capsule_layer = 3;

constexpr float standing_radius = .30f;
constexpr float standing_half_cylinder = .59f;
constexpr float standing_center = standing_radius + standing_half_cylinder;
constexpr float standing_padding = .02f;
constexpr float inner_radius = .26f;
constexpr float inner_half_cylinder = .55f;
constexpr float character_core_distance = .28f;
constexpr float separation_speed = .80f;
constexpr float rest_speed = .10f;
constexpr float step_height = .28f;
constexpr float max_step_height = .36f;
constexpr float step_clearance = .012f;
constexpr uint32_t step_lift_ticks = 8;
constexpr float support_normal_y = .70f;
constexpr float pi = std::numbers::pi_v<float>;
constexpr uint32_t impact_delay_ticks = 24;
constexpr uint32_t settle_ticks = 8;
constexpr uint32_t recovery_ticks = 36;
constexpr float local_recovery_radius = .28f;
constexpr float recovery_drop = .70f;
constexpr uint32_t run_ticks = 54;

const RVec3 player_start(-7, .02f, 0);

const std::array<solid, fixture_count> room{{
  {{primitive::box, {12, .25f, 7}, 0x576671}, {{0, -.25f, 0}}},
  {{primitive::box, {.8f, step_height * .5f, 1.10f}, 0xc09a60}, {{-5, step_height * .5f, 0}}},
  {{primitive::box, {1.75f, .125f, 1.30f}, 0x9a624b}, {{2.9f, .965f, -2.7f}}},
  {{primitive::box, {1.30f, .10f, 1.42f}, 0x4c9d92}, {{-2.5f, .10f, 0}}},
  {{primitive::box, {.24f, .24f, .24f}, 0xd7a75c}, {{-2.07f, .45f, .91f}}},
  {{primitive::box, {.15f, 2.2f, 7}, 0x374854}, {{-12, 2.2f, 0}}},
  {{primitive::box, {.15f, 2.2f, 7}, 0x374854}, {{12, 2.2f, 0}}},
  {{primitive::box, {12, 2.2f, .15f}, 0x445563}, {{0, 2.2f, -7}}},
  {{primitive::box, {12, 2.2f, .15f}, 0x445563}, {{0, 2.2f, 7}}},
}};

struct recipe_ref {
  const JPH::RagdollSettings* value;
};

bool grounded(const JPH::CharacterVirtual& character) {
  return character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

class room_filter final : public JPH::ObjectLayerFilter {
public:
  bool ShouldCollide(const JPH::ObjectLayer layer) const override {
    return layer == fixed_layer || layer == moving_layer;
  }
};

class actor_filter final : public JPH::ObjectLayerFilter {
public:
  bool ShouldCollide(const JPH::ObjectLayer layer) const override {
    return layer == ragdoll_layer || layer == capsule_layer;
  }
};

class ignore_actor final : public JPH::BodyFilter {
public:
  explicit ignore_actor(const aesthetics::entityid_t actor) : actor(actor) {}

  bool ShouldCollideLocked(const JPH::Body& body) const override {
    const auto layer = body.GetObjectLayer();
    return (layer != capsule_layer && layer != ragdoll_layer) || body.GetUserData() != actor;
  }

private:
  aesthetics::entityid_t actor;
};

struct support_point {
  BodyID body;
  RVec3 position;
};

class ragdoll_support final : public JPH::CollideShapeCollector {
public:
  std::optional<support_point> contact;
  RVec3 base;

  void AddHit(const JPH::CollideShapeResult& hit) override {
    const auto normal = -hit.mPenetrationAxis.NormalizedOr(Vec3::sZero());
    if (normal.GetY() < support_normal_y) return;
    if (contact && (hit.mPenetrationDepth < depth ||
        (hit.mPenetrationDepth == depth && !(hit.mBodyID2 < contact->body)))) return;
    contact = {hit.mBodyID2, base + hit.mContactPointOn2};
    depth = hit.mPenetrationDepth;
  }

private:
  float depth = 0;
};

class blocking_cast final : public JPH::CastShapeCollector {
public:
  explicit blocking_cast(const Vec3Arg direction) : direction(direction) {}

  void AddHit(const JPH::ShapeCastResult& hit) override {
    const auto axis = hit.mPenetrationAxis.NormalizedOr(Vec3::sZero());
    if (axis.Dot(direction) <= .001f) return;
    blocked = true;
    ForceEarlyOut();
  }

  bool blocked = false;

private:
  Vec3 direction;
};

struct step_path {
  RVec3 start;
  RVec3 landing;
  BodyID support;
  uint32_t cross_ticks;
  uint32_t age = 0;
};

struct body_motion {
  Vec3 linear;
  Vec3 angular;
};

constexpr std::array core_parts{part::pelvis, part::chest};

Quat view_rotation(const float yaw, const float pitch) {
  return Quat::sRotation(Vec3::sAxisY(), yaw) * Quat::sRotation(Vec3::sAxisZ(), pitch);
}

float angle_lerp(const float a, const float b, const float alpha) {
  return a + std::remainder(b - a, 2 * pi) * alpha;
}

float smoothstep(const float value) {
  return value * value * (3 - 2 * value);
}

Vec3 approach(const Vec3 from, const Vec3 to, const float amount) {
  const auto delta = to - from;
  const float length = delta.Length();
  return length <= amount ? to : from + delta * (amount / length);
}

transform body_pose(const JPH::BodyInterface& bodies, const BodyID id) {
  transform result;
  bodies.GetPositionAndRotation(id, result.position, result.rotation);
  return result;
}

JPH::RefConst<JPH::Shape> capsule_shape(const float radius, const float half_cylinder, const float center) {
  return new JPH::RotatedTranslatedShape(Vec3(0, center, 0), Quat::sIdentity(),
                                       new JPH::CapsuleShape(half_cylinder, radius));
}

}

const geometry& query_capsule_geometry() {
  constexpr float radius = standing_radius + standing_padding;
  static const geometry shape{primitive::capsule, {radius, standing_half_cylinder, radius}, 0x62f1ff};
  return shape;
}

const geometry& inner_capsule_geometry() {
  static const geometry shape{primitive::capsule, {inner_radius, inner_half_cylinder, inner_radius}, 0xffc254};
  return shape;
}

struct simulation::world {
  struct character_actor {
    world& scene;
    JPH::Ref<JPH::Ragdoll> ragdoll;
    JPH::Ref<JPH::CharacterVirtual> character;
    std::array<BodyID, part_count> actor_bodies;
    aesthetics::entityid_t actor = aesthetics::invalid_entityid;
    actor_snapshot after;
    std::array<transform, part_count> previous_pose;
    std::array<transform, part_count> recovery_start;
    std::array<body_motion, core_parts.size()> previous_core_motion;
    std::optional<step_path> step;
    Vec3 locomotion = Vec3::sZero();
    Vec3 external_velocity = Vec3::sZero();
    Vec3 controller_velocity = Vec3::sZero();
    float body_yaw = 0;
    float stride_phase = 0;
    float fall_yaw = 0;
    uint32_t quiet_ticks = 0;
    uint32_t recovery_age = 0;
    uint32_t recovery_duration = recovery_ticks;

    explicit character_actor(world& scene) : scene(scene) {}
    ~character_actor();

    JPH::BodyInterface& bodies() const { return scene.physics.GetBodyInterface(); }
    movement_mode& mode() { return *scene.ecs.get<movement_mode>(actor); }
    bool on_platform() const;
    void spawn(JPH::RVec3Arg feet, uint32_t collision_group);
    void despawn();
    void create_controller(JPH::RVec3Arg feet);
    void advance(const intent& input);
    void push(uint32_t tenths);
    Vec3 inherited_velocity() const;
    void move_character(const intent& input);
    bool capsule_fits(JPH::RVec3Arg feet, float margin, const JPH::ObjectLayerFilter& layers = {}) const;
    bool capsule_path_clear(JPH::RVec3Arg start, JPH::RVec3Arg end, const JPH::ObjectLayerFilter& layers) const;
    bool at_rest() const;
    float shift_capsule(JPH::Vec3Arg direction, float distance, bool stop_inward_motion);
    void plan_step(JPH::RVec3Arg feet, JPH::Vec3Arg direction);
    void advance_step();
    body_motion motion(part role) const;
    std::optional<support_point> find_ragdoll_support(const JPH::ObjectLayerFilter& layers) const;
    bool track_recovery_motion(const support_point* support);
    std::optional<support_point> find_recovery_feet(const support_point& support, bool on_actor) const;
    std::optional<support_point> find_rescue_feet(JPH::RVec3Arg origin) const;
    void fall(JPH::Vec3Arg inherited);
    void recover();
    bool rescue(JPH::RVec3Arg fallback);
    void resume_controller(const support_point& feet);
    void publish();
    void check_collision_owner();
  };

  JPH::BroadPhaseLayerInterfaceTable broad{4, 2};
  JPH::ObjectLayerPairFilterTable pairs{4};
  std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broad_filter;
  JPH::TempAllocatorImpl scratch{2 * 1024 * 1024};
  JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
  JPH::PhysicsSystem physics;
  JPH::RefConst<JPH::Shape> standing = capsule_shape(standing_radius, standing_half_cylinder, standing_center);
  JPH::RefConst<JPH::Shape> inner = capsule_shape(inner_radius, inner_half_cylinder, standing_center);
  JPH::RefConst<JPH::Shape> camera_sphere = new JPH::SphereShape(.085f);
  JPH::Ref<JPH::RagdollSettings> recipe = make_recipe(ragdoll_layer);
  std::array<BodyID, fixture_count> fixtures;
  aesthetics::world ecs;
  character_actor player{*this};
  character_actor dummy{*this};

  snapshot before;
  snapshot after;
  uint32_t platform_age = 0;
  float platform_speed = 0;
  bool platform_used = false;

  world();
  ~world();

  JPH::BodyInterface& bodies() { return physics.GetBodyInterface(); }
  const JPH::BodyInterface& bodies() const { return physics.GetBodyInterface(); }
  BodyID body(const fixture id) const { return fixtures[size_t(id)]; }
  void move_platform(brake_test brake);
  void push_room(uint32_t tenths);
  void separate_controllers();
  std::optional<support_point> floor_below(JPH::RVec3Arg start, float distance) const;
  std::optional<support_point> foot_patch(JPH::RVec3Arg start, float distance) const;
  void publish();
  float camera_fraction(JPH::RVec3Arg start, JPH::RVec3Arg end) const;
};

simulation::world::world() {
  broad.MapObjectToBroadPhaseLayer(fixed_layer, JPH::BroadPhaseLayer(0));
  for (const auto layer : {moving_layer, ragdoll_layer, capsule_layer})
    broad.MapObjectToBroadPhaseLayer(layer, JPH::BroadPhaseLayer(1));
  pairs.EnableCollision(fixed_layer, moving_layer);
  pairs.EnableCollision(fixed_layer, ragdoll_layer);
  pairs.EnableCollision(fixed_layer, capsule_layer);
  pairs.EnableCollision(moving_layer, moving_layer);
  pairs.EnableCollision(moving_layer, ragdoll_layer);
  pairs.EnableCollision(moving_layer, capsule_layer);
  pairs.EnableCollision(ragdoll_layer, ragdoll_layer);
  pairs.EnableCollision(capsule_layer, ragdoll_layer);
  // The scene handles upright capsule pairs after their ordinary movement.

  broad_filter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broad, 2, pairs, 4);
  physics.Init(64, 0, 512, 256, broad, *broad_filter, pairs);
  auto settings = physics.GetPhysicsSettings();
  settings.mNumVelocitySteps = 16;
  settings.mNumPositionSteps = 8;
  physics.SetPhysicsSettings(settings);

  for (size_t i = 0; i < fixtures.size(); ++i) {
    const auto& item = room[i];
    const auto id = fixture(i);
    auto motion = JPH::EMotionType::Static;
    if (id == fixture::platform) motion = JPH::EMotionType::Kinematic;
    if (id == fixture::crate) motion = JPH::EMotionType::Dynamic;
    const auto layer = motion == JPH::EMotionType::Static ? fixed_layer : moving_layer;
    JPH::BodyCreationSettings body_settings(new JPH::BoxShape(item.shape.size, .025f),
      item.pose.position, item.pose.rotation, motion, layer);
    body_settings.mFriction = .90f;
    body_settings.mAllowSleeping = id != fixture::crate;
    fixtures[i] = bodies().CreateAndAddBody(body_settings,
      id == fixture::crate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (fixtures[i].IsInvalid()) utils::error{}("PHY03 room body {}: exhausted capacity of 64 bodies", i);
  }

  ecs.reserve<intent>(2);
  ecs.reserve<movement_mode>(2);
  ecs.reserve<recipe_ref>(2);
  // Keep the dummy visible from spawn and clear of the step and deck approaches.
  player.spawn(player_start, 1);
  dummy.spawn(RVec3(-3.8f, .02f, 2.1f), 2);
  physics.OptimizeBroadPhase();
  publish();
  before = after;
}

simulation::world::~world() {
  player.despawn();
  dummy.despawn();
  for (const auto id : fixtures) {
    bodies().RemoveBody(id);
    bodies().DestroyBody(id);
  }
}

void simulation::world::character_actor::spawn(const RVec3Arg feet, const uint32_t collision_group) {
  auto& ecs = scene.ecs;
  actor = ecs.gen_entityid();
  ecs.create<intent>(actor);
  ecs.create<movement_mode>(actor, movement_mode::walking);
  ecs.create<recipe_ref>(actor, recipe_ref{scene.recipe.GetPtr()});
  ragdoll = ecs.get<recipe_ref>(actor)->value->CreateRagdoll(collision_group, actor, &scene.physics);
  if (ragdoll == nullptr) utils::error{}("PHY03 actor {}: exhausted capacity allocating {} ragdoll bodies", actor, part_count);
  if (ragdoll->GetBodyCount() != part_count || ragdoll->GetConstraintCount() != part_count - 1)
    utils::error{}("PHY03 actor {}: ragdoll has {} bodies and {} joints, expected {} and {}",
                   actor, ragdoll->GetBodyCount(), ragdoll->GetConstraintCount(), part_count, part_count - 1);
  for (size_t role = 0; role < actor_bodies.size(); ++role)
    actor_bodies[role] = ragdoll->GetBodyID(int(role));

  create_controller(feet);
  after.parts = animated_pose(feet, 0, 0, 0);
}

simulation::world::character_actor::~character_actor() {
  despawn();
}

void simulation::world::character_actor::despawn() {
  if (ragdoll != nullptr && bodies().IsAdded(actor_bodies[0])) ragdoll->RemoveFromPhysicsSystem();
  character = nullptr;
  ragdoll = nullptr;
}

bool simulation::world::character_actor::on_platform() const {
  return character != nullptr && grounded(*character) && character->GetGroundBodyID() == scene.body(fixture::platform);
}

void simulation::world::character_actor::create_controller(const JPH::RVec3Arg feet) {
  JPH::CharacterVirtualSettings settings;
  settings.mShape = scene.standing;
  settings.mInnerBodyShape = scene.inner;
  settings.mInnerBodyLayer = capsule_layer;
  settings.mCharacterPadding = standing_padding;
  settings.mSupportingVolume = JPH::Plane(Vec3::sAxisY(), -standing_radius);
  settings.mMass = 70;
  settings.mMaxStrength = 100;
  character = new JPH::CharacterVirtual(&settings, feet, Quat::sIdentity(), actor, &scene.physics);
  if (character->GetInnerBodyID().IsInvalid())
    utils::error{}("PHY03 actor {}: exhausted capacity allocating capsule body", actor);
  character->RefreshContacts(scene.physics.GetDefaultBroadPhaseLayerFilter(capsule_layer),
    scene.physics.GetDefaultLayerFilter(capsule_layer), {}, {}, scene.scratch);
}

void simulation::world::move_platform(const brake_test brake) {
  const auto platform = body(fixture::platform);
  if (!platform_used && player.on_platform() && brake != brake_test::none) {
    platform_used = true;
    platform_speed = push_speed(brake == brake_test::weak ? weak_push : strong_push);
  }

  if (platform_speed != 0 && platform_age++ == run_ticks) {
    const auto speed_before = uint32_t(std::lround(platform_speed * 10));
    platform_speed = 0;
    after.brake_tick = after.tick;
    // Each rider keeps its pre-brake momentum once across the support velocity change.
    for (auto* actor : {&player, &dummy}) {
      if (actor->on_platform()) actor->push(speed_before);
    }
  }

  const auto position = bodies().GetPosition(platform);
  bodies().MoveKinematic(platform, position + Vec3(platform_speed * tick_seconds, 0, 0),
                         Quat::sIdentity(), tick_seconds);
}

void simulation::world::push_room(const uint32_t tenths) {
  if (tenths == 0) return;
  const auto power = std::min(tenths, max_push);
  for (auto* actor : {&player, &dummy}) actor->push(power);
  bodies().AddLinearVelocity(body(fixture::crate), Vec3(push_speed(power), 0, 0));
}

Vec3 simulation::world::character_actor::inherited_velocity() const {
  if (on_platform()) return locomotion + external_velocity + Vec3(scene.platform_speed, 0, 0);
  auto velocity = controller_velocity;
  if (grounded(*character)) velocity.SetY(0);
  return velocity;
}

void simulation::world::character_actor::push(const uint32_t tenths) {
  const auto delta = Vec3(push_speed(tenths), 0, 0);
  if (character == nullptr) {
    ragdoll->AddLinearVelocity(delta);
    if (tenths >= fall_threshold) {
      after.last_impact_tick = scene.after.tick;
      quiet_ticks = 0;
    }
    // The next sample should measure the solver response after this known impulse.
    for (auto& sample : previous_core_motion) sample.linear += delta;
    return;
  }
  if (tenths >= fall_threshold) {
    fall(inherited_velocity() + delta);
    return;
  }
  step.reset();
  external_velocity += delta;
  if (tenths >= stagger_threshold && mode() == movement_mode::walking)
    mode() = movement_mode::staggering;
}

void simulation::world::character_actor::advance(const intent& input) {
  *scene.ecs.get<intent>(actor) = input;
  const auto& controls = *scene.ecs.get<intent>(actor);
  if (controls.drop && mode() == movement_mode::walking) fall(inherited_velocity());
  after.yaw = std::remainder(after.yaw + controls.yaw_delta, 2 * pi);
  after.pitch = std::clamp(after.pitch + controls.pitch_delta, -1.42f, 1.42f);
  if (mode() == movement_mode::falling) {
    const float offset = std::clamp(std::remainder(after.yaw - fall_yaw, 2 * pi), -1.28f, 1.28f);
    after.yaw = std::remainder(fall_yaw + offset, 2 * pi);
    after.pitch = std::clamp(after.pitch, -1.0f, 1.0f);
  }
  if (character != nullptr) move_character(controls);
}

void simulation::world::character_actor::move_character(const intent& input) {
  const auto facing = Quat::sRotation(Vec3::sAxisY(), after.yaw);
  const auto desired = facing * Vec3(input.forward, 0, input.right);
  auto speed = 3.0f;
  if (mode() == movement_mode::staggering) speed *= .40f;
  if (mode() == movement_mode::recovering) speed *= float(recovery_age) / recovery_duration;
  locomotion = approach(locomotion, desired / std::max(1.0f, desired.Length()) * speed, 13 * tick_seconds);

  character->UpdateGroundVelocity();
  auto support = grounded(*character) ? character->GetGroundVelocity() : Vec3(0, controller_velocity.GetY(), 0);
  if (on_platform())
    support = Vec3(scene.platform_speed, 0, 0);
  controller_velocity = locomotion + external_velocity + support + scene.physics.GetGravity() * tick_seconds;
  character->SetLinearVelocity(controller_velocity);

  if (step) {
    advance_step();
  } else {
    const auto start = character->GetPosition();
    const bool supported = grounded(*character);
    JPH::CharacterVirtual::ExtendedUpdateSettings update;
    update.mWalkStairsStepUp = Vec3::sZero();
    update.mStickToFloorStepDown = Vec3(0, -.37f, 0);
    character->ExtendedUpdate(tick_seconds, scene.physics.GetGravity(), update,
      scene.physics.GetDefaultBroadPhaseLayerFilter(capsule_layer), scene.physics.GetDefaultLayerFilter(capsule_layer),
      {}, {}, scene.scratch);
    const auto heading = locomotion.NormalizedOr(Vec3::sZero());
    const auto travel = Vec3(character->GetPosition() - start) - support * tick_seconds;
    if (supported && !heading.IsNearZero() && travel.Dot(heading) < .75f * locomotion.Length() * tick_seconds)
      plan_step(character->GetPosition(), heading);
  }

  external_velocity = approach(external_velocity, Vec3::sZero(), 3.4f * tick_seconds);
  if (mode() == movement_mode::staggering && external_velocity.IsNearZero()) mode() = movement_mode::walking;
  if (mode() == movement_mode::recovering && ++recovery_age >= recovery_duration) mode() = movement_mode::walking;

  body_yaw = angle_lerp(body_yaw, after.yaw, std::min(1.0f, 10 * tick_seconds));
  stride_phase = std::remainder(stride_phase + locomotion.Length() * 4.8f * tick_seconds, 2 * pi);
}

std::optional<support_point> simulation::world::floor_below(const RVec3Arg start, const float distance) const {
  const JPH::RRayCast ray(start, Vec3(0, -distance, 0));
  JPH::RayCastResult hit;
  if (!physics.GetNarrowPhaseQuery().CastRay(ray, hit, {}, room_filter{}) || hit.mFraction == 0)
    return std::nullopt;
  const auto contact = ray.GetPointOnRay(hit.mFraction);
  const auto normal = bodies().GetTransformedShape(hit.mBodyID).GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, contact);
  if (normal.GetY() < support_normal_y) return std::nullopt;
  return support_point{hit.mBodyID, contact};
}

std::optional<support_point> simulation::world::foot_patch(const RVec3Arg start, const float distance) const {
  const auto center = floor_below(start, distance);
  if (!center) return std::nullopt;
  // Sample an 18 cm footprint with the same orientation for every approach.
  for (const auto offset : {Vec3(.09f, 0, 0), Vec3(-.09f, 0, 0), Vec3(0, 0, .09f), Vec3(0, 0, -.09f)}) {
    const auto edge = floor_below(start + offset, distance);
    if (!edge || edge->body != center->body || std::abs(edge->position.GetY() - center->position.GetY()) > .035f)
      return std::nullopt;
  }
  return center;
}

bool simulation::world::character_actor::capsule_fits(const RVec3Arg feet, const float margin,
                                                      const JPH::ObjectLayerFilter& layers) const {
  JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> overlaps;
  JPH::CollideShapeSettings clearance;
  clearance.mMaxSeparationDistance = margin;
  scene.physics.GetNarrowPhaseQuery().CollideShape(scene.standing, Vec3::sOne(),
    JPH::RMat44::sTranslation(feet + scene.standing->GetCenterOfMass()), clearance, feet, overlaps,
    {}, layers, ignore_actor(actor));
  return !overlaps.HadHit();
}

bool simulation::world::character_actor::capsule_path_clear(const RVec3Arg start, const RVec3Arg end,
                                                            const JPH::ObjectLayerFilter& layers) const {
  const auto delta = Vec3(end - start);
  if (delta.LengthSq() < 1e-10f) return true;
  const auto path = JPH::RShapeCast::sFromWorldTransform(scene.standing, Vec3::sOne(),
    JPH::RMat44::sTranslation(start), delta);
  blocking_cast collector(delta.Normalized());
  scene.physics.GetNarrowPhaseQuery().CastShape(path, {}, start, collector, {}, layers, ignore_actor(actor));
  return !collector.blocked;
}

bool simulation::world::character_actor::at_rest() const {
  return grounded(*character) && !step && (locomotion + external_velocity).LengthSq() <= rest_speed * rest_speed;
}

float simulation::world::character_actor::shift_capsule(const Vec3Arg direction, const float distance,
                                                        const bool stop_inward_motion) {
  if (distance < .0001f) return 0;
  const auto start = character->GetPosition();
  const auto layers = scene.physics.GetDefaultLayerFilter(capsule_layer);
  const auto can_shift = [&](const float length) {
    // A grounded capsule already touches its support. Only blocking contacts
    // along the horizontal path should stop it from moving.
    return capsule_path_clear(start, start + direction * length, layers);
  };
  auto accepted = distance;
  if (!can_shift(accepted)) {
    // A nearby wall can allow only part of the correction.
    accepted = 0;
    auto blocked = distance;
    for (int i = 0; i < 5; ++i) {
      const auto length = (accepted + blocked) * .5f;
      if (can_shift(length)) accepted = length;
      else blocked = length;
    }
  }
  if (accepted < .0001f) return 0;
  character->SetPosition(start + direction * accepted);
  character->RefreshContacts(scene.physics.GetDefaultBroadPhaseLayerFilter(capsule_layer),
    scene.physics.GetDefaultLayerFilter(capsule_layer), {}, {}, scene.scratch);
  if (stop_inward_motion) {
    const auto speed = std::max(0.0f, -controller_velocity.Dot(direction));
    controller_velocity += direction * std::min(speed, accepted / tick_seconds);
    if (step) {
      step.reset();
      controller_velocity.SetY(0);
    }
    character->SetLinearVelocity(controller_velocity);
  }
  return accepted;
}

void simulation::world::separate_controllers() {
  if (player.character == nullptr || dummy.character == nullptr) return;

  const auto player_feet = player.character->GetPosition();
  const auto dummy_feet = dummy.character->GetPosition();
  const auto difference = Vec3(dummy_feet - player_feet);
  const bool resting = player.at_rest() && dummy.at_rest();
  const auto diameter = resting ? 2 * standing_radius : character_core_distance;
  // With the same upright shape, only the hemispherical ends matter when
  // the vertical axes no longer overlap.
  const auto vertical_gap = std::max(0.0f, std::abs(difference.GetY()) - 2 * standing_half_cylinder);
  if (vertical_gap >= diameter) return;
  const auto reach = std::sqrt(diameter * diameter - vertical_gap * vertical_gap);
  const Vec3 horizontal(1, 0, 1);
  const auto planar = difference * horizontal;
  const auto previous_planar = Vec3(before.dummy.capsule.position - before.player.capsule.position) * horizontal;
  auto axis = planar.NormalizedOr(previous_planar.NormalizedOr(Vec3::sAxisX()));
  auto depth = reach - planar.Length();
  if (!resting) {
    // Detect entry into the core even if the pair would cross between ticks.
    const auto travel = planar - previous_planar;
    const auto a = travel.LengthSq();
    const auto b = previous_planar.Dot(travel);
    const auto c = previous_planar.LengthSq() - reach * reach;
    const auto discriminant = b * b - a * c;
    if (c >= -.0003f && b < 0 && discriminant > 0) {
      const auto entry = std::max(0.0f, c) / (-b + std::sqrt(discriminant));
      if (entry <= 1) {
        axis = (previous_planar + travel * entry).NormalizedOr(axis);
        depth = reach - planar.Dot(axis);
      }
    }
  }
  if (depth <= .0005f) return;

  auto correction = depth;
  float player_share = .5f;
  if (resting) {
    correction = std::min(depth, std::min(separation_speed, .06f + 8 * depth) * tick_seconds);
  } else {
    // Give back the inward displacement of the moving actor first.
    const auto player_move = Vec3(player_feet - (before.player.capsule.position - Vec3(0, standing_center, 0)));
    const auto dummy_move = Vec3(dummy_feet - (before.dummy.capsule.position - Vec3(0, standing_center, 0)));
    const auto player_inward = std::max(0.0f, player_move.Dot(axis));
    const auto dummy_inward = std::max(0.0f, -dummy_move.Dot(axis));
    const auto inward = player_inward + dummy_inward;
    if (inward > .0001f) player_share = player_inward / inward;
  }

  const auto player_moved = player.shift_capsule(-axis, correction * player_share, !resting);
  const auto dummy_request = correction - player_moved;
  const auto dummy_moved = dummy.shift_capsule(axis, dummy_request, !resting);
  if (dummy_moved < dummy_request)
    player.shift_capsule(-axis, dummy_request - dummy_moved, !resting);
}

void simulation::world::character_actor::plan_step(const RVec3Arg feet, const Vec3Arg direction) {
  const auto source = scene.floor_below(feet + Vec3(0, .10f, 0), .28f);
  if (!source) return;
  const auto layers = scene.physics.GetDefaultLayerFilter(capsule_layer);
  for (const float distance : {.40f, .50f, .60f, .70f}) {
    const auto probe = source->position + direction * distance + Vec3(0, .55f, 0);
    const auto target = scene.foot_patch(probe, .55f);
    if (!target) continue;
    const auto rise = target->position.GetY() - source->position.GetY();
    if (rise < .04f || rise > max_step_height) continue;
    const auto landing = target->position + Vec3(0, step_clearance, 0);
    const auto raised = RVec3(feet.GetX(), landing.GetY(), feet.GetZ());
    if (!capsule_fits(landing, .002f, layers) ||
        !capsule_path_clear(feet, raised, layers) || !capsule_path_clear(raised, landing, layers))
      continue;
    const float crossing_speed = std::max(.80f, locomotion.Length());
    step = step_path{feet, landing, target->body, uint32_t(std::ceil(distance / (crossing_speed * tick_seconds)))};
    return;
  }
}

void simulation::world::character_actor::advance_step() {
  auto& path = *step;
  const auto floor = scene.foot_patch(path.landing + Vec3(0, .13f, 0), .26f);
  if (!floor || floor->body != path.support ||
      std::abs(floor->position.GetY() + step_clearance - path.landing.GetY()) > .008f) {
    controller_velocity.SetY(0);
    step.reset();
    return;
  }
  ++path.age;
  const auto raised = RVec3(path.start.GetX(), path.landing.GetY(), path.start.GetZ());
  const auto lift = smoothstep(float(std::min(path.age, step_lift_ticks)) / step_lift_ticks);
  const auto t = float(path.age - std::min(path.age, step_lift_ticks)) / path.cross_ticks;
  const auto cross = t * t * (2 - t); // Leave the step at walking speed.
  const auto next = path.start + (raised - path.start) * lift + (path.landing - raised) * cross;
  const auto current = character->GetPosition();
  const auto object_layers = scene.physics.GetDefaultLayerFilter(capsule_layer);
  if (!capsule_path_clear(current, next, object_layers) || !capsule_fits(next, 0, object_layers)) {
    controller_velocity.SetY(0);
    step.reset();
    return;
  }
  controller_velocity = Vec3(next - current) / tick_seconds;
  character->SetLinearVelocity(controller_velocity);
  character->SetPosition(next);
  const auto broad_layers = scene.physics.GetDefaultBroadPhaseLayerFilter(capsule_layer);
  if (path.age == step_lift_ticks + path.cross_ticks) {
    if (character->StickToFloor(Vec3(0, -.04f, 0), broad_layers, object_layers, {}, {}, scene.scratch) &&
        grounded(*character) && character->GetGroundBodyID() == path.support)
      ++after.completed_steps;
    step.reset();
  } else {
    character->RefreshContacts(broad_layers, object_layers, {}, {}, scene.scratch);
  }
}

void simulation::world::character_actor::fall(const JPH::Vec3Arg inherited) {
  step.reset();
  fall_yaw = after.yaw;
  quiet_ticks = 0;
  after.last_impact_tick = scene.after.tick;
  after.recovery_blocked = false;
  ++after.handoffs;
  after.inherited_velocity = inherited;

  // Derive limb motion from the last two poses in the controller's moving frame.
  // The world-space root velocity replaces that frame's translation exactly once.
  const auto root = after.parts[0].position;
  const auto old_root = previous_pose[0].position;
  std::array<JPH::Mat44, part_count> matrices;
  for (size_t i = 0; i < actor_bodies.size(); ++i)
    matrices[i] = JPH::Mat44::sRotationTranslation(after.parts[i].rotation, Vec3(after.parts[i].position - root));
  character = nullptr;
  ragdoll->SetPose(root, matrices.data());
  ragdoll->ResetWarmStart();
  for (size_t i = 0; i < actor_bodies.size(); ++i) {
    auto delta = after.parts[i].rotation * previous_pose[i].rotation.Conjugated();
    if (delta.GetW() < 0) delta = -delta;
    Vec3 axis;
    float angle;
    delta.GetAxisAngle(axis, angle);
    const auto relative = (after.parts[i].position - root) - (previous_pose[i].position - old_root);
    bodies().SetLinearAndAngularVelocity(actor_bodies[i], inherited + Vec3(relative) / tick_seconds,
                                        axis * (angle / tick_seconds));
  }
  after.pelvis_launch_velocity = bodies().GetLinearVelocity(actor_bodies[0]);
  ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
  for (size_t i = 0; i < core_parts.size(); ++i) previous_core_motion[i] = motion(core_parts[i]);
  mode() = movement_mode::falling;
}

body_motion simulation::world::character_actor::motion(const part role) const {
  body_motion result;
  bodies().GetLinearAndAngularVelocity(actor_bodies[size_t(role)], result.linear, result.angular);
  return result;
}

std::optional<support_point> simulation::world::character_actor::find_ragdoll_support(
    const JPH::ObjectLayerFilter& layers) const {
  const auto& query = scene.physics.GetNarrowPhaseQuery();
  JPH::CollideShapeSettings settings;
  settings.mMaxSeparationDistance = .035f;
  ragdoll_support support;
  const ignore_actor exclude_self(actor);
  for (const auto id : actor_bodies) {
    const auto shape = bodies().GetShape(id);
    const auto pose = bodies().GetCenterOfMassTransform(id);
    support.base = pose.GetTranslation();
    query.CollideShape(shape, Vec3::sOne(), pose, settings, support.base, support, {}, layers, exclude_self);
  }
  return support.contact;
}

bool simulation::world::character_actor::track_recovery_motion(const support_point* support) {
  bool supported_and_quiet = support != nullptr;
  const auto support_angular = support == nullptr ? Vec3::sZero() : bodies().GetAngularVelocity(support->body);
  for (size_t i = 0; i < core_parts.size(); ++i) {
    const auto current = motion(core_parts[i]);
    const auto previous = previous_core_motion[i];
    previous_core_motion[i] = current;
    if ((current.linear - previous.linear).LengthSq() > .85f * .85f ||
        (current.angular - previous.angular).LengthSq() > 3.2f * 3.2f)
      after.last_impact_tick = scene.after.tick;

    if (support == nullptr) continue;
    const auto center = bodies().GetCenterOfMassPosition(actor_bodies[size_t(core_parts[i])]);
    const auto support_linear = bodies().GetPointVelocity(support->body, center);
    supported_and_quiet &= (current.linear - support_linear).LengthSq() <= .50f * .50f &&
                           (current.angular - support_angular).LengthSq() <= 1.35f * 1.35f;
  }
  quiet_ticks = supported_and_quiet ? std::min(quiet_ticks + 1, settle_ticks) : 0;
  return scene.after.tick - after.last_impact_tick >= impact_delay_ticks && quiet_ticks == settle_ticks;
}

std::optional<support_point> simulation::world::character_actor::find_recovery_feet(
    const support_point& support, const bool on_actor) const {
  const auto pelvis = bodies().GetPosition(actor_bodies[size_t(part::pelvis)]);
  constexpr float probe_height = .12f;
  constexpr float clearance = .015f;
  constexpr float feet_offset = .025f;
  constexpr float support_level_tolerance = .10f;
  const auto above = RVec3(pelvis.GetX(), support.position.GetY() + probe_height, pelvis.GetZ());

  const auto search = [&](const bool below_fixture) -> std::optional<support_point> {
    const bool allow_drop = on_actor || below_fixture;
    const auto probe_distance = allow_drop ? probe_height + recovery_drop : .26f;
    // Widen the search only when an actor provides the support or blocks a local spot.
    bool widen = on_actor;
    for (const float radius : {0.0f, .14f, local_recovery_radius, .42f, .56f, .70f, .84f, .98f, 1.12f}) {
      const bool extended = radius > local_recovery_radius;
      if (extended && !widen) break;
      const size_t count = radius == 0 ? 1 : 8;
      for (size_t i = 0; i < count; ++i) {
        const float angle = float(i) * pi / 4;
        const auto sample = above + radius * Vec3(std::cos(angle), 0, std::sin(angle));
        const auto floor = scene.foot_patch(sample, probe_distance);
        if (!floor) continue;

        const auto drop = support.position.GetY() - floor->position.GetY();
        if (below_fixture && drop <= support_level_tolerance) continue;
        // Keep the usual deck/step seam transition close in height and point velocity.
        if (!allow_drop && floor->body != support.body) {
          if (std::abs(drop) > support_level_tolerance) continue;
          const auto original = bodies().GetPointVelocity(support.body, floor->position);
          const auto candidate = bodies().GetPointVelocity(floor->body, floor->position);
          if ((original - candidate).LengthSq() > .20f * .20f) continue;
        }
        const auto feet = floor->position + Vec3(0, feet_offset, 0);
        if (!capsule_fits(feet, clearance)) {
          if (!below_fixture && !widen) widen = capsule_fits(feet, clearance, room_filter{});
          continue;
        }
        if (allow_drop || extended) {
          const auto start = RVec3(pelvis.GetX(), feet.GetY(), pelvis.GetZ());
          if (!capsule_path_clear(start, feet, room_filter{})) continue;
        }
        return support_point{floor->body, feet};
      }
    }
    return std::nullopt;
  };

  const auto feet = search(false);
  // A limb on a higher fixture can win the support query while the pelvis
  // hangs over the floor. Try a nearby lower spot after the usual candidates.
  return feet || on_actor ? feet : search(true);
}

void simulation::world::character_actor::recover() {
  after.recovery_blocked = false;
  auto support = find_ragdoll_support(room_filter{});
  const bool on_actor = !support;
  if (on_actor) support = find_ragdoll_support(actor_filter{});
  if (!track_recovery_motion(support ? &*support : nullptr)) return;
  const auto feet = find_recovery_feet(*support, on_actor);
  after.recovery_blocked = !feet;
  if (!feet) return;

  for (size_t i = 0; i < actor_bodies.size(); ++i) recovery_start[i] = body_pose(bodies(), actor_bodies[i]);
  resume_controller(*feet);
  const auto torso_up = recovery_start[size_t(part::chest)].rotation.RotateAxisY().GetY();
  recovery_age = 0;
  recovery_duration = torso_up > .88f ? 12 : recovery_ticks;
  ++after.recoveries;
  mode() = movement_mode::recovering;
}

std::optional<support_point> simulation::world::character_actor::find_rescue_feet(const RVec3Arg origin) const {
  // Search outward on roughly the same floor level, including the area beside the roof.
  const auto above = origin + Vec3(0, .34f, 0);
  for (const float radius : {0.0f, .20f, .40f, .60f, .80f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f}) {
    const size_t count = radius == 0 ? 1 : 8;
    for (size_t i = 0; i < count; ++i) {
      const float angle = float(i) * pi / 4;
      const auto point = above + radius * Vec3(std::cos(angle), 0, std::sin(angle));
      const auto floor = scene.foot_patch(point, .70f);
      if (!floor) continue;
      const auto feet = floor->position + Vec3(0, .025f, 0);
      if (capsule_fits(feet, .015f)) return support_point{floor->body, feet};
    }
  }
  return std::nullopt;
}

bool simulation::world::character_actor::rescue(const RVec3Arg fallback) {
  if (mode() != movement_mode::falling) return false;
  std::optional<support_point> feet;
  if (const auto support = find_ragdoll_support(room_filter{})) {
    const auto pelvis = bodies().GetPosition(actor_bodies[size_t(part::pelvis)]);
    feet = find_rescue_feet(RVec3(pelvis.GetX(), support->position.GetY(), pelvis.GetZ()));
  }
  if (!feet) feet = find_rescue_feet(fallback);
  if (!feet) return false;

  resume_controller(*feet);
  body_yaw = after.yaw;
  stride_phase = 0;
  ++after.rescues;
  mode() = movement_mode::walking;
  return true;
}

void simulation::world::character_actor::resume_controller(const support_point& feet) {
  const auto support_velocity = bodies().GetPointVelocity(feet.body, feet.position);
  ragdoll->RemoveFromPhysicsSystem();
  create_controller(feet.position);
  character->StickToFloor(Vec3(0, -.05f, 0), scene.physics.GetDefaultBroadPhaseLayerFilter(capsule_layer),
    scene.physics.GetDefaultLayerFilter(capsule_layer), {}, {}, scene.scratch);
  controller_velocity = support_velocity;
  locomotion = external_velocity = Vec3::sZero();
  after.recovery_blocked = false;
}

void simulation::world::character_actor::check_collision_owner() {
  after.capsule_bodies = character != nullptr && bodies().IsAdded(character->GetInnerBodyID());
  if (after.capsule_bodies && bodies().GetUserData(character->GetInnerBodyID()) != actor)
    utils::error{}("PHY03 actor {}: capsule lost its entity ID", actor);
  after.ragdoll_bodies = 0;
  for (const auto id : actor_bodies) {
    if (bodies().IsAdded(id)) ++after.ragdoll_bodies;
    if (bodies().GetUserData(id) != actor)
      utils::error{}("PHY03 actor {}: mismatched ragdoll body user data {}", actor, bodies().GetUserData(id));
  }
  const bool falling = mode() == movement_mode::falling;
  if (!scene.ecs.exists(actor) || after.capsule_bodies != uint32_t(!falling) ||
      after.ragdoll_bodies != (falling ? part_count : 0))
    utils::error{}("PHY03 actor {} at tick {}: mode '{}', {} capsule bodies, {} ragdoll bodies",
                   actor, scene.after.tick, mode_name(mode()), after.capsule_bodies, after.ragdoll_bodies);
}

void simulation::world::character_actor::publish() {
  check_collision_owner();
  previous_pose = after.parts;
  after.entity = actor;
  after.mode = mode();
  after.stepping = step.has_value();
  after.motion_velocity = character != nullptr ? controller_velocity : motion(part::pelvis).linear;
  after.grounded = character != nullptr && grounded(*character);
  after.on_platform = on_platform();
  if (character == nullptr) {
    for (size_t i = 0; i < actor_bodies.size(); ++i) after.parts[i] = body_pose(bodies(), actor_bodies[i]);
    after.eye = after.parts[size_t(part::head)];
    after.head_weight = 1;
    return;
  }

  const float stride = std::sin(stride_phase) * std::min(1.0f, locomotion.Length() / 2.5f);
  const float lean = mode() == movement_mode::staggering ? .24f * std::min(external_velocity.Length() / push_speed(weak_push), 1.8f) : 0;
  const auto pose_feet = character->GetPosition();
  after.parts = animated_pose(pose_feet, body_yaw, stride, lean);
  after.capsule = {pose_feet + Vec3(0, standing_center, 0), Quat::sIdentity()};
  after.query_capsule = {character->GetCenterOfMassPosition(), character->GetRotation()};
  const auto inner_pose = bodies().GetCenterOfMassTransform(character->GetInnerBodyID());
  after.inner_capsule = {inner_pose.GetTranslation(), inner_pose.GetQuaternion()};
  after.eye = {pose_feet + Vec3(0, 1.65f, 0), view_rotation(after.yaw, after.pitch)};
  after.head_weight = 0;
  if (mode() == movement_mode::recovering) {
    const float blend = smoothstep(float(recovery_age) / recovery_duration);
    for (size_t i = 0; i < after.parts.size(); ++i)
      after.parts[i] = interpolate(recovery_start[i], after.parts[i], blend);
    after.eye = interpolate(recovery_start[size_t(part::head)], after.eye, blend);
    after.head_weight = 1 - blend;
  }
}

void simulation::world::publish() {
  player.publish();
  dummy.publish();
  after.player = player.after;
  after.dummy = dummy.after;
  after.platform_speed = platform_speed;
  after.crate_velocity = bodies().GetLinearVelocity(body(fixture::crate));
  after.room = room;
  for (const auto id : {fixture::platform, fixture::crate})
    after.room[size_t(id)].pose = body_pose(bodies(), body(id));
}

float simulation::world::camera_fraction(const JPH::RVec3Arg start, const JPH::RVec3Arg end) const {
  const auto delta = Vec3(end - start);
  if (delta.LengthSq() < 1e-8f) return 1;
  const JPH::RShapeCast cast(camera_sphere, Vec3::sOne(), JPH::RMat44::sTranslation(start), delta);
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
  JPH::ShapeCastSettings settings;
  settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
  physics.GetNarrowPhaseQuery().CastShape(cast, settings, start, collector, {}, {}, ignore_actor(player.actor));
  if (!collector.HadHit()) return 1;
  return std::clamp(collector.mHit.mFraction - .009f / delta.Length(), 0.0f, 1.0f);
}

jolt_runtime::jolt_runtime() {
  JPH::RegisterDefaultAllocator();
  JPH::Factory::sInstance = new JPH::Factory;
  JPH::RegisterTypes();
}

jolt_runtime::~jolt_runtime() {
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
}

simulation::simulation() : state(std::make_unique<world>()) {}
simulation::~simulation() = default;

void simulation::step(const intent& input) {
  auto& w = *state;
  w.before = w.after;
  ++w.after.tick;
  w.move_platform(input.brake);
  w.push_room(input.room_push);
  w.player.advance(input);
  w.dummy.advance({});
  w.separate_controllers();
  const auto error = w.physics.Update(tick_seconds, 2, &w.scratch, &w.jobs);
  if (error != JPH::EPhysicsUpdateError::None)
    utils::error{}("PHY03 physics tick {}: Jolt update flags {}", w.after.tick, uint32_t(error));
  const bool rescued = input.rescue && w.player.rescue(player_start);
  for (auto* actor : {&w.player, &w.dummy}) {
    if (actor->mode() == movement_mode::falling) actor->recover();
  }
  w.publish();
  // A debug relocation must not seed limb velocities on the next handoff.
  if (rescued) w.player.previous_pose = w.after.player.parts;
}

const snapshot& simulation::previous() const noexcept { return state->before; }
const snapshot& simulation::current() const noexcept { return state->after; }

camera_pose simulation::sample_camera(const float alpha, const camera_mode mode) const {
  const auto& b = state->after.player;
  const auto& a = state->before.player.rescues == b.rescues ? state->before.player : b;
  const auto eye = interpolate(a.eye, b.eye, alpha);
  if (mode == camera_mode::overview)
    return {RVec3(-8.6f, 5.5f, 5.6f), view_rotation(.56f, -.44f), false};

  const auto level = view_rotation(angle_lerp(a.yaw, b.yaw, alpha), std::lerp(a.pitch, b.pitch, alpha));
  const auto rotation = mode == camera_mode::head ? eye.rotation : level;
  const float weight = std::lerp(a.head_weight, b.head_weight, alpha);
  const float interpolation_fraction = state->camera_fraction(a.eye.position, eye.position);
  const auto anchor = a.eye.position + (eye.position - a.eye.position) * interpolation_fraction;
  const auto target = anchor + rotation.RotateAxisX() * (.26f * weight);
  const float forward_fraction = state->camera_fraction(anchor, target);
  return {anchor + (target - anchor) * forward_fraction, rotation,
          interpolation_fraction < 1 || forward_fraction < 1};
}

joint_error simulation::measure_joints() const {
  const auto* rig = state->player.ragdoll.GetPtr();
  const auto& bodies = state->bodies();
  joint_error result;
  for (size_t i = 0; i < rig->GetConstraintCount(); ++i) {
    const auto* joint = rig->GetConstraint(int(i));
    const auto [first, second] = state->recipe->GetBodyIndicesForConstraintIndex(int(i));
    const auto a = body_pose(bodies, state->player.actor_bodies[size_t(first)]);
    const auto b = body_pose(bodies, state->player.actor_bodies[size_t(second)]);
    const auto anchor_a = a.position + a.rotation * joint->GetConstraintToBody1Matrix().GetTranslation();
    const auto anchor_b = b.position + b.rotation * joint->GetConstraintToBody2Matrix().GetTranslation();
    result.anchor_distance = std::max(result.anchor_distance, float((anchor_a - anchor_b).Length()));
    if (joint->GetSubType() == JPH::EConstraintSubType::Hinge) {
      const auto& hinge = static_cast<const JPH::HingeConstraint&>(*joint);
      const auto angle = hinge.GetCurrentAngle();
      result.angle = std::max({result.angle, hinge.GetLimitsMin() - angle, angle - hinge.GetLimitsMax()});
    } else {
      const auto& swing_joint = static_cast<const JPH::SwingTwistConstraint&>(*joint);
      Quat swing, twist;
      swing_joint.GetRotationInConstraintSpace().GetSwingTwist(swing, twist);
      const float swing_angle = 2 * std::acos(std::clamp(std::abs(swing.GetW()), 0.0f, 1.0f));
      const float twist_angle = std::abs(twist.GetRotationAngle(Vec3::sAxisX()));
      // The recipe uses a circular cone and symmetric twist limits.
      result.angle = std::max({result.angle, swing_angle - swing_joint.GetNormalHalfConeAngle(),
                              twist_angle - swing_joint.GetTwistMaxAngle()});
    }
  }
  return result;
}

void simulation::apply_impulse(const part target, const JPH::Vec3Arg impulse) {
  state->bodies().AddImpulse(state->player.actor_bodies[size_t(target)], impulse);
}

void simulation::apply_force(const part target, const JPH::Vec3Arg force) {
  state->bodies().AddForce(state->player.actor_bodies[size_t(target)], force);
}

JPH::Vec3 simulation::angular_velocity(const part target) const {
  return state->bodies().GetAngularVelocity(state->player.actor_bodies[size_t(target)]);
}

std::string_view mode_name(const movement_mode mode) {
  switch (mode) {
    case movement_mode::walking: return "walking";
    case movement_mode::staggering: return "staggering";
    case movement_mode::falling: return "ragdoll";
    case movement_mode::recovering: return "recovering";
  }
  utils::error{}("PHY03: unknown movement mode {}", int(mode));
}

transform interpolate(const transform& previous, const transform& current, const float alpha) {
  return {previous.position + (current.position - previous.position) * alpha,
          previous.rotation.SLERP(current.rotation, alpha)};
}

}
