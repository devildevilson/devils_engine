// TIME-02 acceptance: wall-time is only a fixed-step pacing source. Two deliberately different
// presentation schedules must execute the same authoritative ticks and produce the same complete
// tile_frontier actor checkpoint.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/timeline.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"
#include "core/actor_checkpoint.h"
#include "test_brain_fixture.h"

using namespace devils_engine;
namespace tf = tile_frontier::core;

namespace {

struct run_result {
  std::vector<std::byte> checkpoint;
  utils::timelines_causal_state clocks;
};

run_result run_schedule(const std::span<const uint64_t> frame_microseconds,
                        const tf::brain_config& brains) {
  constexpr utils::simulation_rate rate(60);
  constexpr uint64_t max_steps_per_frame = 8;

  utils::fixed_step_accumulator pacer(rate);
  utils::timelines clocks(rate);
  thread::atomic_pool pool(1);
  tf::actor_batch batch;
  batch.bind("v2ui1c4v1");
  if (!batch.valid()) {
    utils::error{}("TIME-02 actor batch layout is invalid");
  }

  tf::actor_world_slice actors;
  actors.init(128, {0.5f, 0.5f}, {64.0f, 64.0f}, 4, brains);

  const auto execute = [&](const uint64_t step_count) {
    for (uint64_t i = 0; i < step_count; ++i) {
      const auto tick = clocks.simulation_now() + utils::simulation_duration{1};
      if (tick == utils::simulation_tick{30}) {
        act::intent intent;
        intent.kind = act::intent_kind::spawn_prefab;
        intent.payload.spawn.prefab = utils::string_hash("food");
        intent.payload.spawn.target = act::vec3{12.0, 13.0, 0.0};
        intent.source_action = utils::string_hash("time_smoke_spawn");
        if (!actors.enqueue_player_intent(intent)) {
          utils::error{}("TIME-02 recorded intent was rejected");
        }
      }
      actors.update(tick, clocks.advance_simulation(tick), batch, pool);
      if (pool.tasks_count() != 0 || pool.working_count() != 0) {
        utils::error{}("TIME-02 update returned before its batch task completed");
      }
    }
  };

  for (const uint64_t elapsed : frame_microseconds) {
    pacer.add_elapsed({elapsed});
    execute(pacer.take_steps(max_steps_per_frame));
  }
  while (pacer.pending_steps() != 0) {
    execute(pacer.take_steps(max_steps_per_frame));
  }

  tf::actor_checkpoint_buffers checkpoint;
  if (!tf::write_actor_checkpoint(actors, clocks, checkpoint)) {
    utils::error{}("TIME-02 checkpoint write failed");
  }
  return {std::move(checkpoint.document), clocks.causal_state()};
}

std::vector<uint64_t> fragmented_schedule(const uint64_t total) {
  constexpr std::array<uint64_t, 5> pattern{1'000, 17'000, 33'333, 7'919, 41'000};
  std::vector<uint64_t> result;
  uint64_t remaining = total;
  size_t i = 0;
  while (remaining != 0) {
    const uint64_t part = std::min(remaining, pattern[i++ % pattern.size()]);
    result.push_back(part);
    remaining -= part;
  }
  return result;
}

} // namespace

int main() {
  spdlog::set_level(spdlog::level::err);
  test_brain_fixture fixture(TILE_FRONTIER_SOURCE_RESOURCE_ROOT);

  constexpr std::array<uint64_t, 1> coarse{2'000'000};
  const auto fragmented = fragmented_schedule(coarse.front());
  const run_result a = run_schedule(coarse, fixture.config());
  const run_result b = run_schedule(fragmented, fixture.config());

  const bool same_tick = a.clocks.simulation == utils::simulation_tick{120} &&
                         b.clocks.simulation == a.clocks.simulation;
  const bool same_game_time = a.clocks.game == utils::game_timestamp{2'000'000} &&
                              b.clocks.game == a.clocks.game;
  const bool same_remainder = a.clocks.game_remainder == b.clocks.game_remainder;
  const bool same_checkpoint = a.checkpoint == b.checkpoint;

  std::printf("TIME-02 coarse frames: %zu, fragmented frames: %zu\n",
              coarse.size(), fragmented.size());
  std::printf("fixed steps: %llu, game time: %llu us\n",
              static_cast<unsigned long long>(a.clocks.simulation.value),
              static_cast<unsigned long long>(a.clocks.game.ticks));
  std::printf("checkpoint bytes: %zu, pacing-independent: %s\n",
              a.checkpoint.size(), same_checkpoint ? "yes" : "NO");

  if (!same_tick || !same_game_time || !same_remainder || !same_checkpoint) {
    std::fprintf(stderr,
                 "TIME-02 FAILED: tick=%d game=%d remainder=%d checkpoint=%d\n",
                 int(same_tick), int(same_game_time), int(same_remainder), int(same_checkpoint));
    return 1;
  }
  std::printf("TIME-02 OK: wall-time partition cannot change the authoritative state\n");
  return 0;
}
