#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include <devils_script/system.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>

#include "core/goap_resource.h"

using namespace tile_frontier::core;

TEST_CASE("GOAP config inheritance preserves base order and overlays by semantic key [goap][config]") {
  goap_config base;
  base.metrics.push_back(goap_metric{"sense", {}});
  base.metrics.push_back(goap_metric{"hungry", {}});
  base.actions.push_back(goap_action_config{"walk", {}, {"done"}, {}});
  base.actions.push_back(goap_action_config{"eat", {"hungry"}, {"done"}, {}});
  base.goals.push_back(goap_goal_config{"done", {}, {"done"}});

  goap_config derived;
  derived.base = "base";
  derived.metrics.push_back(goap_metric{"hungry", {}}); // replace in place
  derived.actions.push_back(goap_action_config{"eat", {"hungry", "sense"}, {"done"}, {}});
  derived.actions.push_back(goap_action_config{"sleep", {}, {"done"}, {}});
  derived.disable_actions.push_back("walk");

  const auto flat = merge_goap_config(base, derived);
  CHECK(flat.base.empty());
  REQUIRE(flat.metrics.size() == 2);
  CHECK(flat.metrics[0].key == "sense");
  CHECK(flat.metrics[1].key == "hungry");
  REQUIRE(flat.actions.size() == 2);
  CHECK(flat.actions[0].name == "eat");
  CHECK(flat.actions[0].requirements == std::vector<std::string>{"hungry", "sense"});
  CHECK(flat.actions[1].name == "sleep");
  REQUIRE(flat.goals.size() == 1);
  CHECK(flat.goals[0].name == "done");
}

TEST_CASE("GOAP resource resolves a single-parent config chain [goap][config][demiurg]") {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "devils_engine_goap_inheritance_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "goap");
  {
    std::ofstream base(root / "core" / "goap" / "base.tavl");
    base << "metrics = [ alive = true ]\n";
    base << "actions = [ { name = walk requirements = [ alive ] next_state = [ done ] weight_state = [ ] } ]\n";
    base << "goals = [ { name = done requirements = [ ] goal = [ done ] } ]\n";
  }
  {
    std::ofstream derived(root / "core" / "goap" / "derived.tavl");
    derived << "base = base\n";
    derived << "disable_actions = [ walk ]\n";
    derived << "actions = [ { name = sleep requirements = [ alive ] next_state = [ done ] weight_state = [ ] } ]\n";
  }

  devils_script::system scripts;
  scripts.init_basic_functions();
  scripts.init_math();
  devils_engine::demiurg::module_system modules(root.generic_string() + "/");
  modules.load_modules({devils_engine::demiurg::module_system::list_entry{"core/", "", ""}});
  devils_engine::demiurg::resource_system resources;
  resources.register_type<goap_resource>("goap", "tavl", &scripts);
  resources.parse_resources(&modules);

  const auto flat = resolve_goap_config(resources, "derived");
  REQUIRE(flat.metrics.size() == 1);
  CHECK(flat.metrics[0].key == "alive");
  REQUIRE(flat.actions.size() == 1);
  CHECK(flat.actions[0].name == "sleep");
  REQUIRE(flat.goals.size() == 1);
  CHECK(flat.goals[0].name == "done");

  fs::remove_all(root);
}
