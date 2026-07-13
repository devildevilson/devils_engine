#include <doctest/doctest.h>

#include <tavl/deserialize.h>
#include <tavl/parser.h>

#include <devils_engine/utils/timeline.h>

#include "core/config.h"

using namespace devils_engine;
using namespace tile_frontier::core;

TEST_CASE("tile_frontier project config selects a turn-driven calendar [config][time][calendar]") {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(R"(
    time = {
      game_seconds = 60
      real_seconds = 1
      calendar = {
        source = turn
        hours_per_day = 10
        days_in_month = [ 3, 2 ]
        start_year = 1
        start_month = 1
        start_day = 3
        days_per_turn = 0
        months_per_turn = 1
      }
    }
  )");
  parser.finish();

  tavl::ct_context context;
  app_config cfg{};
  tavl::deserialize(parser, context, cfg);
  // Partial config intentionally produces warn_missing_field diagnostics; C++ defaults fill them.
  CHECK(cfg.time.game_seconds == 60);
  CHECK(cfg.time.calendar.source == "turn");
  CHECK(cfg.time.calendar.days_in_month == std::vector<uint32_t>{3, 2});

  const auto calendar = make_calendar_clock(cfg.time);
  CHECK(calendar.source() == utils::calendar_source::turn);
  utils::timelines clocks;
  clocks.set_turn({1});
  const auto date = calendar.date(clocks);
  CHECK(date.year == 1);
  CHECK(date.month == 2);
  CHECK(date.day == 2); // third day clamps into the two-day target month
}
