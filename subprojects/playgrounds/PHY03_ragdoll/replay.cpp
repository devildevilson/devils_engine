#include "replay.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <numeric>
#include <utility>
#include <vector>

#include "devils_engine/utils/error.h"

namespace devils_engine::phy03 {
namespace {

using JPH::Vec3;

void require(const bool condition, const std::string_view reason) {
  if (!condition) utils::error{}("PHY03 verification: {}", reason);
}

struct observations {
  bool climbed_step = false;
  bool rode_platform = false;
  bool staggered = false;
  bool fell = false;
  bool stood_up = false;
  bool dummy_stood_up = false;
  bool player_toppled = false;
  bool dummy_toppled = false;
  bool head_comparison = false;
  float crate_speed_before_brake = 0;
  float crate_speed_after_brake = 0;
  float riding_relative_x = 0;
  float walking_speed = 0;
  float stagger_speed = 0;
  float anchor_error = 0;
  float angle_error = 0;
  uint32_t blocked_ticks = 0;
  std::vector<uint64_t> trace;
};

observations observe(const replay_case scenario, const uint32_t render_rate, const bool dump) {
  simulation sim;
  fixed_clock clock;
  observations result;
  result.trace.reserve(replay_ticks);
  const auto player_id = sim.current().player.entity;
  const auto dummy_id = sim.current().dummy.entity;
  require(player_id != dummy_id, "player and dummy share an entity ID");
  while (sim.current().tick < replay_ticks) {
    clock.advance(1.0 / render_rate, [&] {
      if (sim.current().tick >= replay_ticks) return;
      const auto input = recorded_input(scenario, sim.current().tick + 1);
      sim.step(input);
      const auto& current = sim.current();
      const auto& previous = sim.previous();
      const auto x = current.player.parts[0].position.GetX();
      const auto feet_y = current.player.capsule.position.GetY() - .89f;
      const bool step = current.player.grounded && x > -5.7f && x < -4.3f && feet_y > .22f;
      if (dump && (current.tick % 30 == 0 || input.room_push != 0 || input.rescue || previous.dummy.mode != current.dummy.mode ||
                   previous.player.mode != current.player.mode || previous.player.stepping != current.player.stepping ||
                   previous.player.on_platform != current.player.on_platform || previous.player.recovery_blocked != current.player.recovery_blocked)) {
        const auto pelvis = current.player.parts[0].position;
        const auto platform = current.room[size_t(fixture::platform)].pose.position;
        std::cout << current.tick << " player " << mode_name(current.player.mode) << " pelvis " << pelvis
                  << " dummy " << mode_name(current.dummy.mode) << " pelvis " << current.dummy.parts[0].position
                  << " platform " << platform.GetX() << " support " << current.player.on_platform
                  << " blocked " << current.player.recovery_blocked << " stepping " << current.player.stepping
                  << " push " << push_speed(input.room_push) << " m/s"
                  << " rescues " << current.player.rescues << '\n';
      }
      require(current.player.entity == player_id && current.dummy.entity == dummy_id, "entity ID changed at handoff");
      result.climbed_step |= step;
      result.rode_platform |= current.player.on_platform;
      result.staggered |= current.player.mode == movement_mode::staggering;
      result.fell |= current.player.mode == movement_mode::falling;
      result.stood_up |= current.player.mode == movement_mode::walking && current.player.recoveries != 0;
      result.dummy_stood_up |= current.dummy.mode == movement_mode::walking && current.dummy.recoveries != 0;
      const auto chest = size_t(part::chest);
      result.player_toppled |= current.player.mode == movement_mode::falling &&
        current.player.parts[chest].rotation.RotateAxisY().GetY() < .7f;
      result.dummy_toppled |= current.dummy.mode == movement_mode::falling &&
        current.dummy.parts[chest].rotation.RotateAxisY().GetY() < .7f;
      if (current.player.recovery_blocked) ++result.blocked_ticks;
      if (current.tick < 90) result.walking_speed = std::max(result.walking_speed, current.player.motion_velocity.Length());
      if (current.player.mode == movement_mode::staggering && current.brake_tick + 15 == current.tick)
        result.stagger_speed = current.player.motion_velocity.Length();
      if (current.brake_tick == current.tick) {
        result.crate_speed_before_brake = previous.crate_velocity.GetX();
        result.crate_speed_after_brake = current.crate_velocity.GetX();
        if (scenario == replay_case::strong) {
          require(current.player.handoffs == 1 && current.player.capsule_bodies == 0 && current.player.ragdoll_bodies == part_count,
                  "strong brake did not replace capsule with all ragdoll bodies");
          require((current.player.inherited_velocity - Vec3(4.2f, 0, 0)).LengthSq() < .0001f,
                  "unexpected inherited support velocity on strong brake");
          require((current.player.pelvis_launch_velocity - current.player.inherited_velocity).LengthSq() < .0001f,
                  "platform speed was lost or added twice");
        }
      }
      if (previous.player.on_platform && current.player.on_platform && current.platform_speed != 0 && previous.platform_speed != 0) {
        const auto platform = size_t(fixture::platform);
        const auto player_dx = x - previous.player.parts[0].position.GetX();
        const auto platform_dx = current.room[platform].pose.position.GetX() - previous.room[platform].pose.position.GetX();
        if (scenario == replay_case::strong)
          result.riding_relative_x += std::abs(float(player_dx - platform_dx));
      }
      if (current.player.mode == movement_mode::falling) {
        const auto joint = sim.measure_joints();
        result.anchor_error = std::max(result.anchor_error, joint.anchor_distance);
        result.angle_error = std::max(result.angle_error, joint.angle);
      }
      if (scenario == replay_case::weak) {
        require(current.player.handoffs == 0 && current.player.ragdoll_bodies == 0 && current.player.capsule_bodies == 1,
                "weak brake lost capsule control");
      }
      result.trace.push_back(fingerprint(result.trace.empty() ? 0 : result.trace.back(), current));
    });
    const auto stabilized = sim.sample_camera(clock.alpha(), camera_mode::level);
    const auto head = sim.sample_camera(clock.alpha(), camera_mode::head);
    require(std::abs(stabilized.rotation.RotateAxisZ().GetY()) < .0001f, "stabilized camera rolled");
    if (sim.current().player.mode == movement_mode::falling) {
      result.head_comparison |= std::abs(head.rotation.Dot(stabilized.rotation)) < .96f;
      require(std::abs(sim.current().player.pitch) <= 1.0f && std::abs(sim.current().player.yaw) <= 1.28f,
              "falling camera ignored look limits");
    }
  }
  return result;
}

struct response {
  Vec3 translation;
  float angular_speed;
};

response impulse_response(const part target, const bool continuous) {
  simulation sim;
  sim.step(intent{.drop = true});
  const auto start = sim.current().player.parts[0].position;
  float peak = 0;
  for (int i = 0; i < 14; ++i) {
    if (continuous && i < 12) sim.apply_force(target, Vec3(0, 0, 350));
    if (!continuous && i == 0) sim.apply_impulse(target, Vec3(0, 0, 70));
    sim.step({});
    peak = std::max(peak, sim.angular_velocity(part::chest).Length());
  }
  return {Vec3(sim.current().player.parts[0].position - start), peak};
}

joint_error verify_drop_from_step() {
  simulation sim;
  for (int tick = 0; tick < 45; ++tick) sim.step(intent{.forward = 1});
  for (int tick = 0; tick < 15; ++tick) sim.step({});
  for (int tick = 0; tick < 24; ++tick) sim.step(intent{.right = 1});
  require(sim.current().player.grounded && sim.current().player.parts[0].position.GetY() > 1.09f,
          "free fall fixture did not start on the step");
  sim.step(intent{.drop = true});
  joint_error worst;
  bool rolled_off = false;
  for (int tick = 0; tick < 180; ++tick) {
    sim.step({});
    if (sim.current().player.mode != movement_mode::falling) break;
    const auto error = sim.measure_joints();
    worst.anchor_distance = std::max(worst.anchor_distance, error.anchor_distance);
    worst.angle = std::max(worst.angle, error.angle);
    const auto pelvis = sim.current().player.parts[0].position;
    rolled_off |= pelvis.GetZ() > 1.10f && pelvis.GetY() < .40f;
  }
  require(rolled_off && sim.current().player.mode == movement_mode::recovering, "free ragdoll did not roll off the step and settle");
  return worst;
}

void verify_wall_camera() {
  simulation sim;
  // Approach the west wall, then fall with the head moving toward it.
  sim.step(intent{.forward = 1, .yaw_delta = JPH::JPH_PI});
  for (int tick = 1; tick < 94; ++tick) sim.step(intent{.forward = 1});
  sim.step(intent{.drop = true});
  bool wall_sweep = false;
  for (int tick = 0; tick < 45 && sim.current().player.mode == movement_mode::falling; ++tick) {
    const auto eye = sim.sample_camera(1, camera_mode::level);
    if (eye.clipped && sim.current().player.eye.position.GetX() < -11.4f) {
      require(eye.position.GetX() >= -11.85f + .083f, "camera sweep allowed the lens through the west wall");
      wall_sweep = true;
    }
    sim.step({});
  }
  require(wall_sweep, "no sphere sweep hit while falling toward the west wall");
}

void walk_to(simulation& sim, const float x, const float z) {
  for (int tick = 0; tick < 200; ++tick) {
    const auto p = sim.current().player.capsule.position;
    const auto horizontal_speed = sim.current().player.motion_velocity * Vec3(1, 0, 1);
    if (std::abs(x - p.GetX()) < .005f && std::abs(z - p.GetZ()) < .005f &&
        horizontal_speed.LengthSq() < .05f * .05f) break;
    sim.step({.forward = std::clamp(3.f * float(x - p.GetX()), -1.f, 1.f),
              .right = std::clamp(3.f * float(z - p.GetZ()), -1.f, 1.f)});
  }
  for (int tick = 0; tick < 16; ++tick) sim.step({});
  const auto p = sim.current().player.capsule.position;
  if (std::abs(x - p.GetX()) > .035f || std::abs(z - p.GetZ()) > .035f)
    utils::error{}("PHY03 walking probe: intended ({}, {}), reached ({}, {})", x, z, p.GetX(), p.GetZ());
}

void verify_step_approach(const float start_x, const float start_z, const intent& approach) {
  simulation sim;
  walk_to(sim, -7, start_z);
  walk_to(sim, start_x, start_z);
  const auto start_height = sim.current().player.capsule.position.GetY();
  float largest_rise = 0;
  uint32_t rising_ticks = 0;
  for (int tick = 0; tick < 95 && sim.current().player.completed_steps == 0; ++tick) {
    sim.step(approach);
    const auto rise = sim.current().player.capsule.position.GetY() - sim.previous().player.capsule.position.GetY();
    if (rise > .005f) ++rising_ticks;
    largest_rise = std::max(largest_rise, float(rise));
  }
  const auto& result = sim.current().player;
  const auto rise = result.capsule.position.GetY() - start_height;
  if (result.completed_steps != 1 || !result.grounded || rise < .27f || rise > .30f ||
      largest_rise > .065f || rising_ticks < 6)
    utils::error{}("PHY03 step from ({}, {}): {} completed, grounded {}, total rise {} m, largest tick {} m over {} rising ticks",
                   start_x, start_z, result.completed_steps, result.grounded, rise, largest_rise, rising_ticks);
}

void verify_crate_refusal(const float start_x, const float start_z, const intent& approach) {
  simulation sim;
  walk_to(sim, -3.24f, 0);
  walk_to(sim, start_x, start_z);
  const auto deck_height = sim.current().player.capsule.position.GetY();
  const auto steps = sim.current().player.completed_steps;
  require(sim.current().player.on_platform, "crate approach did not start on the deck");
  for (int tick = 0; tick < 65; ++tick) {
    sim.step(approach);
    require(sim.current().player.capsule.position.GetY() - deck_height < .04f && sim.current().player.completed_steps == steps,
            "the capsule climbed the 48 cm crate");
  }
}

void verify_upright_recovery() {
  simulation sim;
  for (int tick = 0; tick < 245; ++tick) sim.step({.forward = -1, .right = -1});
  for (int tick = 0; tick < 20; ++tick) sim.step({});
  require(sim.current().player.grounded && sim.current().player.capsule.position.GetX() < -11.4f &&
          sim.current().player.capsule.position.GetZ() < -6.4f, "upright recovery fixture did not reach the corner");

  sim.step({.drop = true});
  const auto fall_tick = sim.current().tick;
  for (int age = 1; age <= 36 && sim.current().player.mode == movement_mode::falling; ++age) {
    if (age == 12 || age == 20) sim.apply_impulse(part::pelvis, Vec3(.55f, 0, .30f));
    sim.step({});
    require(sim.current().player.last_impact_tick == fall_tick, "a small impulse restarted recovery delay");
    if (age < 24) require(sim.current().player.mode == movement_mode::falling, "upright ragdoll recovered during the impact delay");
  }
  require(sim.current().player.mode == movement_mode::recovering &&
          sim.current().player.parts[size_t(part::chest)].rotation.RotateAxisY().GetY() > .88f,
          "upright ragdoll did not recover from the corner");
  for (int tick = 0; tick < 12; ++tick) sim.step({});
  require(sim.current().player.mode == movement_mode::walking && sim.current().player.grounded,
          "upright recovery did not return control after the short blend");
}

void verify_airborne_recovery_delay() {
  simulation sim;
  sim.step({.drop = true});
  const auto fall_tick = sim.current().tick;
  bool lifted = false;
  for (int age = 1; age <= 240; ++age) {
    if (age == 20) sim.apply_impulse(part::pelvis, Vec3(0, 260, 0));
    sim.step({});
    if (age == 20) require(sim.current().player.last_impact_tick > fall_tick, "strong impulse did not renew recovery delay");
    if (age >= 20 && age <= 43)
      require(sim.current().player.mode == movement_mode::falling, "capsule returned during the renewed impact delay");
    lifted |= sim.current().player.parts[size_t(part::pelvis)].position.GetY() > 1.25f;
    if (sim.current().player.mode == movement_mode::recovering) {
      require(lifted && sim.current().player.grounded, "airborne fixture did not land before restoring control");
      return;
    }
  }
  require(false, "airborne ragdoll never returned control");
}

void verify_room_push_thresholds() {
  struct sample {
    uint32_t power;
    movement_mode expected;
  };
  for (const auto [power, expected] : {
         sample{stagger_threshold - 1, movement_mode::walking},
         sample{stagger_threshold, movement_mode::staggering},
         sample{weak_push, movement_mode::staggering},
         sample{fall_threshold - 1, movement_mode::staggering},
         sample{fall_threshold, movement_mode::falling},
         sample{strong_push, movement_mode::falling}}) {
    simulation sim;
    for (int tick = 0; tick < 30; ++tick) sim.step({});
    sim.step({.room_push = power});
    for (const auto* actor : {&sim.current().player, &sim.current().dummy}) {
      if (actor->mode != expected)
        utils::error{}("PHY03 room push {} m/s: entity {} entered '{}', expected '{}'",
                       push_speed(power), actor->entity, mode_name(actor->mode), mode_name(expected));
      if (expected == movement_mode::falling) {
        require(actor->capsule_bodies == 0 && actor->ragdoll_bodies == part_count && actor->handoffs == 1,
                "room push did not swap the capsule for the full ragdoll");
        require((actor->pelvis_launch_velocity - Vec3(push_speed(power), 0, 0)).LengthSq() < .0001f,
                "room push was added more than once at handoff");
      } else {
        require(actor->capsule_bodies == 1 && actor->ragdoll_bodies == 0,
                "sub-threshold push lost standing control");
      }
    }
  }

  simulation unpushed;
  simulation pushed;
  for (int tick = 0; tick < 16; ++tick) {
    const intent walk{.right = -1};
    unpushed.step(walk);
    pushed.step(tick == 0 ? intent{.right = -1, .room_push = weak_push} : walk);
  }
  require(unpushed.current().player.motion_velocity.GetZ() < -2.9f &&
          std::abs(pushed.current().player.motion_velocity.GetZ()) <= 1.21f &&
          pushed.current().player.mode == movement_mode::staggering,
          "weak room push did not reduce steering authority");
  require(std::abs(unpushed.current().dummy.parts[0].position.GetZ() - 2.1f) < .005f &&
          std::abs(pushed.current().dummy.parts[0].position.GetZ() - 2.1f) < .005f,
          "dummy followed the player's movement input");
}

void verify_push_on_active_ragdolls() {
  simulation control;
  simulation pushed;
  control.step({.room_push = strong_push});
  pushed.step({.room_push = strong_push});
  for (int tick = 0; tick < 12; ++tick) {
    control.step({});
    pushed.step({.room_push = tick == 5 ? weak_push : 0});
  }
  const auto check_displacement = [](const actor_snapshot& reference, const actor_snapshot& nudged) {
    require(nudged.mode == movement_mode::falling && nudged.handoffs == 1,
            "weak push replaced an already active ragdoll");
    require(nudged.parts[0].position.GetX() - reference.parts[0].position.GetX() > .10f,
            "weak push did not displace the already active ragdoll");
  };
  check_displacement(control.current().player, pushed.current().player);
  check_displacement(control.current().dummy, pushed.current().dummy);
  for (int tick = 0; tick < 300; ++tick) pushed.step({});
  require(pushed.current().player.mode == movement_mode::walking && pushed.current().player.recoveries == 1 &&
          pushed.current().dummy.mode == movement_mode::walking && pushed.current().dummy.recoveries == 1,
          "both room ragdolls did not regain their standing controllers");
}

void verify_stacked_recovery() {
  simulation sim;
  bool stacked = false;
  while (sim.current().tick < replay_ticks) {
    sim.step(recorded_input(replay_case::stack, sim.current().tick + 1));
    const auto& state = sim.current();
    const auto& previous = sim.previous();
    if (state.player.mode == movement_mode::falling && state.dummy.mode == movement_mode::falling) {
      const auto delta = Vec3(state.player.parts[0].position - state.dummy.parts[0].position);
      stacked |= delta.GetY() > .23f && (delta * Vec3(1, 0, 1)).LengthSq() < .42f * .42f;
    }
    for (const auto& [before, after] : {std::pair{&previous.player, &state.player},
                                      std::pair{&previous.dummy, &state.dummy}}) {
      if (before->mode != movement_mode::falling || after->mode != movement_mode::recovering) continue;
      const auto shift = Vec3(after->capsule.position - before->parts[0].position) * Vec3(1, 0, 1);
      require(shift.LengthSq() <= 1.13f * 1.13f && after->grounded,
              "stacked recovery did not land on nearby ground");
    }
  }
  require(stacked, "stack fixture did not put the player ragdoll on the dummy");
  for (const auto* actor : {&sim.current().player, &sim.current().dummy})
    require(actor->mode == movement_mode::walking && actor->recoveries == 1 && actor->rescues == 0,
            "stacked ragdolls did not both stand without rescue");

  // A slightly different approach sends a limb onto the deck and the pelvis over the floor.
  for (const int approach_ticks : {67, 69}) {
    simulation beside_platform;
    for (int tick = 0; tick < 40; ++tick) beside_platform.step({.right = 1});
    for (int tick = 0; tick < 16; ++tick) beside_platform.step({});
    for (int tick = 0; tick < approach_ticks; ++tick) beside_platform.step({.forward = 1});
    beside_platform.step({.room_push = strong_push});
    for (int tick = 0; tick < 360; ++tick) beside_platform.step({});
    const auto& player = beside_platform.current().player;
    require(player.mode == movement_mode::walking && player.recoveries == 1 && player.grounded &&
            std::abs(player.capsule.position.GetY() - .89f) < .04f,
            "ragdoll hung on the deck instead of recovering onto the floor beside it");
  }
}

void verify_debug_rescue() {
  simulation sim;
  simulation baseline;
  while (sim.current().tick < 350) {
    const auto next = sim.current().tick + 1;
    sim.step(recorded_input(replay_case::rescue, next));
    baseline.step(recorded_input(replay_case::low, next));
  }
  const auto& result = sim.current();
  const auto& before = sim.previous().player;
  require(before.recovery_blocked && before.mode == movement_mode::falling, "rescue fixture did not get stuck under the roof");
  require(result.player.rescues == 1 && result.player.mode == movement_mode::walking && result.player.grounded &&
          result.player.entity == before.entity && result.player.capsule_bodies == 1 && result.player.ragdoll_bodies == 0,
          "rescue did not return the same player's standing capsule");
  const auto& roof = result.room[size_t(fixture::roof)];
  const auto relative = result.player.capsule.position - roof.pose.position;
  require(std::abs(relative.GetX()) > roof.shape.size.GetX() + .29f ||
          std::abs(relative.GetZ()) > roof.shape.size.GetZ() + .29f, "rescue left the capsule under the roof");
  const auto target = result.player.capsule.position;
  const auto travel = Vec3(target - before.parts[size_t(part::pelvis)].position);
  require((travel * Vec3(1, 0, 1)).LengthSq() < 1.0f && std::abs(target.GetY() - .89f) < .04f,
          "rescue missed the free floor beside the roof");
  require((sim.sample_camera(0, camera_mode::level).position - result.player.eye.position).LengthSq() < .0001f,
          "camera interpolated across the debug relocation");
  require(result.dummy.entity == baseline.current().dummy.entity &&
          result.dummy.parts[0].position == baseline.current().dummy.parts[0].position,
          "rescue moved the dummy");
  for (size_t i = 0; i < fixture_count; ++i)
    require(result.room[i].pose.position == baseline.current().room[i].pose.position,
            "rescue reset a room fixture");

  for (int tick = 0; tick < 12; ++tick) sim.step({.forward = -1, .rescue = true});
  require(sim.current().player.rescues == 1 && sim.current().player.capsule.position.GetX() < target.GetX() - .10f,
          "movement was not restored or rescue ran again while walking");

  simulation airborne;
  airborne.step({.drop = true});
  airborne.apply_impulse(part::pelvis, Vec3(0, 260, 100));
  for (int tick = 0; tick < 16; ++tick) airborne.step({});
  const auto pelvis = airborne.current().player.parts[size_t(part::pelvis)].position;
  require(pelvis.GetY() > 1.25f && pelvis.GetZ() > .12f, "fallback fixture did not move away from spawn in the air");
  airborne.step({.rescue = true});
  const auto& fallback = airborne.current().player;
  require(fallback.rescues == 1 && fallback.grounded &&
          (fallback.capsule.position - JPH::RVec3(-7, .89f, 0)).LengthSq() < .003f,
          "rescue without a local support did not return the player near spawn");
}

float capsule_distance(const snapshot& state) {
  return (Vec3(state.dummy.capsule.position - state.player.capsule.position) * Vec3(1, 0, 1)).Length();
}

void verify_soft_pass() {
  simulation sim;
  walk_to(sim, -7, 2.5f);
  const auto dummy_start = sim.current().dummy.capsule.position;
  float min_spacing = 1;
  float passing_speed = 3;
  for (int tick = 0; tick < 95; ++tick) {
    sim.step({.forward = 1});
    const auto distance = capsule_distance(sim.current());
    min_spacing = std::min(min_spacing, distance);
    if (distance >= .60f) continue;
    const auto travelled = sim.current().player.capsule.position - sim.previous().player.capsule.position;
    passing_speed = std::min(passing_speed, float(travelled.GetX() / tick_seconds));
  }
  require(min_spacing > .37f && min_spacing < .44f && passing_speed >= 2.95f &&
          sim.current().player.capsule.position.GetX() > dummy_start.GetX() + .80f &&
          sim.current().dummy.capsule.position == dummy_start, "grazing past the dummy interrupted walking speed");

  simulation head_on;
  walk_to(head_on, -7, 2.1f);
  float closest = 1;
  float speed_at_core = 3;
  for (int tick = 0; tick < 95; ++tick) {
    head_on.step({.forward = 1});
    const auto& contact = head_on.current();
    const auto distance = capsule_distance(contact);
    closest = std::min(closest, distance);
    if (distance < .30f) speed_at_core = std::min(speed_at_core, contact.player.motion_velocity.GetX());
  }
  if (closest < .277f || closest > .30f || speed_at_core > .6f ||
      std::abs(head_on.current().dummy.capsule.position.GetX() + 3.8f) >= .01f)
    utils::error{}("PHY03 head-on approach: minimum spacing {} m, approach speed {} m/s near the core",
                   closest, speed_at_core);
}

void verify_rest_separation() {
  simulation sim;
  while (sim.current().tick < 143)
    sim.step(recorded_input(replay_case::overlap, sim.current().tick + 1));
  const auto start = sim.current();
  const auto initial_distance = capsule_distance(start);
  require(initial_distance > .33f && initial_distance < .47f, "separation fixture did not stop in a capsule overlap");
  float largest_correction = 0;
  int separating_ticks = 0;
  for (int tick = 0; tick < 65; ++tick) {
    sim.step({});
    const auto correction = capsule_distance(sim.current()) - capsule_distance(sim.previous());
    largest_correction = std::max(largest_correction, correction);
    separating_ticks += correction > .001f;
  }
  const auto& separated = sim.current();
  const auto player_shift = Vec3(separated.player.capsule.position - start.player.capsule.position);
  const auto dummy_shift = Vec3(separated.dummy.capsule.position - start.dummy.capsule.position);
  if (capsule_distance(separated) < .599f || largest_correction > .014f || separating_ticks < 10 ||
      player_shift.Length() < .05f || dummy_shift.Length() < .05f || player_shift.Dot(dummy_shift) >= 0)
    utils::error{}("PHY03 stopped overlap: spacing {} -> {} m, worst correction {} m over {} ticks",
                   initial_distance, capsule_distance(separated), largest_correction, separating_ticks);
}

void verify_separation_at_wall() {
  simulation sim;
  // Repeated weak room pushes move the dummy to the east wall in its own lane.
  for (int pulse = 0; pulse < 14; ++pulse) {
    sim.step({.room_push = fall_threshold - 1});
    for (int tick = 0; tick < 61; ++tick) sim.step({});
  }
  const auto wall_position = sim.current().dummy.capsule.position;
  require(wall_position.GetX() > 11.49f && sim.current().dummy.mode == movement_mode::walking,
          "wall separation fixture did not reach the wall");
  const auto player_x = float(sim.current().player.capsule.position.GetX());
  walk_to(sim, player_x, 3.3f);
  walk_to(sim, std::midpoint(player_x, 10.35f), 3.3f);
  walk_to(sim, 10.35f, 3.3f);
  walk_to(sim, 10.35f, float(wall_position.GetZ()));
  for (int tick = 0; tick < 60 && capsule_distance(sim.current()) > .32f; ++tick)
    sim.step({.forward = 1});
  require(capsule_distance(sim.current()) < .33f, "wall separation fixture did not enter the overlap");
  const auto initial_x = sim.current().player.capsule.position.GetX();
  for (int tick = 0; tick < 90; ++tick) sim.step({});
  const auto& result = sim.current();
  require(capsule_distance(result) >= .599f && result.player.capsule.position.GetX() < initial_x - .18f &&
          result.dummy.capsule.position.GetX() <= 11.552f, "wall separation did not move the free capsule away");
}

uint64_t fold_float(uint64_t hash, const float value) {
  return (hash ^ std::bit_cast<uint32_t>(value)) * 1099511628211ULL;
}

uint64_t fold_pose(uint64_t hash, const transform& pose) {
  for (int i = 0; i < 3; ++i) hash = fold_float(hash, float(pose.position[i]));
  for (int i = 0; i < 4; ++i) hash = fold_float(hash, pose.rotation.GetXYZW()[i]);
  return hash;
}

}

replay_case replay_from_name(const std::string_view name) {
  if (name == "strong") return replay_case::strong;
  if (name == "weak") return replay_case::weak;
  if (name == "low") return replay_case::low;
  if (name == "push") return replay_case::push;
  if (name == "rescue") return replay_case::rescue;
  if (name == "overlap") return replay_case::overlap;
  if (name == "stack") return replay_case::stack;
  utils::error{}("PHY03 replay: unknown recording '{}'", name);
}

intent recorded_input(const replay_case scenario, const uint32_t tick) {
  intent frame;
  if (scenario == replay_case::push) {
    if (tick == 32 || tick == 98) frame.room_push = weak_push;
    if (tick == 90) frame.room_push = strong_push;
  } else if (scenario == replay_case::overlap) {
    if (tick <= 49) frame.right = 1;
    if (tick >= 66 && tick <= 128) frame.forward = 1;
  } else if (scenario == replay_case::stack) {
    if (tick <= 43) frame.right = 1;
    if (tick >= 60 && tick <= 119) frame.forward = 1;
    if (tick == 120) frame.room_push = strong_push;
  } else if (scenario == replay_case::low || scenario == replay_case::rescue) {
    if (tick <= 53) frame.right = -1;
    if (tick >= 66 && tick <= 229) frame.forward = 1;
    frame.drop = tick == 229;
    frame.rescue = scenario == replay_case::rescue && tick == 350;
  } else {
    if (tick <= 85) frame.forward = 1;
    if (tick == 115) frame.brake = scenario == replay_case::weak ? brake_test::weak : brake_test::strong;
    if (scenario == replay_case::weak && tick >= 171 && tick < 194) frame.forward = -1;
  }
  if (tick >= 182 && tick <= 216 && scenario == replay_case::strong) {
    frame.yaw_delta = .067f;
    frame.pitch_delta = -.048f;
  }
  return frame;
}

float fixed_clock::alpha() const {
  return std::clamp(float(credit), 0.0f, 1.0f);
}

uint64_t fingerprint(const uint64_t previous, const snapshot& sample) {
  auto hash = previous == 0 ? UINT64_C(14695981039346656037) : previous;
  hash = (hash ^ sample.tick) * 1099511628211ULL;
  for (const auto* actor : {&sample.player, &sample.dummy}) {
    for (const auto value : {uint32_t(actor->mode), actor->entity, actor->handoffs, actor->recoveries,
                             uint32_t(actor->recovery_blocked), actor->last_impact_tick,
                             actor->completed_steps, uint32_t(actor->stepping)})
      hash = (hash ^ value) * 1099511628211ULL;
    for (const auto& pose : actor->parts) hash = fold_pose(hash, pose);
    hash = fold_pose(hash, actor->eye);
    hash = fold_float(fold_float(hash, actor->yaw), actor->pitch);
    if (actor->rescues != 0) hash = (hash ^ actor->rescues) * 1099511628211ULL;
  }
  for (const auto& item : sample.room) hash = fold_pose(hash, item.pose);
  return hash;
}

uint64_t run_replay(const replay_case scenario, const uint32_t render_rate, const bool dump) {
  return observe(scenario, render_rate, dump).trace.back();
}

void verify() {
  const auto strong = observe(replay_case::strong, tick_rate, false);
  const auto low = observe(replay_case::low, tick_rate, false);
  const auto weak = observe(replay_case::weak, tick_rate, false);
  const auto push = observe(replay_case::push, tick_rate, false);
  const auto rescue = observe(replay_case::rescue, tick_rate, false);
  const auto overlap = observe(replay_case::overlap, tick_rate, false);
  const auto stack = observe(replay_case::stack, tick_rate, false);

  require(strong.climbed_step && strong.rode_platform, "the controller did not walk over the step onto the platform");
  require(strong.walking_speed > 2.9f && strong.riding_relative_x < .15f, "walking speed or platform tracking");
  require(strong.fell && strong.stood_up, "strong brake did not complete ragdoll and recovery");
  require(strong.crate_speed_before_brake > 3.0f && strong.crate_speed_after_brake > 2.4f,
          "crate did not retain momentum when the deck stopped");
  require(weak.staggered && !weak.fell && weak.stagger_speed < 1.2f, "weak brake and counter-input response");
  require(low.blocked_ticks > 100 && !low.stood_up, "standing capsule was not refused under the low roof");
  require(strong.head_comparison, "raw head orientation did not differ from the stabilized camera");
  require(push.staggered && push.player_toppled && push.dummy_toppled && push.stood_up && push.dummy_stood_up,
          "both actors must topple and stand again in the room push recording");
  verify_step_approach(-7, 0, {.forward = 1});
  verify_step_approach(-5, -2.10f, {.right = 1});
  for (const float degrees : {15.f, 30.f, 45.f, 60.f, 75.f}) {
    const auto radians = JPH::DegreesToRadians(degrees);
    const auto x = std::cos(radians);
    const auto z = std::sin(radians);
    verify_step_approach(-5.8f - 1.4f * x, -1.1f - 1.4f * z, {.forward = x, .right = z});
  }
  verify_crate_refusal(-3.2f, .91f, {.forward = 1});
  verify_crate_refusal(-3.1f, 0, {.forward = 1, .right = 1});
  verify_upright_recovery();
  verify_airborne_recovery_delay();
  verify_wall_camera();
  verify_room_push_thresholds();
  verify_push_on_active_ragdolls();
  verify_stacked_recovery();
  verify_debug_rescue();
  verify_soft_pass();
  verify_rest_separation();
  verify_separation_at_wall();
  const auto step = verify_drop_from_step();
  const auto anchor_error = std::max({strong.anchor_error, low.anchor_error, step.anchor_distance});
  const auto angle_error = std::max({strong.angle_error, low.angle_error, step.angle});
  require(anchor_error < .065f, "ragdoll joint anchors drifted over 65 mm");
  require(angle_error < .36f, "ragdoll exceeded angular limits by over 0.36 rad");

  for (const auto rate : {30u, 144u}) {
    require(observe(replay_case::strong, rate, false).trace == strong.trace, "strong brake differs with render cadence");
    require(observe(replay_case::weak, rate, false).trace == weak.trace, "weak brake differs with render cadence");
    require(observe(replay_case::low, rate, false).trace == low.trace, "low roof replay differs with render cadence");
    require(observe(replay_case::push, rate, false).trace == push.trace, "room push replay differs with render cadence");
    require(observe(replay_case::rescue, rate, false).trace == rescue.trace, "rescue replay differs with render cadence");
    require(observe(replay_case::overlap, rate, false).trace == overlap.trace, "overlap replay differs with render cadence");
    require(observe(replay_case::stack, rate, false).trace == stack.trace, "stacked recovery differs with render cadence");
  }
  const auto pelvis = impulse_response(part::pelvis, false);
  const auto shoulder = impulse_response(part::left_arm, false);
  const auto force = impulse_response(part::pelvis, true);
  require(pelvis.translation.GetZ() > .08f && shoulder.translation.GetZ() > .05f &&
          std::abs(pelvis.angular_speed - shoulder.angular_speed) > .1f, "pelvis and shoulder impulse response");
  require(force.translation.GetZ() > .04f && force.translation.GetZ() < pelvis.translation.GetZ(),
          "force accumulated over 12 ticks");

  std::cout << "PHY03 verified: smooth step from seven angles, crate refusal, upright and blocked recovery, "
               "room pushes at both thresholds, stacked ragdolls, debug rescue, capsule overlap, camera and 30/60/144 Hz replay\n";
  std::cout << "strong fingerprint " << strong.trace.back() << ", low " << low.trace.back() << ", weak " << weak.trace.back()
            << ", push " << push.trace.back() << ", rescue " << rescue.trace.back() << ", overlap " << overlap.trace.back()
            << ", stack " << stack.trace.back()
            << ", maximum joint errors " << anchor_error << " m / " << angle_error << " rad\n";
}
}
