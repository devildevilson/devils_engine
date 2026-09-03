#include <type_traits>

#include <devils_engine/utils/timeline.h>
#include <doctest/doctest.h>

using namespace devils_engine::utils;

static_assert(!std::is_same_v<engine_timestamp, game_timestamp>);
static_assert(!std::is_same_v<engine_duration, game_duration>);
static_assert(!std::is_same_v<game_timestamp, calendar_timestamp>);

TEST_CASE("timelines keep engine running while game is paused [time]") {
  timelines clocks(simulation_rate(1));
  clocks.advance_frame(timeline_ticks_per_second);
  CHECK(clocks.advance_simulation({1}).ticks == timeline_ticks_per_second);
  CHECK(clocks.engine_now().ticks == timeline_ticks_per_second);
  CHECK(clocks.game_now().ticks == timeline_ticks_per_second);

  clocks.set_game_paused(true);
  clocks.advance_frame(3 * timeline_ticks_per_second);
  CHECK(clocks.advance_simulation({2}).ticks == 0);
  CHECK(clocks.advance_simulation({3}).ticks == 0);
  CHECK(clocks.advance_simulation({4}).ticks == 0);
  CHECK(clocks.engine_now().ticks == 4 * timeline_ticks_per_second);
  CHECK(clocks.game_now().ticks == timeline_ticks_per_second);

  clocks.set_game_paused(false);
  clocks.advance_frame(timeline_ticks_per_second);
  CHECK(clocks.advance_simulation({5}).ticks == timeline_ticks_per_second);
  CHECK(clocks.engine_now().ticks == 5 * timeline_ticks_per_second);
  CHECK(clocks.game_now().ticks == 2 * timeline_ticks_per_second);

  const game_deadline deadline{clocks.game_now() + game_duration::from_seconds(2)};
  CHECK_FALSE(deadline.elapsed(clocks.game_now()));
  clocks.advance_simulation({6});
  clocks.advance_simulation({7});
  CHECK(deadline.elapsed(clocks.game_now()));
}

TEST_CASE("game time scale maps nominal real durations without coupling absolute timestamps [time]") {
  const game_time_scale gta_like = game_time_scale::from_seconds(60, 1);
  CHECK(gta_like.to_game(engine_duration::from_seconds(10)).ticks == game_duration::from_minutes(10).ticks);
  CHECK(gta_like.to_engine(game_duration::from_hours(1)).ticks == engine_duration::from_minutes(1).ticks);

  timelines clocks(simulation_rate(1));
  clocks.set_game_scale(gta_like);
  clocks.advance_frame(engine_duration::from_seconds(2).ticks);
  clocks.advance_simulation({1});
  clocks.advance_simulation({2});
  CHECK(clocks.engine_now().ticks == engine_duration::from_seconds(2).ticks);
  CHECK(clocks.game_now().ticks == game_duration::from_minutes(2).ticks);
}

TEST_CASE("real time and turns are orthogonal gameplay coordinates [time][turn]") {
  timelines clocks;
  clocks.advance_turns({2});
  clocks.set_game_paused(true);
  clocks.advance_turns();
  clocks.advance_frame(engine_duration::from_seconds(1).ticks);
  CHECK(clocks.turn_now().value == 2);
  CHECK(clocks.game_now().ticks == 0);
  clocks.advance_frame(engine_duration::from_seconds(1).ticks);
  CHECK(clocks.engine_now().ticks == engine_duration::from_seconds(2).ticks);
}

TEST_CASE("calendar projects configurable days and time of day [time][calendar]") {
  // 10 hours/day; year = 3-day month + 2-day month.
  const calendar_policy calendar(10, {3, 2});
  const auto stamp = calendar.compose_calendar(1, 2, 1, 2, 3, 4, 123);
  const auto fields = calendar.project(stamp);

  CHECK(fields.has_calendar);
  CHECK(fields.absolute_day == 8); // year 1 starts at day 5, month 2 starts after three more days
  CHECK(fields.year == 1);
  CHECK(fields.month == 2);
  CHECK(fields.day == 1);
  CHECK(fields.hour == 2);
  CHECK(fields.minute == 3);
  CHECK(fields.second == 4);
  CHECK(fields.subsecond_ticks == 123);
}

TEST_CASE("calendar can expose only absolute day without months [time][calendar]") {
  const calendar_policy day_cycle(24);
  const auto stamp = day_cycle.compose(12, 6, 30);
  const auto fields = day_cycle.project(stamp);
  CHECK_FALSE(fields.has_calendar);
  CHECK(fields.absolute_day == 12);
  CHECK(fields.hour == 6);
  CHECK(fields.minute == 30);
  CHECK_THROWS(day_cycle.compose_calendar(0, 1, 1));
}

TEST_CASE("project calendar can be driven by scaled game time [time][calendar]") {
  calendar_policy policy(24, {30, 30});
  const calendar_clock calendar(
    calendar_source::game_time, policy, policy.compose_calendar(2, 1, 1));

  timelines clocks(simulation_rate(1));
  clocks.set_game_scale(game_time_scale::from_seconds(60, 1));
  for (uint64_t tick = 1; tick <= 24 * 60; ++tick) {
    clocks.advance_simulation({tick}); // 24 real minutes -> one 24-hour game day
  }
  clocks.advance_turns({10});                              // irrelevant to this calendar

  const auto date = calendar.date(clocks);
  CHECK(date.year == 2);
  CHECK(date.month == 1);
  CHECK(date.day == 2);
}

TEST_CASE("project calendar can be driven by turns with calendar-sized steps [time][calendar][turn]") {
  calendar_policy policy(24, {31, 28, 31});
  const calendar_clock calendar(
    calendar_source::turn, policy, policy.compose_calendar(4, 1, 31),
    calendar_step{.months = 1});

  timelines clocks;
  clocks.set_turn({1});
  auto date = calendar.date(clocks);
  CHECK(date.year == 4);
  CHECK(date.month == 2);
  CHECK(date.day == 28); // clamp Jan 31 -> shorter target month

  clocks.set_turn({2});
  date = calendar.date(clocks);
  CHECK(date.month == 3);
  CHECK(date.day == 31); // direct projection from the epoch, no accumulated clamp
}

TEST_CASE("game time is derived from fixed ticks without fractional drift [time]") {
  timelines clocks(simulation_rate(60));
  for (uint64_t tick = 1; tick <= 60; ++tick) {
    clocks.advance_simulation({tick});
  }
  CHECK(clocks.simulation_now() == simulation_tick{60});
  CHECK(clocks.game_now().ticks == timeline_ticks_per_second);
  CHECK_THROWS(clocks.advance_simulation({62}));
  CHECK_THROWS(clocks.set_simulation_rate(simulation_rate(30)));
}

TEST_CASE("causal timeline state preserves the fractional projection across checkpoint [time]") {
  timelines original(simulation_rate(60));
  original.set_game_scale(game_time_scale(7, 3));
  original.advance_simulation({1});
  original.advance_simulation({2});
  original.advance_turns({4});

  timelines restored(simulation_rate(1));
  restored.advance_frame(123'456); // non-causal clocks do not belong to the checkpoint
  REQUIRE(restored.restore_causal_state(original.causal_state()));
  CHECK(restored.engine_now().ticks == 123'456);
  CHECK(restored.simulation_now() == original.simulation_now());
  CHECK(restored.game_now() == original.game_now());
  CHECK(restored.turn_now() == original.turn_now());

  for (uint64_t tick = 3; tick <= 120; ++tick) {
    CHECK(restored.advance_simulation({tick}) == original.advance_simulation({tick}));
  }
  CHECK(restored.causal_state().game_remainder == original.causal_state().game_remainder);

  auto corrupt = original.causal_state();
  corrupt.game_remainder =
    uint64_t(corrupt.ticks_per_second) * corrupt.scale_engine_ticks;
  const auto before_rejected_restore = restored.causal_state();
  CHECK_FALSE(restored.restore_causal_state(corrupt));
  CHECK(restored.causal_state() == before_rejected_restore);
}

TEST_CASE("turn-driven calendar validates its one-time project policy [time][calendar][turn]") {
  CHECK(parse_calendar_source("game_time") == calendar_source::game_time);
  CHECK(parse_calendar_source("turn") == calendar_source::turn);
  CHECK_THROWS(parse_calendar_source("frames"));
  CHECK_THROWS(calendar_clock(calendar_source::turn));
  CHECK_THROWS(calendar_clock(calendar_source::turn, calendar_policy(24), {}, calendar_step{.months = 1}));
}
