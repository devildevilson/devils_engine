#include <type_traits>

#include <devils_engine/utils/timeline.h>
#include <doctest/doctest.h>

using namespace devils_engine::utils;

static_assert(!std::is_same_v<engine_timestamp, game_timestamp>);
static_assert(!std::is_same_v<engine_duration, game_duration>);
static_assert(!std::is_same_v<game_timestamp, calendar_timestamp>);

TEST_CASE("timelines keep engine running while game is paused [time]") {
  timelines clocks;
  clocks.advance(timeline_ticks_per_second);
  CHECK(clocks.engine_now().ticks == timeline_ticks_per_second);
  CHECK(clocks.presentation_now().ticks == timeline_ticks_per_second);
  CHECK(clocks.game_now().ticks == timeline_ticks_per_second);

  clocks.set_game_paused(true);
  clocks.advance(3 * timeline_ticks_per_second);
  CHECK(clocks.engine_now().ticks == 4 * timeline_ticks_per_second);
  CHECK(clocks.presentation_now().ticks == 4 * timeline_ticks_per_second);
  CHECK(clocks.game_now().ticks == timeline_ticks_per_second);

  clocks.set_game_paused(false);
  clocks.advance(timeline_ticks_per_second);
  CHECK(clocks.engine_now().ticks == 5 * timeline_ticks_per_second);
  CHECK(clocks.game_now().ticks == 2 * timeline_ticks_per_second);

  const game_deadline deadline{clocks.game_now() + game_duration::from_seconds(2)};
  CHECK_FALSE(deadline.elapsed(clocks.game_now()));
  clocks.advance(2 * timeline_ticks_per_second);
  CHECK(deadline.elapsed(clocks.game_now()));
}

TEST_CASE("game time scale maps nominal real durations without coupling absolute timestamps [time]") {
  const game_time_scale gta_like = game_time_scale::from_seconds(60, 1);
  CHECK(gta_like.to_game(engine_duration::from_seconds(10)).ticks == game_duration::from_minutes(10).ticks);
  CHECK(gta_like.to_engine(game_duration::from_hours(1)).ticks == engine_duration::from_minutes(1).ticks);

  timelines clocks;
  clocks.set_game_scale(gta_like);
  clocks.advance(engine_duration::from_seconds(2).ticks);
  CHECK(clocks.engine_now().ticks == engine_duration::from_seconds(2).ticks);
  CHECK(clocks.presentation_now().ticks == presentation_duration::from_seconds(2).ticks);
  CHECK(clocks.game_now().ticks == game_duration::from_minutes(2).ticks);
}

TEST_CASE("presentation and turns are orthogonal gameplay coordinates [time][turn]") {
  timelines clocks;
  clocks.advance_turns({2});
  clocks.set_game_paused(true);
  clocks.advance_turns();
  clocks.advance(engine_duration::from_seconds(1).ticks);
  CHECK(clocks.turn_now().value == 2);
  CHECK(clocks.presentation_now().ticks == presentation_duration::from_seconds(1).ticks);
  CHECK(clocks.game_now().ticks == 0);

  clocks.set_presentation_paused(true);
  clocks.advance(engine_duration::from_seconds(1).ticks);
  CHECK(clocks.engine_now().ticks == engine_duration::from_seconds(2).ticks);
  CHECK(clocks.presentation_now().ticks == presentation_duration::from_seconds(1).ticks);
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

  timelines clocks;
  clocks.set_game_scale(game_time_scale::from_seconds(60, 1));
  clocks.advance(engine_duration::from_minutes(24).ticks); // one 24-hour game day
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
  clocks.advance(game_duration::from_hours(12).ticks); // irrelevant to a turn-driven calendar
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

TEST_CASE("turn-driven calendar validates its one-time project policy [time][calendar][turn]") {
  CHECK(parse_calendar_source("game_time") == calendar_source::game_time);
  CHECK(parse_calendar_source("turn") == calendar_source::turn);
  CHECK_THROWS(parse_calendar_source("frames"));
  CHECK_THROWS(calendar_clock(calendar_source::turn));
  CHECK_THROWS(calendar_clock(calendar_source::turn, calendar_policy(24), {}, calendar_step{.months = 1}));
}
