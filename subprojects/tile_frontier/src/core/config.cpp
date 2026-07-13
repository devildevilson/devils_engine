#include <utility>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/timeline.h>

#include "config.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

utils::calendar_clock make_calendar_clock(const time_config& cfg) {
  const auto& c = cfg.calendar;
  utils::calendar_policy policy(c.hours_per_day, c.days_in_month);
  const auto epoch = policy.has_calendar()
                       ? policy.compose_calendar(c.start_year, c.start_month, c.start_day,
                                                 c.start_hour, c.start_minute, c.start_second)
                       : policy.compose(c.start_absolute_day, c.start_hour, c.start_minute, c.start_second);
  return utils::calendar_clock(
    utils::parse_calendar_source(c.source), std::move(policy), epoch,
    utils::calendar_step{c.seconds_per_turn, c.days_per_turn, c.months_per_turn, c.years_per_turn});
}

std::string make_project_path(std::string path) {
  if (path.empty()) {
    return utils::project_folder();
  }
  if (path.front() == '/') {
    return path;
  }
  return utils::project_folder() + path;
}

std::string make_project_folder_path(std::string path) {
  path = make_project_path(std::move(path));
  if (!path.empty() && path.back() != '/') {
    path.push_back('/');
  }
  return path;
}

} // namespace core
} // namespace tile_frontier
