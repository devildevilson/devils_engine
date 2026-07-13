#ifndef DEVILS_ENGINE_UTILS_TIMELINE_H
#define DEVILS_ENGINE_UTILS_TIMELINE_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace devils_engine {
namespace utils {

inline constexpr uint64_t timeline_ticks_per_second = UINT64_C(1000000); // microseconds

enum class clock_domain : uint8_t { engine, game };

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
using game_timestamp = basic_timestamp<clock_domain::game>;
using engine_duration = basic_duration<clock_domain::engine>;
using game_duration = basic_duration<clock_domain::game>;

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
using game_deadline = basic_deadline<clock_domain::game>;

// Обе шкалы получают один engine delta. Engine идёт всегда; game не накапливает delta на паузе.
class timelines {
public:
  constexpr engine_timestamp engine_now() const noexcept { return {engine_ticks_}; }
  constexpr game_timestamp game_now() const noexcept { return {game_ticks_}; }

  constexpr void advance(const uint64_t delta_ticks) noexcept {
    engine_ticks_ += delta_ticks;
    if (!game_paused_) game_ticks_ += delta_ticks;
  }

  constexpr void set_game_paused(const bool value) noexcept { game_paused_ = value; }
  constexpr bool game_paused() const noexcept { return game_paused_; }
  constexpr void set_game_time(const game_timestamp value) noexcept { game_ticks_ = value.ticks; }

private:
  uint64_t engine_ticks_ = 0;
  uint64_t game_ticks_ = 0;
  bool game_paused_ = false;
};

struct game_time_parts {
  uint64_t absolute_day = 0;
  uint32_t hour = 0;
  uint32_t minute = 0;
  uint32_t second = 0;
  uint32_t subsecond_ticks = 0;
};

struct calendar_fields : game_time_parts {
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
  uint64_t seconds_per_day() const noexcept { return uint64_t(hours_per_day_) * 60u * 60u; }
  uint64_t days_per_year() const noexcept { return days_per_year_; }
  bool has_calendar() const noexcept { return !days_in_month_.empty(); }

  game_time_parts split(const game_timestamp stamp) const noexcept {
    const uint64_t whole_seconds = stamp.ticks / timeline_ticks_per_second;
    uint64_t in_day = whole_seconds % seconds_per_day();
    game_time_parts out;
    out.absolute_day = whole_seconds / seconds_per_day();
    out.hour = static_cast<uint32_t>(in_day / 3600u);
    in_day %= 3600u;
    out.minute = static_cast<uint32_t>(in_day / 60u);
    out.second = static_cast<uint32_t>(in_day % 60u);
    out.subsecond_ticks = static_cast<uint32_t>(stamp.ticks % timeline_ticks_per_second);
    return out;
  }

  calendar_fields project(const game_timestamp stamp) const noexcept {
    const auto parts = split(stamp);
    calendar_fields out;
    static_cast<game_time_parts&>(out) = parts;
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

  game_timestamp compose(const uint64_t absolute_day, const uint32_t hour = 0,
                         const uint32_t minute = 0, const uint32_t second = 0,
                         const uint32_t subsecond_ticks = 0) const {
    validate_time(hour, minute, second, subsecond_ticks);
    const uint64_t whole_seconds = absolute_day * seconds_per_day() +
      uint64_t(hour) * 3600u + uint64_t(minute) * 60u + second;
    return {whole_seconds * timeline_ticks_per_second + subsecond_ticks};
  }

  game_timestamp compose_calendar(const uint64_t year, const uint32_t month, const uint32_t day,
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

}
}

#endif
