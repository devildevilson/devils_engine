#ifndef DEVILS_ENGINE_UTILS_TIMELINE_H
#define DEVILS_ENGINE_UTILS_TIMELINE_H

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace devils_engine {
namespace utils {

inline constexpr uint64_t timeline_ticks_per_second = UINT64_C(1000000); // microseconds

enum class clock_domain : uint8_t { engine, presentation, game, calendar };

template <clock_domain Domain>
struct basic_timestamp {
  uint64_t ticks = 0;
  constexpr auto operator<=>(const basic_timestamp&) const noexcept = default;
};

template <clock_domain Domain>
struct basic_duration {
  uint64_t ticks = 0;
  constexpr auto operator<=>(const basic_duration&) const noexcept = default;

  static constexpr basic_duration from_seconds(const uint64_t value) noexcept {
    return {value * timeline_ticks_per_second};
  }
  static constexpr basic_duration from_minutes(const uint64_t value) noexcept {
    return from_seconds(value * 60);
  }
  static constexpr basic_duration from_hours(const uint64_t value) noexcept {
    return from_minutes(value * 60);
  }
};

using engine_timestamp = basic_timestamp<clock_domain::engine>;
using presentation_timestamp = basic_timestamp<clock_domain::presentation>;
using game_timestamp = basic_timestamp<clock_domain::game>;
using calendar_timestamp = basic_timestamp<clock_domain::calendar>;
using engine_duration = basic_duration<clock_domain::engine>;
using presentation_duration = basic_duration<clock_domain::presentation>;
using game_duration = basic_duration<clock_domain::game>;
using calendar_duration = basic_duration<clock_domain::calendar>;

template <clock_domain Domain>
constexpr basic_timestamp<Domain> operator+(const basic_timestamp<Domain> t, const basic_duration<Domain> d) noexcept {
  return {t.ticks + d.ticks};
}

template <clock_domain Domain>
struct basic_deadline {
  basic_timestamp<Domain> at{};
  constexpr bool elapsed(const basic_timestamp<Domain> now) const noexcept { return now >= at; }
};

using engine_deadline = basic_deadline<clock_domain::engine>;
using presentation_deadline = basic_deadline<clock_domain::presentation>;
using game_deadline = basic_deadline<clock_domain::game>;
using calendar_deadline = basic_deadline<clock_domain::calendar>;

// Nominal project mapping between user/engine durations and in-world durations. Config convention:
// an unqualified duration is an engine/nominal-real duration; gameplay loaders convert it here,
// while explicitly typed game/calendar/turn values remain in their domain. It is deliberately
// a duration mapping, not timestamp conversion: pauses/rate changes make absolute mapping ambiguous.
class game_time_scale {
public:
  constexpr explicit game_time_scale(const uint32_t game_ticks = 1,
                                     const uint32_t engine_ticks = 1)
    : game_ticks_(game_ticks), engine_ticks_(engine_ticks) {
    if (game_ticks == 0 || engine_ticks == 0) {
      throw std::invalid_argument("game_time_scale: ratio terms must be positive");
    }
  }

  static constexpr game_time_scale from_seconds(const uint32_t game_seconds,
                                                const uint32_t engine_seconds = 1) {
    return game_time_scale(game_seconds, engine_seconds);
  }

  constexpr game_duration to_game(const engine_duration value) const noexcept {
    return {scale(value.ticks, game_ticks_, engine_ticks_)};
  }
  constexpr engine_duration to_engine(const game_duration value) const noexcept {
    return {scale(value.ticks, engine_ticks_, game_ticks_)};
  }

  constexpr uint32_t game_ticks() const noexcept { return game_ticks_; }
  constexpr uint32_t engine_ticks() const noexcept { return engine_ticks_; }

private:
  static constexpr uint64_t scale(const uint64_t value, const uint64_t mul,
                                  const uint64_t div) noexcept {
    return (value / div) * mul + ((value % div) * mul) / div;
  }

  uint32_t game_ticks_ = 1;
  uint32_t engine_ticks_ = 1;
};

struct turn_index {
  uint64_t value = 0;
  constexpr auto operator<=>(const turn_index&) const noexcept = default;
};

struct turn_duration { uint64_t turns = 0; };
constexpr turn_index operator+(const turn_index t, const turn_duration d) noexcept { return {t.value + d.turns}; }

struct turn_deadline {
  turn_index at{};
  constexpr bool elapsed(const turn_index now) const noexcept { return now >= at; }
};

// Engine always advances. Presentation is real-rate but pausable (world animation). Game is both
// pausable and scaled. Turns are an orthogonal discrete gameplay coordinate.
class timelines {
public:
  constexpr engine_timestamp engine_now() const noexcept { return {engine_ticks_}; }
  constexpr presentation_timestamp presentation_now() const noexcept { return {presentation_ticks_}; }
  constexpr game_timestamp game_now() const noexcept { return {game_ticks_}; }
  constexpr turn_index turn_now() const noexcept { return {turn_}; }

  constexpr void advance(const uint64_t delta_ticks) noexcept {
    engine_ticks_ += delta_ticks;
    if (!presentation_paused_) presentation_ticks_ += delta_ticks;
    if (!game_paused_) {
      game_ticks_ += (delta_ticks / scale_.engine_ticks()) * scale_.game_ticks();
      const uint64_t scaled_remainder = (delta_ticks % scale_.engine_ticks()) * scale_.game_ticks() + game_remainder_;
      game_ticks_ += scaled_remainder / scale_.engine_ticks();
      game_remainder_ = scaled_remainder % scale_.engine_ticks();
    }
  }

  constexpr void set_game_paused(const bool value) noexcept { game_paused_ = value; }
  constexpr bool game_paused() const noexcept { return game_paused_; }
  constexpr void set_presentation_paused(const bool value) noexcept { presentation_paused_ = value; }
  constexpr bool presentation_paused() const noexcept { return presentation_paused_; }
  constexpr void set_world_paused(const bool value) noexcept {
    game_paused_ = value;
    presentation_paused_ = value;
  }

  constexpr void set_game_scale(const game_time_scale value) noexcept {
    scale_ = value;
    game_remainder_ = 0;
  }
  constexpr game_time_scale game_scale() const noexcept { return scale_; }

  constexpr void set_game_time(const game_timestamp value) noexcept { game_ticks_ = value.ticks; }
  constexpr void advance_turns(const turn_duration value = {1}) noexcept {
    if (!game_paused_) turn_ += value.turns;
  }
  constexpr void set_turn(const turn_index value) noexcept { turn_ = value.value; }

private:
  uint64_t engine_ticks_ = 0;
  uint64_t presentation_ticks_ = 0;
  uint64_t game_ticks_ = 0;
  uint64_t turn_ = 0;
  uint64_t game_remainder_ = 0;
  game_time_scale scale_{};
  bool game_paused_ = false;
  bool presentation_paused_ = false;
};

struct calendar_time_parts {
  uint64_t absolute_day = 0;
  uint32_t hour = 0;
  uint32_t minute = 0;
  uint32_t second = 0;
  uint32_t subsecond_ticks = 0;
};

using game_time_parts = calendar_time_parts; // compatibility name; new code should use calendar_time_parts

struct calendar_fields : calendar_time_parts {
  // year начинается с 0; month/day — с 1. has_calendar=false означает, что проект использует
  // только absolute_day и/или time-of-day и не задавал месяцы.
  uint64_t year = 0;
  uint32_t month = 0;
  uint32_t day = 0;
  bool has_calendar = false;
};

class calendar_policy {
public:
  explicit calendar_policy(const uint32_t hours_per_day = 24,
                           std::vector<uint32_t> days_in_month = {})
    : hours_per_day_(hours_per_day), days_in_month_(std::move(days_in_month)) {
    if (hours_per_day_ == 0) throw std::invalid_argument("calendar_policy: hours_per_day must be positive");
    for (const auto days : days_in_month_) {
      if (days == 0) throw std::invalid_argument("calendar_policy: month must contain at least one day");
      days_per_year_ += days;
    }
  }

  uint32_t hours_per_day() const noexcept { return hours_per_day_; }
  uint32_t months_per_year() const noexcept { return static_cast<uint32_t>(days_in_month_.size()); }
  uint32_t days_in_month(const uint32_t month) const {
    if (month == 0 || month > days_in_month_.size()) throw std::out_of_range("calendar_policy: invalid month");
    return days_in_month_[month - 1];
  }
  uint64_t seconds_per_day() const noexcept { return uint64_t(hours_per_day_) * 60u * 60u; }
  uint64_t days_per_year() const noexcept { return days_per_year_; }
  bool has_calendar() const noexcept { return !days_in_month_.empty(); }

  calendar_time_parts split(const calendar_timestamp stamp) const noexcept {
    const uint64_t whole_seconds = stamp.ticks / timeline_ticks_per_second;
    uint64_t in_day = whole_seconds % seconds_per_day();
    calendar_time_parts out;
    out.absolute_day = whole_seconds / seconds_per_day();
    out.hour = static_cast<uint32_t>(in_day / 3600u);
    in_day %= 3600u;
    out.minute = static_cast<uint32_t>(in_day / 60u);
    out.second = static_cast<uint32_t>(in_day % 60u);
    out.subsecond_ticks = static_cast<uint32_t>(stamp.ticks % timeline_ticks_per_second);
    return out;
  }

  calendar_fields project(const calendar_timestamp stamp) const noexcept {
    const auto parts = split(stamp);
    calendar_fields out;
    static_cast<calendar_time_parts&>(out) = parts;
    if (!has_calendar()) return out;

    out.has_calendar = true;
    out.year = parts.absolute_day / days_per_year_;
    uint64_t day_of_year = parts.absolute_day % days_per_year_;
    for (size_t i = 0; i < days_in_month_.size(); ++i) {
      if (day_of_year < days_in_month_[i]) {
        out.month = static_cast<uint32_t>(i + 1);
        out.day = static_cast<uint32_t>(day_of_year + 1);
        break;
      }
      day_of_year -= days_in_month_[i];
    }
    return out;
  }

  calendar_timestamp compose(const uint64_t absolute_day, const uint32_t hour = 0,
                             const uint32_t minute = 0, const uint32_t second = 0,
                             const uint32_t subsecond_ticks = 0) const {
    validate_time(hour, minute, second, subsecond_ticks);
    const uint64_t whole_seconds = absolute_day * seconds_per_day() +
      uint64_t(hour) * 3600u + uint64_t(minute) * 60u + second;
    return {whole_seconds * timeline_ticks_per_second + subsecond_ticks};
  }

  calendar_timestamp compose_calendar(const uint64_t year, const uint32_t month, const uint32_t day,
                                      const uint32_t hour = 0, const uint32_t minute = 0,
                                      const uint32_t second = 0, const uint32_t subsecond_ticks = 0) const {
    if (!has_calendar()) throw std::logic_error("calendar_policy: no months configured");
    if (month == 0 || month > days_in_month_.size()) throw std::out_of_range("calendar_policy: invalid month");
    if (day == 0 || day > days_in_month_[month - 1]) throw std::out_of_range("calendar_policy: invalid day");
    uint64_t absolute_day = year * days_per_year_;
    for (uint32_t i = 0; i + 1 < month; ++i) absolute_day += days_in_month_[i];
    absolute_day += day - 1;
    return compose(absolute_day, hour, minute, second, subsecond_ticks);
  }

private:
  void validate_time(const uint32_t hour, const uint32_t minute, const uint32_t second,
                     const uint32_t subsecond_ticks) const {
    if (hour >= hours_per_day_ || minute >= 60 || second >= 60 ||
        subsecond_ticks >= timeline_ticks_per_second) {
      throw std::out_of_range("calendar_policy: invalid time of day");
    }
  }

  uint32_t hours_per_day_ = 24;
  std::vector<uint32_t> days_in_month_;
  uint64_t days_per_year_ = 0;
};

enum class calendar_source : uint8_t { game_time, turn };

inline calendar_source parse_calendar_source(const std::string_view value) {
  if (value == "game_time") return calendar_source::game_time;
  if (value == "turn") return calendar_source::turn;
  throw std::invalid_argument("calendar source must be 'game_time' or 'turn'");
}

// Calendar amount contributed by one turn. Seconds/days are linear; months/years use the
// configured calendar and clamp the origin day when a target month is shorter.
struct calendar_step {
  uint64_t seconds = 0;
  uint64_t days = 0;
  uint64_t months = 0;
  uint64_t years = 0;

  constexpr bool empty() const noexcept {
    return seconds == 0 && days == 0 && months == 0 && years == 0;
  }
};

// Immutable project-level calendar adapter. Both game time and turns remain available; this
// object only selects which one drives the project date. Construct once during project startup.
class calendar_clock {
public:
  explicit calendar_clock(calendar_source source = calendar_source::game_time,
                          calendar_policy policy = calendar_policy{}, calendar_timestamp epoch = {},
                          calendar_step per_turn = {})
    : source_(source), policy_(std::move(policy)), epoch_(epoch), per_turn_(per_turn) {
    if (source_ == calendar_source::turn && per_turn_.empty()) {
      throw std::invalid_argument("calendar_clock: turn source requires a non-zero per-turn step");
    }
    if ((per_turn_.months != 0 || per_turn_.years != 0) && !policy_.has_calendar()) {
      throw std::invalid_argument("calendar_clock: month/year turn step requires configured months");
    }
  }

  calendar_timestamp now(const timelines& clocks) const {
    if (source_ == calendar_source::game_time) {
      return {checked_add(epoch_.ticks, clocks.game_now().ticks)};
    }
    return advance_from_epoch(clocks.turn_now().value);
  }

  calendar_fields date(const timelines& clocks) const { return policy_.project(now(clocks)); }
  calendar_source source() const noexcept { return source_; }
  const calendar_policy& policy() const noexcept { return policy_; }
  calendar_timestamp epoch() const noexcept { return epoch_; }
  calendar_step per_turn() const noexcept { return per_turn_; }

private:
  static uint64_t checked_add(const uint64_t a, const uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a) {
      throw std::overflow_error("calendar_clock: timestamp overflow");
    }
    return a + b;
  }

  static uint64_t checked_mul(const uint64_t a, const uint64_t b) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
      throw std::overflow_error("calendar_clock: timestamp overflow");
    }
    return a * b;
  }

  calendar_timestamp advance_from_epoch(const uint64_t turns) const {
    calendar_timestamp out = epoch_;

    if (per_turn_.months != 0 || per_turn_.years != 0) {
      const auto origin = policy_.project(epoch_);
      const uint64_t months_per_year = policy_.months_per_year();
      const uint64_t months_each = checked_add(checked_mul(per_turn_.years, months_per_year), per_turn_.months);
      const uint64_t origin_month = checked_add(checked_mul(origin.year, months_per_year), origin.month - 1u);
      const uint64_t target_month = checked_add(origin_month, checked_mul(months_each, turns));
      const uint64_t year = target_month / months_per_year;
      const uint32_t month = static_cast<uint32_t>(target_month % months_per_year) + 1u;

      const uint32_t day = std::min(origin.day, policy_.days_in_month(month));
      out = policy_.compose_calendar(year, month, day, origin.hour, origin.minute,
                                     origin.second, origin.subsecond_ticks);
    }

    const uint64_t seconds_each = checked_add(
      checked_mul(per_turn_.days, policy_.seconds_per_day()), per_turn_.seconds);
    const uint64_t delta_ticks = checked_mul(
      checked_mul(seconds_each, turns), timeline_ticks_per_second);
    out.ticks = checked_add(out.ticks, delta_ticks);
    return out;
  }

  calendar_source source_ = calendar_source::game_time;
  calendar_policy policy_;
  calendar_timestamp epoch_{};
  calendar_step per_turn_{};
};

}
}

#endif
