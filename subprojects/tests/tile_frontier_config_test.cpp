#include <devils_engine/utils/timeline.h>
#include <devils_engine/simul/configuration_layers.h>
#include <doctest/doctest.h>
#include <tavl/deserialize.h>
#include <tavl/parser.h>
#include <tavl/serialize.h>

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
  CHECK(cfg.simulation.tick_rate == 60);
  CHECK(cfg.simulation.max_steps_per_frame == 8);

  const auto calendar = make_calendar_clock(cfg.time);
  CHECK(calendar.source() == utils::calendar_source::turn);
  utils::timelines clocks;
  clocks.set_turn({1});
  const auto date = calendar.date(clocks);
  CHECK(date.year == 1);
  CHECK(date.month == 2);
  CHECK(date.day == 2); // third day clamps into the two-day target month
}

TEST_CASE("tile_frontier user settings expose only player-facing sections") {
  user_settings settings;
  std::string data;
  REQUIRE(tavl::serialize(settings, data));

  CHECK(data.find("window") != std::string::npos);
  CHECK(data.find("fullscreen") != std::string::npos);
  CHECK(data.find("monitor") != std::string::npos);
  CHECK(data.find("sound") != std::string::npos);
  CHECK(data.find("device") != std::string::npos);
  CHECK(data.find("metrics") == std::string::npos);
  CHECK(data.find("logging") == std::string::npos);
  CHECK(data.find("vulkan_debug") == std::string::npos);
  CHECK(data.find("lua_instruction_limit") != std::string::npos);
  CHECK(data.find("convert_wall_time_us") != std::string::npos);

  CHECK(data.find("simulation = {") == std::string::npos);
  CHECK(data.find("render = {") == std::string::npos);
  CHECK(data.find("time = {") == std::string::npos);
  CHECK(data.find("title") == std::string::npos);
  CHECK(data.find("create_on_start") == std::string::npos);
}

TEST_CASE("engine project and debug configuration sections are disjoint and atomic") {
  namespace s = devils_engine::simul;
  app_config cfg;
  s::deserialize_configuration_layer("simulation = { sound_mixing = device }", "engine", s::engine_configuration_sections, cfg);
  CHECK(cfg.simulation.sound_mixing == "device");
  constexpr std::array<std::string_view, 1> project{"time"};
  s::deserialize_configuration_layer("time = { game_seconds = 7 }", "project", project, cfg);
  CHECK(cfg.time.game_seconds == 7);
  CHECK(cfg.simulation.sound_mixing == "device");
  s::deserialize_configuration_layer("logging = { vulkan_debug = true }", "debug", s::debug_configuration_sections, cfg);
  CHECK(cfg.logging.vulkan_debug);
  CHECK(cfg.time.game_seconds == 7);
  CHECK_THROWS_AS(s::deserialize_configuration_layer("simulation = { sound_enabled = false }", "debug", s::debug_configuration_sections, cfg), utils::critical_error);
  CHECK(cfg.simulation.sound_enabled);
  CHECK_THROWS_AS(s::deserialize_configuration_layer("{ simulation = { sound_enabled = false } }", "debug", s::debug_configuration_sections, cfg), utils::critical_error);
  CHECK_THROWS_AS(s::deserialize_configuration_layer("time = { game_seconds = bad }", "project", project, cfg), utils::critical_error);
  CHECK(cfg.time.game_seconds == 7);
}
