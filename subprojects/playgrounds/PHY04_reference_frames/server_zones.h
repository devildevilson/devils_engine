#pragma once

#include "frames.h"

#include <vector>

namespace devils_engine::phy04 {

enum class zone_id : size_t { left, right, common, count };
constexpr size_t zone_count = size_t(zone_id::count);
constexpr size_t zone_actor_count = 4;
constexpr uint32_t zone_step_budget = 570;
constexpr float player_radius = .30f;
constexpr float player_half_cylinder = .55f;
constexpr float cargo_radius = .45f;
constexpr int64_t right_start_chunk_x = first_chunk[0] + 8;

struct zone_actor_snapshot {
  aesthetics::entityid_t entity = aesthetics::invalid_entityid;
  zone_id owner = zone_id::left;
  bool player = false;
  chunk_point address;
  body_state motion; // The experiment's common display coordinates, relative to first_chunk.
  uint32_t steps = 0;
};

struct resident_floor {
  int64_t chunk_x;
  uint32_t owners;
};

struct server_snapshot {
  uint32_t tick = 0;
  uint32_t merged_tick = 0;
  uint32_t first_contact_tick = 0;
  uint32_t player_contact_tick = 0;
  uint32_t cargo_contact_tick = 0;
  float merge_gap = 0;
  continuity merge_error;
  std::array<bool, zone_count> active_zones{};
  std::array<uint32_t, zone_count> zone_steps{};
  std::array<chunk_key, zone_count> origins{};
  std::array<zone_actor_snapshot, zone_actor_count> actors;
  std::vector<resident_floor> floors;
};

// The sole writer of both separated worlds and the replacement common world.
class server_experiment {
public:
  server_experiment();
  ~server_experiment();
  void step();
  const server_snapshot& current() const noexcept;
  const server_snapshot& previous() const noexcept;

private:
  struct server;
  std::unique_ptr<server> state;
};

std::string_view zone_name(zone_id id);
void print_zones(const server_snapshot& snapshot);
void verify_zones();

}
