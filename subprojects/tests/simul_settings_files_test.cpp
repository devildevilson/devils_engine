#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include <devils_engine/simul/main_runtime.h>
#include <devils_engine/utils/fileio.h>

namespace {

struct test_settings {
  uint32_t width = 1280;
  std::string title = "default";
};

struct temp_folder {
  std::filesystem::path path;

  temp_folder() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("devils_engine_settings_files_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
  }

  ~temp_folder() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

} // namespace

TEST_CASE("runtime tavl files create, load and fall back atomically") {
  using devils_engine::simul::config_file_status;

  temp_folder dir;
  const auto path = (dir.path / "settings.tavl").string();
  const test_settings defaults{};
  test_settings loaded{42, "dirty"};

  CHECK(devils_engine::simul::load_tavl_file(path, defaults, loaded, "test settings") ==
        config_file_status::missing);
  CHECK(loaded.width == defaults.width);
  CHECK(loaded.title == defaults.title);

  const test_settings custom{1920, "custom"};
  REQUIRE(devils_engine::simul::write_tavl_file(custom, path, "test settings"));
  REQUIRE(devils_engine::file_io::exists(path));
  CHECK(devils_engine::simul::load_tavl_file(path, defaults, loaded, "test settings") ==
        config_file_status::loaded);
  CHECK(loaded.width == custom.width);
  CHECK(loaded.title == custom.title);

  REQUIRE(devils_engine::file_io::write(
    std::string("width = not_a_number\ntitle = 7\n"), path));
  loaded = custom;
  CHECK(devils_engine::simul::load_tavl_file(path, defaults, loaded, "test settings") ==
        config_file_status::invalid);
  CHECK(loaded.width == defaults.width);
  CHECK(loaded.title == defaults.title);
}
