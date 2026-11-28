#include "frames.h"
#include "server_zones.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>

#include "devils_engine/utils/error.h"

namespace devils_engine::phy04 {
namespace {

constexpr float tolerance = .0002f;

void require(const bool valid, const std::string_view reason) {
  if (!valid) utils::error{}("PHY04 verification: {}", reason);
}

void verify_coordinates() {
  const RVec3 point(-.5f, -16, 16);
  const auto address = point_in_chunks(point, first_chunk);
  require(address.key == chunk_key{first_chunk[0] - 1, -1, first_chunk[2] + 1} &&
          address.offset == Vec3(15.5f, 0, 0), "negative coordinates or exact chunk edges");
  require(point_in_zone(address, first_chunk) == point && point_in_zone(address, second_chunk) == RVec3(-16.5f, -16, 16),
          "chunk round trip with large signed keys");

  moving_frame frame{{RVec3(19, 1, 3)}, Vec3(4, 0, 0)};
  const body_state local{{RVec3(2, 0, 0)}, Vec3(2, 0, 0), Vec3::sAxisZ()};
  require(to_outer(frame, local).linear == Vec3(6, 0, 0), "linear departure must inherit the hull speed once");
  frame.pose.rotation = JPH::Quat::sRotation(Vec3::sAxisY(), std::numbers::pi_v<float> / 2);
  frame.angular = Vec3::sAxisY();
  const auto outer = to_outer(frame, local);
  require((outer.pose.position - RVec3(19, 1, 1)).Length() < tolerance &&
          (outer.linear - Vec3(2, 0, -2)).Length() < tolerance &&
          (outer.angular - Vec3(1, 1, 0)).Length() < tolerance, "turning frame point and twist");
  const auto back = to_cabin(frame, outer);
  require((back.pose.position - local.pose.position).Length() < tolerance &&
          (back.linear - local.linear).Length() < tolerance &&
          (back.angular - local.angular).Length() < tolerance, "inverse turning transfer");
}

void verify_replay(const bool turning) {
  experiment sim(turning);
  experiment fixed_origin(turning, false);
  float worst_joint_gap = 0;
  bool rode = false;
  bool deck_crossed_chunk = false;
  bool moved_on_deck = false;
  bool unrebased_reference = false;
  auto departure = RVec3::sZero();
  while (sim.current().tick < step_budget && sim.current().stage != phase::finished) {
    sim.step();
    fixed_origin.step();
    const auto& now = sim.current();
    const auto& old = sim.previous();
    worst_joint_gap = std::max(worst_joint_gap, now.joint_gap);
    require(now.entity == old.entity, "entity changed at boundary");
    const auto round_trip = point_in_zone(now.pelvis_address, now.origin);
    require((round_trip - now.pelvis_zone.pose.position).Length() < tolerance, "published point lost its world address");
    if (now.rebase_tick != 0 && now.tick <= now.rebase_tick + 1) {
      require(now.origin == second_chunk && now.pelvis_address.key == fixed_origin.current().pelvis_address.key &&
              (now.parts[0].position - fixed_origin.current().parts[0].position).Length() < .002f,
              "rebase changed the physical trajectory across the seam");
      unrebased_reference = true;
    }
    if (now.stage == phase::cabin && (now.pelvis_cabin.pose.position - old.pelvis_cabin.pose.position).Length() < .002f &&
        (now.hull.pose.position - old.hull.pose.position).Length() > .025f)
      rode = true;
    if (now.stage == phase::cabin) {
      const auto deck = to_outer(now.hull, {{RVec3(cabin_marker)}}).pose.position;
      const auto previous_deck = to_outer(old.hull, {{RVec3(cabin_marker)}}).pose.position;
      const auto deck_address = point_in_chunks(deck, first_chunk);
      require((point_in_zone(deck_address, first_chunk) - deck).Length() < tolerance &&
              (to_cabin(now.hull, {{deck}}).pose.position - RVec3(cabin_marker)).Length() < tolerance,
              "cabin marker disagrees with the shared coordinate views");
      deck_crossed_chunk |= previous_deck.GetX() < chunk_size && deck.GetX() >= chunk_size;
    }
    if (now.stage == phase::exit && now.pelvis_cabin.linear.GetX() > 1)
      moved_on_deck = true;
    if (now.tick == now.leave_tick) {
      departure = now.parts[0].position;
      require((now.hull.angular.Length() > .30f) == turning, "wrong hull rotation at departure");
    }
  }
  const auto& result = sim.current();
  require(result.stage == phase::finished && result.rebase_tick > 0 && result.rebase_tick < result.enter_tick &&
          result.enter_tick < result.kick_tick && result.kick_tick < result.leave_tick,
          "replay did not cross the chunk seam and both cabin boundaries in order");
  require(unrebased_reference && rode && moved_on_deck && deck_crossed_chunk,
          "missing stationary ride, relative deck motion, deck marker crossing or rebase sample");
  require(result.parts[0].position.GetY() < departure.GetY() - .15f &&
          (result.parts[0].position - departure).Length() > .35f, "ragdoll did not continue physically after exit");
  require(worst_joint_gap < .10f, "joint anchors tore during replay");
  std::cout << "PHY04 " << (turning ? "turning" : "straight") << ": rebase " << result.rebase_tick
            << ", enter " << result.enter_tick << ", leave " << result.leave_tick
            << ", maximum observed joint gap " << worst_joint_gap << " m\n";
}

}

void verify() {
  verify_coordinates();
  verify_replay(false);
  verify_replay(true);
}

void verify_zones() {
  server_experiment sim;
  const auto initial = sim.current();
  require(initial.active_zones == std::array{true, true, false} &&
          initial.origins[0][0] == first_chunk[0] && initial.origins[1][0] == right_start_chunk_x &&
          (initial.actors[2].motion.pose.position - initial.actors[0].motion.pose.position).Length() == 128,
          "two separated server worlds must start eight chunks apart");
  bool left_changed_chunk = false;
  bool right_changed_chunk = false;
  float highest_cargo = cargo_radius;
  float slowest_cargo = 9;
  while (sim.current().tick < zone_step_budget) {
    sim.step();
    const auto& now = sim.current();
    if (now.tick > 30 && now.cargo_contact_tick == 0) {
      for (const auto i : {1u, 3u}) {
        const auto& motion = now.actors[i].motion;
        highest_cargo = std::max(highest_cargo, float(motion.pose.position.GetY()));
        slowest_cargo = std::min(slowest_cargo, std::abs(motion.linear.GetX()));
      }
    }
    for (size_t i = 0; i < now.actors.size(); ++i) {
      const auto& actor = now.actors[i];
      require(actor.entity == initial.actors[i].entity && actor.steps == now.tick,
              "entity identity or exactly-once stepping changed during the server run");
      require((point_in_zone(actor.address, first_chunk) - actor.motion.pose.position).Length() < tolerance,
              "published server body lost its chunk address");
      const auto tile = std::find_if(now.floors.begin(), now.floors.end(), [&](const auto& floor) {
        return floor.chunk_x == actor.address.key[0];
      });
      require(tile != now.floors.end() && (tile->owners & (1u << size_t(actor.owner))) != 0,
              "server body stepped outside its loaded floor");
    }
    if (now.merged_tick == 0) {
      require(now.active_zones == initial.active_zones && now.first_contact_tick == 0 &&
              now.zone_steps == std::array<uint32_t, zone_count>{now.tick, now.tick, 0},
              "separate zone clocks or contacts differ before the merge");
      left_changed_chunk |= now.actors[0].address.key[0] > initial.actors[0].address.key[0];
      right_changed_chunk |= now.actors[2].address.key[0] < initial.actors[2].address.key[0];
    } else {
      require(now.active_zones == std::array{false, false, true} &&
              now.zone_steps == std::array<uint32_t, zone_count>{
                now.merged_tick - 1, now.merged_tick - 1, now.tick - now.merged_tick + 1},
              "retired worlds must stop at the same tick the common world starts");
      require(std::all_of(now.floors.begin(), now.floors.end(), [](const auto& tile) {
        return tile.owners == (1u << size_t(zone_id::common));
      }), "common floor kept a copy from the retired zones");
    }
  }
  const auto& result = sim.current();
  require(left_changed_chunk && right_changed_chunk, "both distant groups must travel across chunks while separated");
  require(result.merged_tick > 0 && result.merge_gap > 1 &&
          result.first_contact_tick > result.merged_tick &&
          result.cargo_contact_tick >= result.first_contact_tick && result.player_contact_tick >= result.first_contact_tick,
          "the server must merge before either physical contact");
  require(result.actors[1].motion.linear.GetX() < -1 && result.actors[3].motion.linear.GetX() > 1,
          "the common solver did not bounce the opposing cargo");
  if (highest_cargo - cargo_radius > .01f || slowest_cargo < 8.9f)
    utils::error{}("PHY04 floor traversal before cargo contact: lift {} m, lowest forward speed {} m/s",
      highest_cargo - cargo_radius, slowest_cargo);
  print_zones(result);
}

}
