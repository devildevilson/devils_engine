#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/lifecycle.h>
#include <devils_engine/simul/startup_resources.h>
#include <devils_engine/utils/safe_handle.h>

namespace {

using devils_engine::simul::app_state;

struct lifecycle_host {
  std::vector<app_state> entered;
  std::vector<app_state> left;
  std::vector<app_state> ticked;
  bool boot_ready = false;
  bool loading_ready = false;

  void on_lifecycle_enter(const app_state phase) { entered.push_back(phase); }
  void on_lifecycle_tick(const app_state phase, const size_t) { ticked.push_back(phase); }
  bool lifecycle_phase_complete(const app_state phase) const {
    if (phase == app_state::boot) return boot_ready;
    if (phase == app_state::loading) return loading_ready;
    return false;
  }
  void on_lifecycle_leave(const app_state phase) { left.push_back(phase); }
};

}

TEST_CASE("lifecycle controller enters phases in strict order") {
  devils_engine::simul::lifecycle_controller lifecycle;
  lifecycle_host host;

  lifecycle.start(host);
  CHECK(lifecycle.phase() == app_state::boot);
  CHECK(host.entered == std::vector<app_state>{app_state::boot});

  lifecycle.update(host, 1);
  CHECK(lifecycle.phase() == app_state::boot);
  CHECK(host.left.empty());

  host.boot_ready = true;
  lifecycle.update(host, 1);
  CHECK(lifecycle.phase() == app_state::loading);
  CHECK(host.left == std::vector<app_state>{app_state::boot});
  CHECK(host.entered == std::vector<app_state>{app_state::boot, app_state::loading});

  host.loading_ready = true;
  lifecycle.update(host, 1);
  CHECK(lifecycle.phase() == app_state::game);
  CHECK(host.left == std::vector<app_state>{app_state::boot, app_state::loading});
  CHECK(host.entered == std::vector<app_state>{app_state::boot, app_state::loading, app_state::game});

  lifecycle.update(host, 1);
  CHECK(lifecycle.phase() == app_state::game);
  CHECK(host.entered.size() == 3);

  REQUIRE(lifecycle.request_loading());
  CHECK_FALSE(lifecycle.request_loading());
  lifecycle.update(host, 1);
  CHECK(lifecycle.phase() == app_state::loading);
  CHECK(host.entered.back() == app_state::loading);

  lifecycle.update(host, 1);
  CHECK(lifecycle.phase() == app_state::game);
  CHECK(host.entered.back() == app_state::game);
}

TEST_CASE("startup entry and runtime state come from the first module") {
  namespace fs = std::filesystem;
  namespace demiurg = devils_engine::demiurg;
  namespace simul = devils_engine::simul;

  const auto root = fs::temp_directory_path() / "devils_engine_simul_startup_test";
  fs::remove_all(root);
  fs::create_directories(root / "mod" / "startup");
  fs::create_directories(root / "mod" / "states");
  fs::create_directories(root / "core" / "startup");

  {
    std::ofstream out(root / "mod" / "startup" / "entry.tavl");
    out << "state = \"states/mod_menu\"\n";
  }
  {
    std::ofstream out(root / "mod" / "states" / "mod_menu.tavl");
    out << "script = \"ui/mod_entry\"\n";
    out << "resources = [\"ui/mod_entry\", \"fonts/mod\", \"textures/mod_bg\"]\n";
    out << "scene = \"scenes/mod_scene\"\n";
  }
  {
    std::ofstream out(root / "core" / "startup" / "entry.tavl");
    out << "state = \"states/core_menu\"\n";
  }

  demiurg::module_system modules(root.generic_string() + "/");
  modules.load_modules({
    demiurg::module_system::list_entry{"mod/", "", ""},
    demiurg::module_system::list_entry{"core/", "", ""}
  });

  demiurg::resource_system resources;
  resources.register_type<simul::startup_entry_resource>("startup", "tavl");
  resources.register_type<simul::runtime_state_resource>("states", "tavl");
  resources.parse_resources(&modules);

  auto* entry = resources.get<simul::startup_entry_resource>("startup/entry");
  REQUIRE(entry != nullptr);
  entry->load(devils_engine::utils::safe_handle_t{});
  CHECK(entry->config().state == "states/mod_menu");

  auto* ui = resources.get<simul::runtime_state_resource>(entry->config().state);
  REQUIRE(ui != nullptr);
  ui->load(devils_engine::utils::safe_handle_t{});
  CHECK(ui->config().script == "ui/mod_entry");
  CHECK(ui->config().resources == std::vector<std::string>{
    "ui/mod_entry", "fonts/mod", "textures/mod_bg"
  });
  CHECK(ui->config().scene == "scenes/mod_scene");

  fs::remove_all(root);
}
