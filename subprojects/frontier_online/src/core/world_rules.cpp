#include "core/world_rules.h"

namespace frontier_online::core {

const time_config& world_time_rules() noexcept {
  static const time_config rules = [] {
    time_config value;
    value.game_seconds = 1;
    value.real_seconds = 1;
    value.calendar.source = "game_time";
    value.calendar.hours_per_day = 24;
    value.calendar.days_in_month = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    value.calendar.start_absolute_day = 0;
    value.calendar.start_year = 0;
    value.calendar.start_month = 1;
    value.calendar.start_day = 1;
    value.calendar.days_per_turn = 1;
    return value;
  }();
  return rules;
}

} // namespace frontier_online::core
