#pragma once

#include "recipe.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace devils_engine::phy04 {

using phy03::geometry;
using phy03::part_count;
using phy03::solid;
using phy03::transform;
using JPH::RVec3;
using JPH::Vec3;

constexpr size_t fixture_count = 6;
constexpr uint32_t step_budget = 480;
constexpr float chunk_size = 16;
using chunk_key = std::array<int64_t, 3>;
constexpr chunk_key first_chunk{1'000'000, 0, -1'000'000};
constexpr chunk_key second_chunk{first_chunk[0] + 1, first_chunk[1], first_chunk[2]};
inline const Vec3 cabin_marker(-4.4f, .035f, -.9f);
inline const Vec3 chunk_marker(0, .04f, 0);

struct chunk_point {
  chunk_key key;
  Vec3 offset;
};

// The linear velocity belongs to pose.position. Angular velocity is expressed in the outer axes.
struct moving_frame {
  transform pose;
  Vec3 linear = Vec3::sZero();
  Vec3 angular = Vec3::sZero();
};

struct body_state {
  transform pose; // Center of mass.
  Vec3 linear = Vec3::sZero();
  Vec3 angular = Vec3::sZero();
};

RVec3 point_in_zone(const chunk_point& point, const chunk_key& origin);
chunk_point point_in_chunks(JPH::RVec3Arg in_zone, const chunk_key& origin);
body_state to_outer(const moving_frame& frame, const body_state& local);
body_state to_cabin(const moving_frame& frame, const body_state& outer);

enum class phase { approach, cabin, exit, outside, finished };

struct continuity {
  float position = 0;
  float rotation = 0; // Quaternion difference, accounting for equivalent signs.
  float linear = 0;
  float angular = 0;
};

struct snapshot {
  uint32_t tick = 0;
  phase stage = phase::approach;
  chunk_key origin = first_chunk;
  chunk_point pelvis_address{};
  uint32_t rebase_tick = 0;
  uint32_t enter_tick = 0;
  uint32_t kick_tick = 0;
  uint32_t leave_tick = 0;
  continuity rebase_error;
  continuity enter_error;
  continuity leave_error;
  std::array<solid, fixture_count> fixtures;
  std::array<transform, part_count> parts;
  moving_frame hull; // Published poses use the first chunk for a stable view across rebases.
  body_state pelvis_zone;
  body_state pelvis_cabin;
  float joint_gap = 0;
  aesthetics::entityid_t entity = aesthetics::invalid_entityid;
};

class experiment {
public:
  explicit experiment(bool turning = false, bool rebase = true);
  ~experiment();
  void step();
  const snapshot& current() const noexcept;
  const snapshot& previous() const noexcept;

private:
  struct world;
  std::unique_ptr<world> state;
};

std::string_view phase_name(phase value);
void print_sample(const snapshot& sample);
void print_points(const snapshot& sample);
void verify();

}
