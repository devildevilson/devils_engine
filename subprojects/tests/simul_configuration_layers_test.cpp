#include <chrono>
#include <filesystem>
#include <doctest/doctest.h>
#include <devils_engine/simul/main_runtime.h>
#include "core/app_config_resource.h"

namespace s = devils_engine::simul;
namespace tf = tile_frontier::core;
using namespace devils_engine;
namespace {
struct test_bootstrap : s::standard_runtime_bootstrap<tf::app_config, tf::user_settings> {
  void export_persisted_settings() {
    persisted_settings.window = {settings.window.width, settings.window.height, settings.window.fullscreen, settings.window.monitor};
    persisted_settings.sound = settings.sound;
    persisted_settings.ui = settings.ui;
  }
  void import_persisted_settings() {
    settings.window.width = persisted_settings.window.width;
    settings.window.height = persisted_settings.window.height;
    settings.window.fullscreen = persisted_settings.window.fullscreen;
    settings.window.monitor = persisted_settings.window.monitor;
    settings.sound = persisted_settings.sound;
    settings.ui = persisted_settings.ui;
  }
};
struct legacy_settings {
  tf::user_window_config window;
  tf::sound_config sound;
  tf::metrics_config metrics;
  tf::logging_config logging;
  tf::ui_config ui;
  tf::time_config time;
};
struct temporary_folder {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
    ("devils_config_layers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  temporary_folder() { std::filesystem::create_directories(path); }
  ~temporary_folder() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
}

TEST_CASE("shipped three configuration resources compose with the same project defaults") {
  demiurg::module_system modules(ENGINE_CONFIG_TEST_ROOT);
  modules.load_modules({demiurg::module_system::list_entry{"engine", "", ""}});
  demiurg::resource_system resources;
  resources.register_type<tf::app_config_resource>("config", "tavl");
  resources.parse_resources(&modules);
  tf::app_config settings;
  for (auto id : {"config/engine", "config/project", "config/debug"}) {
    auto* resource = resources.get<tf::app_config_resource>(id);
    REQUIRE(resource != nullptr);
    resource->load(utils::safe_handle_t{});
    resource->apply_to(settings);
  }
  CHECK(settings.simulation.sound_mixing == "owner");
  CHECK(settings.simulation.tick_rate == 60);
  CHECK(settings.window.width == 1280);
  CHECK(settings.render.cache_folder == "cache");
  CHECK(settings.time.calendar.days_in_month == std::vector<uint32_t>{31,28,31,30,31,30,31,31,30,31,30,31});
  CHECK(settings.logging.main == "off");
}

TEST_CASE("legacy user debug preferences migrate once and remain independent from player settings") {
  temporary_folder folder;
  test_bootstrap boot;
  boot.engine.settings_file = (folder.path / "settings.tavl").string();
  boot.engine.debug_settings_file = (folder.path / "debug.tavl").string();
  boot.engine.key_bindings_file = (folder.path / "keys.tavl").string();
  boot.default_settings = boot.settings;
  s::export_persisted_settings(boot);
  boot.default_persisted_settings = boot.persisted_settings;
  boot.default_persisted_debug = boot.persisted_debug;
  legacy_settings legacy;
  legacy.logging.main = "flow";
  legacy.metrics.log_interval_ms = 777;
  legacy.window.width = 1800;
  legacy.time.game_seconds = 999; // player file must not override project policy.
  REQUIRE(s::write_tavl_file(legacy, boot.engine.settings_file, "legacy test"));
  REQUIRE(s::deserialize_settings_file(boot, boot.engine.settings_file) == s::config_file_status::loaded);
  CHECK(boot.settings.window.width == 1800);
  CHECK(boot.settings.logging.main == "flow");
  CHECK(boot.settings.metrics.log_interval_ms == 777);
  CHECK(boot.settings.time.game_seconds == 1);
  const auto migrated = file_io::read(boot.engine.settings_file);
  CHECK(migrated.find("logging") == std::string::npos);
  CHECK(migrated.find("metrics") == std::string::npos);
  CHECK(migrated.find("time =") == std::string::npos);
  REQUIRE(file_io::exists(boot.engine.debug_settings_file));
  legacy.logging.main = "trace";
  REQUIRE(s::write_tavl_file(legacy, boot.engine.settings_file, "legacy test"));
  REQUIRE(s::deserialize_settings_file(boot, boot.engine.settings_file) == s::config_file_status::loaded);
  CHECK(boot.settings.logging.main == "flow"); // existing debug document wins.
  boot.settings.logging.main = "trace";
  REQUIRE(s::save_settings(boot));
  const auto user = file_io::read(boot.engine.settings_file);
  const auto debug = file_io::read(boot.engine.debug_settings_file);
  CHECK(user.find("logging") == std::string::npos);
  CHECK(debug.find("trace") != std::string::npos);
}
