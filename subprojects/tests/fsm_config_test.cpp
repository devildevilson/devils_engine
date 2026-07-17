#include <filesystem>
#include <fstream>
#include <memory>

#include <devils_engine/act/function.h>
#include <devils_engine/act/registry.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/mood/system.h>
#include <devils_engine/utils/safe_handle.h>
#include <doctest/doctest.h>

#include "fsm_resource.h"

TEST_CASE("FSM resource parses native TAVL transition rows and parenthesized actions") {
  namespace fs = std::filesystem;
  namespace tf = tile_frontier::core;
  using namespace devils_engine;

  const auto root = fs::temp_directory_path() / "devils_engine_fsm_config_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "fsm");
  {
    std::ofstream out(root / "core" / "fsm" / "combat.tavl");
    out << "transitions = [\n";
    out << "  idle + see [can_see, awake] / (remember, signal) = alert\n";
    out << "  alert + calm / (forget) = idle\n";
    out << "]\n";
  }

  demiurg::module_system modules(root.generic_string() + "/");
  modules.load_default_modules();
  demiurg::resource_system resources;
  resources.register_type<tf::fsm_resource>("fsm", "tavl");
  resources.parse_resources(&modules);

  auto* resource = resources.get<tf::fsm_resource>("fsm/combat");
  REQUIRE(resource != nullptr);
  resource->load(utils::safe_handle_t{});
  REQUIRE(resource->transitions().size() == 2);
  const auto& first = resource->transitions()[0];
  CHECK(first.current_state == "idle");
  CHECK(first.event == "see");
  CHECK(first.guards == std::vector<std::string>{"can_see", "awake"});
  CHECK(first.actions == std::vector<std::string>{"remember", "signal"});
  CHECK(first.next_state == "alert");

  act::registry functions;
  functions.reg("can_see", std::make_unique<act::native_function<bool>>(+[](const act::exec_context&) { return true; }));
  functions.reg("awake", std::make_unique<act::native_function<bool>>(+[](const act::exec_context&) { return true; }));
  for (const std::string_view name : {"remember", "signal", "forget"}) {
    functions.reg(name, std::make_unique<act::native_function<void>>(+[](const act::exec_context&) {}));
  }
  mood::system system(&functions, resource->transitions());
  const auto found = system.find_transitions("idle", "see");
  REQUIRE(found.size() == 1);
  CHECK(found[0].guards[1] == "awake");
  CHECK(found[0].actions[1] == "signal");

  fs::remove_all(root);
}
