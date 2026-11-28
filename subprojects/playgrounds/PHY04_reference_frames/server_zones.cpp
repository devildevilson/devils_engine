#include "server_zones.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include "devils_engine/aesthetics/world.h"
#include "devils_engine/utils/error.h"

#include "physics_world.h"

namespace devils_engine::phy04 {
namespace {

constexpr float speed = 9;
constexpr float speed_bound = 12;
constexpr float step_reach = 2 * speed_bound * phy03::tick_seconds;
constexpr float merge_distance = 24 * step_reach + .75f;
constexpr uint32_t release_input_tick = 340;
constexpr uint32_t player_pair = 1;
constexpr uint32_t cargo_pair = 2;
constexpr uint32_t mixed_pair = 4;

using entities = std::array<aesthetics::entityid_t, zone_actor_count>;

bool is_player(const size_t actor) { return actor % 2 == 0; }
bool starts_on_left(const size_t actor) { return actor < 2; }

chunk_key at_x(const int64_t x) { return {x, first_chunk[1], first_chunk[2]}; }

struct chunk_range {
  int64_t first = std::numeric_limits<int64_t>::max();
  int64_t last = std::numeric_limits<int64_t>::min();

  bool operator==(const chunk_range&) const = default;

  void include(const int64_t lo, const int64_t hi) {
    first = std::min(first, lo);
    last = std::max(last, hi);
  }
};

struct arrivals final : JPH::ContactListener {
  explicit arrivals(const entities& ids) : ids(ids) {}
  const entities& ids;
  uint32_t pairs = 0;

  void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&,
                      JPH::ContactSettings&) override {
    if (a.GetObjectLayer() != dynamic_layer || b.GetObjectLayer() != dynamic_layer) return;
    const auto i = size_t(std::find(ids.begin(), ids.end(), a.GetUserData()) - ids.begin());
    const auto j = size_t(std::find(ids.begin(), ids.end(), b.GetUserData()) - ids.begin());
    if (i >= ids.size() || j >= ids.size() || starts_on_left(i) == starts_on_left(j)) return;
    if (is_player(i) != is_player(j)) pairs |= mixed_pair;
    else pairs |= is_player(i) ? player_pair : cargo_pair;
  }
};

struct zone {
  const chunk_key origin;
  arrivals contacts;
  std::optional<chunk_range> resident;
  JPH::BodyID floor;
  physics_world physics;

  zone(const chunk_key& origin, const entities& ids) : origin(origin), contacts(ids) {
    physics.system.SetContactListener(&contacts);
  }

  void load_floor(const chunk_range& range) {
    if (resident == range) return;
    if (range.last < range.first || range.last - range.first >= 8)
      utils::error{}("PHY04 zone {}: floor range {}..{} exceeds 8 chunks", origin[0], range.first, range.last);
    // The flat resident corridor is one surface. Chunk edges only affect its address and extent.
    constexpr float half_depth = .20f;
    constexpr float half_width = chunk_size * .5f;
    const float half_length = float(range.last - range.first + 1) * half_width;
    const auto center = point_in_zone({at_x(range.first), Vec3::sZero()}, origin) +
      Vec3(half_length, -half_depth, half_width);
    const auto next = physics.add(new JPH::BoxShape(Vec3(half_length, half_depth, half_width), .02f), center, false);
    if (!floor.IsInvalid()) {
      physics.bodies().RemoveBody(floor);
      physics.bodies().DestroyBody(floor);
    }
    floor = next;
    resident = range;
  }
};

struct body_slot {
  zone_id owner = zone_id::left;
  JPH::BodyID body;
};

}

struct server_experiment::server {
  JPH::TempAllocatorImpl scratch{2 * 1024 * 1024};
  JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
  JPH::RefConst<JPH::Shape> player = new JPH::CapsuleShape(player_half_cylinder, player_radius);
  JPH::RefConst<JPH::Shape> cargo = new JPH::SphereShape(cargo_radius);
  aesthetics::world ecs;
  entities ids;
  std::array<std::unique_ptr<zone>, zone_count> zones;
  std::array<body_slot, zone_actor_count> slots;
  server_snapshot before;
  server_snapshot after;

  server();
  zone& owner(size_t actor) { return *zones[size_t(slots[actor].owner)]; }
  const zone& owner(size_t actor) const { return *zones[size_t(slots[actor].owner)]; }
  body_state display_motion(size_t actor) const;
  void load_residency();
  void merge(float gap);
  void apply_input();
  void publish();
  void step();
};

server_experiment::server::server() {
  for (auto& entity : ids) entity = ecs.gen_entityid();
  zones[size_t(zone_id::left)] = std::make_unique<zone>(first_chunk, ids);
  zones[size_t(zone_id::right)] = std::make_unique<zone>(at_x(right_start_chunk_x), ids);
  for (size_t i = 0; i < slots.size(); ++i) {
    const bool left = starts_on_left(i);
    const bool avatar = is_player(i);
    auto& slot = slots[i];
    slot.owner = left ? zone_id::left : zone_id::right;
    const float x = avatar ? 8.0f : left ? 10.0f : 6.0f;
    const float y = avatar ? player_radius + player_half_cylinder : cargo_radius;
    JPH::BodyCreationSettings settings(avatar ? player.GetPtr() : cargo.GetPtr(),
      RVec3(x, y + .02f, avatar ? 6 : 10), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, dynamic_layer);
    settings.mUserData = ids[i];
    settings.mFriction = 0;
    settings.mRestitution = avatar ? .20f : .60f;
    settings.mLinearDamping = 0;
    settings.mAngularDamping = 0;
    settings.mAllowSleeping = false;
    settings.mMaxLinearVelocity = speed_bound;
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    if (avatar) settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY |
                                        JPH::EAllowedDOFs::TranslationZ;
    else {
      settings.mLinearVelocity = Vec3(left ? speed : -speed, 0, 0);
      settings.mAngularVelocity = Vec3(0, 0, left ? -2.0f : 2.0f);
    }
    slot.body = owner(i).physics.bodies().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (slot.body.IsInvalid()) utils::error{}("PHY04 initial server zone {}: could not allocate entity {}", zone_name(slot.owner), ids[i]);
  }
  load_residency();
  publish();
  before = after;
}

body_state server_experiment::server::display_motion(const size_t actor) const {
  const auto& zone = owner(actor);
  auto result = read_body(zone.physics.bodies(), slots[actor].body);
  result.pose.position += point_in_zone({zone.origin, Vec3::sZero()}, first_chunk);
  return result;
}

void server_experiment::server::load_residency() {
  std::array<chunk_range, zone_count> ranges;
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& region = owner(i);
    const auto x = region.physics.bodies().GetPosition(slots[i].body).GetX();
    const auto key = point_in_chunks(RVec3(x, 0, 0), region.origin).key[0];
    ranges[size_t(slots[i].owner)].include(key - 1, key + 1);
  }
  for (size_t i = 0; i < zones.size(); ++i)
    if (zones[i]) zones[i]->load_floor(ranges[i]);
}

void server_experiment::server::merge(const float gap) {
  if (gap <= step_reach)
    utils::error{}("PHY04 server tick {}: merge gap {} m is inside one tick's reach of {} m", after.tick, gap, step_reach);
  const auto a = display_motion(1).pose.position;
  const auto b = display_motion(3).pose.position;
  const auto origin = at_x(point_in_chunks((a + b) * .5f, first_chunk).key[0]);
  auto combined = std::make_unique<zone>(origin, ids);
  std::array<JPH::BodyID, zone_actor_count> staged;
  std::array<body_state, zone_actor_count> before_merge;
  chunk_range range;
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& source = owner(i);
    before_merge[i] = display_motion(i);
    JPH::BodyLockRead lock(source.physics.system.GetBodyLockInterface(), slots[i].body);
    if (!lock.Succeeded()) utils::error{}("PHY04 server tick {}: missing source entity {}", after.tick, ids[i]);
    auto settings = lock.GetBody().GetBodyCreationSettings();
    const auto address = point_in_chunks(settings.mPosition, source.origin);
    settings.mPosition = point_in_zone(address, origin);
    range.include(address.key[0] - 1, address.key[0] + 1);
    const auto* body = combined->physics.bodies().CreateBody(settings);
    if (body == nullptr) utils::error{}("PHY04 server tick {}: common zone cannot stage entity {}", after.tick, ids[i]);
    staged[i] = body->GetID();
  }
  combined->load_floor(range);

  // No simulation step or publication takes place until all ownership has changed.
  zones[size_t(zone_id::left)].reset();
  zones[size_t(zone_id::right)].reset();
  zones[size_t(zone_id::common)] = std::move(combined);
  for (size_t i = 0; i < slots.size(); ++i) {
    slots[i] = {zone_id::common, staged[i]};
    owner(i).physics.bodies().AddBody(staged[i], JPH::EActivation::Activate);
    accumulate(after.merge_error, before_merge[i], display_motion(i));
  }
  require_continuous(after.merge_error, "server zone merge");
  after.merged_tick = after.tick;
  after.merge_gap = gap;
}

void server_experiment::server::apply_input() {
  if (after.tick > release_input_tick) return;
  for (const size_t i : {size_t(0), size_t(2)}) {
    auto& bodies = owner(i).physics.bodies();
    const auto id = slots[i].body;
    auto velocity = bodies.GetLinearVelocity(id);
    // Opposing recorded movement intents, with a bounded acceleration.
    const float goal = starts_on_left(i) ? speed : -speed;
    constexpr float increment = 18 * phy03::tick_seconds;
    velocity.SetX(velocity.GetX() + std::clamp(goal - velocity.GetX(), -increment, increment));
    bodies.SetLinearVelocity(id, velocity);
  }
}

void server_experiment::server::publish() {
  after.floors.clear();
  for (size_t i = 0; i < zones.size(); ++i) {
    after.active_zones[i] = zones[i] != nullptr;
    if (!zones[i]) continue;
    const auto& region = *zones[i];
    after.origins[i] = region.origin;
    const auto assigned = std::count_if(slots.begin(), slots.end(), [&](const auto& slot) { return size_t(slot.owner) == i; });
    if (!region.resident || !region.physics.bodies().IsAdded(region.floor) ||
        region.physics.system.GetNumBodies() != 1 + size_t(assigned))
      utils::error{}("PHY04 server tick {}: zone {} has incorrect floor or body ownership", after.tick, i);
    for (auto key = region.resident->first; key <= region.resident->last; ++key) {
      const auto found = std::find_if(after.floors.begin(), after.floors.end(), [&](const auto& cell) { return cell.chunk_x == key; });
      if (found != after.floors.end()) found->owners |= 1u << i;
      else after.floors.push_back({key, 1u << i});
    }
  }
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    const auto& bodies = owner(i).physics.bodies();
    if (!bodies.IsAdded(slot.body) || bodies.GetUserData(slot.body) != ids[i] || after.actors[i].steps != after.tick)
      utils::error{}("PHY04 server tick {}: ownership or step mismatch for entity {}", after.tick, ids[i]);
    auto& target = after.actors[i];
    target.entity = ids[i];
    target.owner = slot.owner;
    target.player = is_player(i);
    target.motion = display_motion(i);
    target.address = point_in_chunks(target.motion.pose.position, first_chunk);
  }
}

void server_experiment::server::step() {
  if (after.tick >= zone_step_budget) return;
  before = after;
  ++after.tick;
  if (after.merged_tick == 0) {
    // The test has parallel lanes along X. Spheres and upright capsules have fixed horizontal extents.
    const auto edge = [&](const size_t i, const float sign) {
      return float(display_motion(i).pose.position.GetX()) + sign * (is_player(i) ? player_radius : cargo_radius);
    };
    const auto left_edge = std::max(edge(0, 1), edge(1, 1));
    const auto right_edge = std::min(edge(2, -1), edge(3, -1));
    if (right_edge - left_edge <= merge_distance) merge(right_edge - left_edge);
  }
  load_residency();
  apply_input();
  for (size_t i = 0; i < zones.size(); ++i) {
    if (!zones[i]) continue;
    auto& region = *zones[i];
    region.contacts.pairs = 0;
    region.physics.step(scratch, jobs, after.tick, zone_name(zone_id(i)));
    ++after.zone_steps[i];
    for (size_t k = 0; k < slots.size(); ++k)
      if (size_t(slots[k].owner) == i && region.physics.bodies().IsAdded(slots[k].body)) ++after.actors[k].steps;
    const auto contacts = region.contacts.pairs;
    if (contacts != 0) {
      if (i != size_t(zone_id::common) || after.merged_tick == 0 || (contacts & mixed_pair) != 0)
        utils::error{}("PHY04 server tick {} zone {}: unexpected inter-group contact mask {}", after.tick, i, contacts);
      if (after.first_contact_tick == 0) after.first_contact_tick = after.tick;
      if ((contacts & cargo_pair) != 0 && after.cargo_contact_tick == 0) after.cargo_contact_tick = after.tick;
      if ((contacts & player_pair) != 0 && after.player_contact_tick == 0) after.player_contact_tick = after.tick;
    }
  }
  publish();
}

server_experiment::server_experiment() : state(std::make_unique<server>()) {}
server_experiment::~server_experiment() = default;
void server_experiment::step() { state->step(); }
const server_snapshot& server_experiment::current() const noexcept { return state->after; }
const server_snapshot& server_experiment::previous() const noexcept { return state->before; }

std::string_view zone_name(const zone_id id) {
  switch (id) {
    case zone_id::left: return "left";
    case zone_id::right: return "right";
    case zone_id::common: return "common";
    case zone_id::count: break;
  }
  utils::error{}("PHY04 unknown server zone {}", size_t(id));
}

void print_zones(const server_snapshot& sample) {
  std::cout << "tick " << sample.tick << " | zones";
  for (size_t i = 0; i < sample.active_zones.size(); ++i)
    if (sample.active_zones[i]) std::cout << ' ' << zone_name(zone_id(i)) << '@' << sample.origins[i][0];
  std::cout << " | merge " << sample.merged_tick << " gap " << sample.merge_gap
            << " m | cargo contact " << sample.cargo_contact_tick << " player contact " << sample.player_contact_tick << '\n';
  if (sample.merged_tick != 0)
    std::cout << "  merge errors: position " << sample.merge_error.position << ", rotation " << sample.merge_error.rotation
              << ", linear " << sample.merge_error.linear << ", angular " << sample.merge_error.angular << '\n';
  for (const auto& actor : sample.actors)
    std::cout << "  " << (actor.player ? "player " : "cargo ") << actor.entity << " [" << zone_name(actor.owner)
              << "] chunk " << actor.address.key[0] << " + " << actor.address.offset.GetX()
              << " m | scene " << actor.motion.pose.position << " | v " << actor.motion.linear
              << " | steps " << actor.steps << '\n';
}

}
