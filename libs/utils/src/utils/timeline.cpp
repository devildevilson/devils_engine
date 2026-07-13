#include "devils_engine/utils/timeline.h"

namespace devils_engine {
namespace utils {
calendar_source parse_calendar_source(const std::string_view value) {
  if (value == "game_time") {
    return calendar_source::game_time;
  }
  if (value == "turn") {
    return calendar_source::turn;
  }
  throw std::invalid_argument("calendar source must be 'game_time' or 'turn'");
}
} // namespace utils
} // namespace devils_engine
