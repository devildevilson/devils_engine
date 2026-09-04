#include <cstdint>
#include <limits>

#include <devils_engine/utils/simulation_time.h>
#include <doctest/doctest.h>

namespace utils = devils_engine::utils;

static_assert(utils::simulation_rate(60).to_ticks_ceil({1'000'000}).ticks == 60);
static_assert(utils::simulation_rate(60).to_microseconds_floor({60}).microseconds ==
              1'000'000);

TEST_CASE("authored microseconds convert to causal ticks without early completion [time]") {
  const utils::simulation_rate hz60(60);

  CHECK(hz60.to_ticks_ceil({0}).ticks == 0);
  CHECK(hz60.to_ticks_ceil({1}).ticks == 1);
  CHECK(hz60.to_ticks_ceil({16'666}).ticks == 1);
  CHECK(hz60.to_ticks_ceil({16'667}).ticks == 2);
  CHECK(hz60.to_ticks_ceil({750'000}).ticks == 45);
  CHECK(hz60.to_ticks_ceil({1'000'000}).ticks == 60);
}

TEST_CASE("authored durations survive a project tick-rate choice [time]") {
  CHECK(utils::simulation_rate(30).to_ticks_ceil({1'000'000}).ticks == 30);
  CHECK(utils::simulation_rate(60).to_ticks_ceil({1'000'000}).ticks == 60);
  CHECK(utils::simulation_rate(120).to_ticks_ceil({1'000'000}).ticks == 120);

  // 750 ms cannot be represented exactly at 30 Hz, so it completes on tick 23
  // rather than early on tick 22. Rates which divide it retain the exact time.
  CHECK(utils::simulation_rate(30).to_ticks_ceil({750'000}).ticks == 23);
  CHECK(utils::simulation_rate(60).to_ticks_ceil({750'000}).ticks == 45);
  CHECK(utils::simulation_rate(120).to_ticks_ceil({750'000}).ticks == 90);
}

TEST_CASE("tick-derived time uses rational projection without per-step drift [time]") {
  const utils::simulation_rate hz60(60);
  CHECK(hz60.to_microseconds_floor({1}).microseconds == 16'666);
  CHECK(hz60.to_microseconds_floor({2}).microseconds == 33'333);
  CHECK(hz60.to_microseconds_floor({3}).microseconds == 50'000);
  CHECK(hz60.to_microseconds_floor({59}).microseconds == 983'333);
  CHECK(hz60.to_microseconds_floor({60}).microseconds == 1'000'000);
}

TEST_CASE("simulation time rejects invalid rates and arithmetic overflow [time]") {
  CHECK_THROWS(utils::simulation_rate(0));

  const utils::simulation_rate maximum_rate(std::numeric_limits<uint32_t>::max());
  CHECK_THROWS(
    maximum_rate.to_ticks_ceil({std::numeric_limits<uint64_t>::max()}));
  CHECK_THROWS(
    utils::simulation_tick{std::numeric_limits<uint64_t>::max()} +
      utils::simulation_duration{1});

  constexpr uint64_t max_whole_seconds =
    std::numeric_limits<uint64_t>::max() / utils::microseconds_per_second;
  CHECK_THROWS(
    utils::simulation_rate(3).to_microseconds_floor(
      {max_whole_seconds * 3 + 2}));
}

TEST_CASE("fixed-step pacing is independent of elapsed-time partition [time]") {
  utils::fixed_step_accumulator coarse(utils::simulation_rate(60));
  utils::fixed_step_accumulator fragmented(utils::simulation_rate(60));

  CHECK(coarse.advance({1'000'000}) == 60);
  uint64_t fragmented_steps = 0;
  for (const uint64_t elapsed : {7'000u, 13'000u, 1u, 29'999u, 450'000u, 500'000u}) {
    fragmented_steps += fragmented.advance({elapsed});
  }
  CHECK(fragmented_steps == 60);
  CHECK(fragmented.fractional_units() == coarse.fractional_units());

  utils::fixed_step_accumulator bounded(utils::simulation_rate(60));
  CHECK(bounded.advance({1'000'000}, 8) == 8);
  CHECK(bounded.pending_steps() == 52);
  CHECK(bounded.advance({0}, 8) == 8);
  CHECK(bounded.pending_steps() == 44);
}
