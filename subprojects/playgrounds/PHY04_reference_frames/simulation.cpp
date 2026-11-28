#include "physics_world.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>

#include "devils_engine/aesthetics/world.h"
#include "devils_engine/utils/error.h"

namespace devils_engine::phy04 {

using JPH::Quat;
using JPH::RVec3Arg;

RVec3 point_in_zone(const chunk_point& point, const chunk_key& origin) {
  RVec3 result(point.offset);
  for (size_t i = 0; i < origin.size(); ++i)
    result.SetComponent(i, result[i] + JPH::Real(point.key[i] - origin[i]) * chunk_size);
  return result;
}

chunk_point point_in_chunks(const RVec3Arg in_zone, const chunk_key& origin) {
  chunk_point result{origin, Vec3::sZero()};
  for (size_t i = 0; i < origin.size(); ++i) {
    const auto chunk = int64_t(std::floor(in_zone[i] / chunk_size));
    result.key[i] += chunk;
    result.offset.SetComponent(i, float(in_zone[i] - JPH::Real(chunk) * chunk_size));
  }
  return result;
}

body_state to_outer(const moving_frame& frame, const body_state& local) {
  const auto r = frame.pose.rotation * Vec3(local.pose.position);
  return {
    {frame.pose.position + r, frame.pose.rotation * local.pose.rotation},
    frame.linear + frame.angular.Cross(r) + frame.pose.rotation * local.linear,
    frame.angular + frame.pose.rotation * local.angular
  };
}

body_state to_cabin(const moving_frame& frame, const body_state& outer) {
  const auto r = Vec3(outer.pose.position - frame.pose.position);
  const auto inverse = frame.pose.rotation.Conjugated();
  return {
    {RVec3(inverse * r), inverse * outer.pose.rotation},
    inverse * (outer.linear - frame.linear - frame.angular.Cross(r)),
    inverse * (outer.angular - frame.angular)
  };
}

namespace {

constexpr float half_length = 4.5f;
constexpr float wall_offset = 1.45f;
constexpr float wall_half_width = .08f;
constexpr float deck_height = .62f;
constexpr float cruise_speed = 2;
constexpr float stop_x = 26;
const RVec3 start_hull(19.6f, deck_height, 8);
constexpr float max_turn_speed = .38f;
constexpr float clearance = .025f;

const std::array<solid, 3> outside{{
  {{phy03::primitive::box, {8, .20f, 8}, 0x415766}, {{8, -.20f, 8}}},
  {{phy03::primitive::box, {8, .20f, 8}, 0x536671}, {{24, -.20f, 8}}},
  {{phy03::primitive::box, {.10f, .55f, 3.4f}, 0xc17a53}, {{31.90f, .55f, 7.2f}}},
}};
const std::array<solid, 3> cabin{{
  {{phy03::primitive::box, {half_length, .14f, wall_offset + wall_half_width}, 0x558f84}, {{0, -.14f, 0}}},
  {{phy03::primitive::box, {half_length, .27f, wall_half_width}, 0x6eaaa1}, {{0, .27f, -wall_offset}}},
  {{phy03::primitive::box, {half_length, .27f, wall_half_width}, 0x6eaaa1}, {{0, .27f, wall_offset}}},
}};
static_assert(outside.size() + cabin.size() == fixture_count);

using rig_state = std::array<body_state, part_count>;

JPH::RefConst<JPH::Shape> cabin_shape() {
  JPH::StaticCompoundShapeSettings compound;
  for (const auto& box : cabin)
    compound.AddShape(Vec3(box.pose.position), box.pose.rotation, new JPH::BoxShape(box.shape.size, .02f));
  const auto result = compound.Create();
  if (result.HasError()) utils::error{}("PHY04 cabin compound: {}", result.GetError().c_str());
  return result.Get();
}

bool is_in_cabin(const phase stage) { return stage == phase::cabin || stage == phase::exit; }

}

struct experiment::world {
  JPH::TempAllocatorImpl scratch{2 * 1024 * 1024};
  JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
  physics_world exterior;
  physics_world interior;
  JPH::Ref<JPH::RagdollSettings> recipe = phy03::make_recipe(dynamic_layer);
  aesthetics::world ecs;
  const aesthetics::entityid_t entity = ecs.gen_entityid();
  std::array<JPH::BodyID, outside.size()> terrain;
  JPH::BodyID hull;
  const bool turning;
  const bool rebase;
  JPH::Ref<JPH::Ragdoll> rig;
  snapshot before;
  snapshot after;

  explicit world(bool turning, bool rebase);
  ~world();
  void step();
  void publish();
  void move_hull();
  void shift_origin();
  void transfer(bool enter, const moving_frame& frame, const rig_state& target);
  moving_frame hull_frame() const;
  rig_state read_rig() const;
  JPH::AABox bounds(size_t role, const body_state& state) const;
  void check_owner() const;
};

experiment::world::world(const bool turning, const bool rebase) : turning(turning), rebase(rebase) {
  for (size_t i = 0; i < terrain.size(); ++i)
    terrain[i] = exterior.add(new JPH::BoxShape(outside[i].shape.size, .02f), outside[i].pose.position, false);
  const auto shape = cabin_shape();
  hull = exterior.add(shape, start_hull, true);
  interior.add(shape, RVec3::sZero(), false);

  rig = recipe->CreateRagdoll(1, entity, &exterior.system);
  if (rig == nullptr) utils::error{}("PHY04 initial ragdoll: exhausted exterior body capacity");
  const auto posed = phy03::animated_pose(RVec3::sZero(), 0, 0, 0);
  const auto lying = Quat::sRotation(Vec3::sAxisZ(), -std::numbers::pi_v<float> / 2);
  const RVec3 pelvis(14.1f, 1.40f, 8);
  for (size_t i = 0; i < part_count; ++i) {
    const auto id = rig->GetBodyID(int(i));
    const auto position = pelvis + lying * Vec3(posed[i].position - posed[0].position);
    exterior.bodies().SetPositionAndRotation(id, position, lying * posed[i].rotation, JPH::EActivation::DontActivate);
    exterior.bodies().SetLinearAndAngularVelocity(id, Vec3(8.4f, 0, 0), Vec3::sZero());
  }
  exterior.system.OptimizeBroadPhase();
  interior.system.OptimizeBroadPhase();
  rig->AddToPhysicsSystem(JPH::EActivation::Activate);
}

experiment::world::~world() {
  rig->RemoveFromPhysicsSystem();
  rig = nullptr;
}

moving_frame experiment::world::hull_frame() const {
  moving_frame result;
  const auto& bodies = exterior.bodies();
  bodies.GetPositionAndRotation(hull, result.pose.position, result.pose.rotation);
  result.linear = bodies.GetPointVelocity(hull, result.pose.position);
  result.angular = bodies.GetAngularVelocity(hull);
  return result;
}

rig_state experiment::world::read_rig() const {
  const auto& bodies = is_in_cabin(after.stage) ? interior.bodies() : exterior.bodies();
  rig_state result;
  for (size_t i = 0; i < result.size(); ++i) result[i] = read_body(bodies, rig->GetBodyID(int(i)));
  return result;
}

JPH::AABox experiment::world::bounds(const size_t role, const body_state& state) const {
  return recipe->mParts[role].GetShape()->GetWorldSpaceBounds(
    JPH::Mat44::sRotationTranslation(state.pose.rotation, Vec3(state.pose.position)), Vec3::sOne());
}

void experiment::world::move_hull() {
  const auto seconds = after.tick * phy03::tick_seconds;
  const auto x = std::min(stop_x, float(start_hull.GetX()) + cruise_speed * seconds);
  const auto turn_age = after.kick_tick != 0 ? after.tick - after.kick_tick : 0;
  const auto yaw = turning ? max_turn_speed * std::min(110u, turn_age) * phy03::tick_seconds : 0.0f;
  const auto position = RVec3(x, start_hull.GetY(), start_hull.GetZ()) - point_in_zone({after.origin, Vec3::sZero()}, first_chunk);
  exterior.bodies().MoveKinematic(hull, position, Quat::sRotation(Vec3::sAxisY(), yaw), phy03::tick_seconds);
}

void experiment::world::shift_origin() {
  const RVec3 delta = point_in_zone({after.origin, Vec3::sZero()}, second_chunk);
  auto& bodies = exterior.bodies();
  JPH::BodyIDVector ids;
  exterior.system.GetBodies(ids);
  const auto floor_contact = [&](const RVec3Arg start) {
    const JPH::RRayCast ray(start, Vec3(0, -1, 0));
    JPH::RayCastResult hit;
    if (!exterior.system.GetNarrowPhaseQuery().CastRay(ray, hit) || hit.mBodyID != terrain[0])
      utils::error{}("PHY04 rebase: missed first chunk floor from ({}, {}, {})", start.GetX(), start.GetY(), start.GetZ());
    return hit.mFraction;
  };
  const auto ray_start = point_in_zone({first_chunk, Vec3(2, .7f, 2)}, after.origin);
  const auto height = floor_contact(ray_start);
  for (const auto id : ids) {
    const auto old = read_body(bodies, id);
    JPH::RVec3 position;
    Quat rotation;
    bodies.GetPositionAndRotation(id, position, rotation);
    bodies.SetPositionAndRotation(id, position + delta, rotation, JPH::EActivation::DontActivate);
    auto next = read_body(bodies, id);
    next.pose.position -= delta;
    accumulate(after.rebase_error, old, next);
  }
  if (std::abs(floor_contact(ray_start + delta) - height) > continuity_tolerance)
    utils::error{}("PHY04 rebase: a floor ray changed its hit distance after translating both endpoints");
  after.origin = second_chunk;
  after.rebase_tick = after.tick;
  require_continuous(after.rebase_error, "zone rebase");
}

void experiment::world::transfer(const bool enter, const moving_frame& frame, const rig_state& target) {
  auto& destination = enter ? interior : exterior;
  const auto previous = read_rig();
  auto arrival = recipe->CreateRagdoll(1, entity, &destination.system);
  if (arrival == nullptr) utils::error{}("PHY04 transfer at tick {}: exhausted receiving body capacity", after.tick);
  for (size_t i = 0; i < target.size(); ++i) {
    const auto id = arrival->GetBodyID(int(i));
    const auto& body = target[i];
    // The shared recipe uses centered boxes, spheres and capsules for every link.
    destination.bodies().SetPositionAndRotation(id, body.pose.position, body.pose.rotation, JPH::EActivation::DontActivate);
    destination.bodies().SetLinearAndAngularVelocity(id, body.linear, body.angular);
  }
  rig->RemoveFromPhysicsSystem();
  arrival->AddToPhysicsSystem(JPH::EActivation::Activate);
  rig = std::move(arrival);
  auto& error = enter ? after.enter_error : after.leave_error;
  auto& tick = enter ? after.enter_tick : after.leave_tick;
  tick = after.tick;
  after.stage = enter ? phase::cabin : phase::outside;
  const auto next = read_rig();
  for (size_t i = 0; i < next.size(); ++i) {
    const auto outgoing = enter ? previous[i] : to_outer(frame, previous[i]);
    const auto incoming = enter ? to_outer(frame, next[i]) : next[i];
    accumulate(error, outgoing, incoming);
  }
  require_continuous(error, enter ? "cabin entry" : "cabin exit");
}

void experiment::world::check_owner() const {
  const bool inside = is_in_cabin(after.stage);
  constexpr auto exterior_fixtures = outside.size() + 1;
  const auto outside_count = exterior.system.GetNumBodies();
  const auto inside_count = interior.system.GetNumBodies();
  if (outside_count != exterior_fixtures + (inside ? 0 : part_count) ||
      inside_count != 1 + (inside ? part_count : 0) ||
      exterior.system.GetConstraints().size() != (inside ? 0 : part_count - 1) ||
      interior.system.GetConstraints().size() != (inside ? part_count - 1 : 0))
    utils::error{}("PHY04 tick {}: wrong ragdoll ownership, {} exterior and {} interior bodies",
      after.tick, outside_count, inside_count);
  const auto& owner = inside ? interior.bodies() : exterior.bodies();
  for (const auto id : rig->GetBodyIDs()) {
    if (!owner.IsAdded(id) || owner.GetUserData(id) != entity)
      utils::error{}("PHY04 tick {}: ragdoll ownership or entity mismatch at body {}", after.tick, id.GetIndex());
  }
}

void experiment::world::publish() {
  check_owner();
  const auto frame = hull_frame();
  const auto display_offset = point_in_zone({after.origin, Vec3::sZero()}, first_chunk);
  after.entity = entity;
  after.hull = frame;
  after.hull.pose.position += display_offset;
  std::copy(outside.begin(), outside.end(), after.fixtures.begin());
  for (size_t i = 0; i < cabin.size(); ++i) {
    after.fixtures[outside.size() + i] = cabin[i];
    after.fixtures[outside.size() + i].pose = to_outer(after.hull, {cabin[i].pose}).pose;
  }
  const bool inside = is_in_cabin(after.stage);
  const auto motion = read_rig();
  after.pelvis_zone = inside ? to_outer(frame, motion[0]) : motion[0];
  after.pelvis_cabin = inside ? motion[0] : to_cabin(frame, motion[0]);
  after.pelvis_address = point_in_chunks(after.pelvis_zone.pose.position, after.origin);
  for (size_t i = 0; i < after.parts.size(); ++i) {
    after.parts[i] = inside ? to_outer(frame, motion[i]).pose : motion[i].pose;
    after.parts[i].position += display_offset;
  }
  after.joint_gap = 0;
  const auto& links = recipe->GetConstraintIndexToBodyIdxPair();
  for (size_t i = 0; i < links.size(); ++i) {
    const auto* joint = rig->GetConstraint(int(i));
    const auto& a = motion[size_t(links[i].first)].pose;
    const auto& b = motion[size_t(links[i].second)].pose;
    const auto anchor_a = a.position + a.rotation * joint->GetConstraintToBody1Matrix().GetTranslation();
    const auto anchor_b = b.position + b.rotation * joint->GetConstraintToBody2Matrix().GetTranslation();
    after.joint_gap = std::max(after.joint_gap, float((anchor_a - anchor_b).Length()));
  }
}

void experiment::world::step() {
  if (after.stage == phase::finished) return;
  before = after;
  ++after.tick;
  move_hull();
  exterior.step(scratch, jobs, after.tick, "outside");
  interior.step(scratch, jobs, after.tick, "cabin");

  if (after.stage == phase::approach) {
    if (rebase && after.rebase_tick == 0 && exterior.bodies().GetPosition(rig->GetBodyID(0)).GetX() >= chunk_size)
      shift_origin();
    const auto frame = hull_frame();
    auto target = read_rig();
    bool fits = true;
    for (size_t i = 0; i < target.size(); ++i) {
      target[i] = to_cabin(frame, target[i]);
      const auto box = bounds(i, target[i]);
      fits &= box.mMin.GetX() >= -half_length + clearance && box.mMax.GetX() <= half_length - clearance &&
              box.mMin.GetY() >= clearance && box.mMin.GetZ() >= -wall_offset + wall_half_width + clearance &&
              box.mMax.GetZ() <= wall_offset - wall_half_width - clearance;
    }
    if (fits) transfer(true, frame, target);
  } else if (after.stage == phase::cabin && after.tick == after.enter_tick + 90) {
    rig->AddLinearVelocity(Vec3(13, 0, 0));
    after.kick_tick = after.tick;
    after.stage = phase::exit;
  } else if (after.stage == phase::exit) {
    const auto frame = hull_frame();
    auto target = read_rig();
    bool clear = true;
    for (size_t i = 0; i < target.size(); ++i) {
      clear &= bounds(i, target[i]).mMin.GetX() > half_length + clearance;
      target[i] = to_outer(frame, target[i]);
      clear &= bounds(i, target[i]).mMin.GetY() >= clearance;
    }
    if (clear) transfer(false, frame, target);
  } else if (after.stage == phase::outside && after.tick == after.leave_tick + 60) {
    after.stage = phase::finished;
  }
  publish();
}

experiment::experiment(const bool turning, const bool rebase) : state(std::make_unique<world>(turning, rebase)) {
  state->publish();
  state->before = state->after;
}
experiment::~experiment() = default;
void experiment::step() { state->step(); }
const snapshot& experiment::current() const noexcept { return state->after; }
const snapshot& experiment::previous() const noexcept { return state->before; }

std::string_view phase_name(const phase value) {
  switch (value) {
    case phase::approach: return "approach";
    case phase::cabin: return "cabin";
    case phase::exit: return "exit";
    case phase::outside: return "outside";
    case phase::finished: return "finished";
  }
  utils::error{}("PHY04 unknown phase {}", int(value));
}

void print_sample(const snapshot& sample) {
  const auto& key = sample.pelvis_address.key;
  std::cout << "tick " << sample.tick << " " << phase_name(sample.stage)
            << " origin " << sample.origin[0] << " chunk (" << key[0] << ',' << key[1] << ',' << key[2]
            << ") + (" << sample.pelvis_address.offset << ")"
            << " | zone " << sample.pelvis_zone.pose.position << " v " << sample.pelvis_zone.linear
            << " | cabin " << sample.pelvis_cabin.pose.position << " v " << sample.pelvis_cabin.linear
            << " | hull " << sample.hull.pose.position << " omega " << sample.hull.angular
            << " | joint gap " << sample.joint_gap << '\n';
}

void print_points(const snapshot& sample) {
  const auto shift = point_in_zone({sample.origin, Vec3::sZero()}, first_chunk);
  const auto print = [&](const std::string_view label, const RVec3Arg display_position) {
    const auto zone = display_position - shift;
    const auto address = point_in_chunks(zone, sample.origin);
    const auto local = to_cabin(sample.hull, {{display_position}}).pose.position;
    std::cout << "  " << label << " chunk (" << address.key[0] << ',' << address.key[1] << ',' << address.key[2]
              << ") + (" << address.offset << ") | zone " << zone << " | cabin " << local << '\n';
  };
  print("first chunk", point_in_zone({first_chunk, chunk_marker}, first_chunk));
  print("second chunk", point_in_zone({second_chunk, chunk_marker}, first_chunk));
  print("deck", to_outer(sample.hull, {{RVec3(cabin_marker)}}).pose.position);
  print("pelvis", sample.parts[0].position);
}

}
