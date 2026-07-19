#include <filesystem>
#include <fstream>

#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_compiler.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/utils/core.h>
#include <devils_script/system.h>
#include <doctest/doctest.h>
#include <tavl/parser.h>

using namespace devils_engine;
using namespace devils_engine::acumen;

namespace {
struct test_scope {
  bool valid() const noexcept { return true; }
};

void config_effect(test_scope) noexcept {}

struct test_script_compiler final : act::script_compiler {
  devils_script::system sys;

  test_script_compiler() {
    sys.init_basic_functions();
    sys.init_math();
    sys.register_function<&config_effect>("config_effect");
  }

  void configure_parser(tavl::parser& parser) const override {
    sys.configure_parser(parser);
  }

  act::compiled_script compile(std::string_view name,
                               std::string_view return_type,
                               std::string_view scope,
                               std::string_view expression) const override {
    if (return_type == "bool" && scope == "test") {
      return {sys.parse<bool, test_scope>(name, expression), act::category::predicate};
    }
    utils::error{}("unsupported test script signature ({}, {})", return_type, scope);
  }

  devils_script::container compile_predicate(std::string_view name,
                                              tavl::parser& parser) const override {
    devils_script::container program;
    devils_script::system::parse_context ctx;
    sys.parse<bool, test_scope>(name, parser, ctx, program);
    return program;
  }

  devils_script::container compile_effect(std::string_view name,
                                           tavl::parser& parser) const override {
    devils_script::container program;
    devils_script::system::parse_context ctx;
    sys.parse<void, test_scope>(name, parser, ctx, program);
    return program;
  }
};
}

TEST_CASE("GOAP config inheritance preserves base order and overlays by semantic key [goap][config]") {
  goap_config base;
  base.metrics.push_back(goap_metric{"sense", {}, {}});
  base.metrics.push_back(goap_metric{"hungry", {}, {}});
  base.actions.push_back(goap_action_config{"walk", {}, {"done"}, {}, {}, false, {}});
  base.actions.push_back(goap_action_config{"eat", {"hungry"}, {"done"}, {}, {}, false, {}});
  base.goals.push_back(goap_goal_config{"done", {}, {"done"}});

  goap_config derived;
  derived.base = "base";
  derived.metrics.push_back(goap_metric{"hungry", {}, {}}); // replace in place
  derived.actions.push_back(goap_action_config{"eat", {"hungry", "sense"}, {"done"}, {}, {}, false, {}});
  derived.actions.push_back(goap_action_config{"sleep", {}, {"done"}, {}, {}, false, {}});
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

TEST_CASE("act script resource delegates return type and root scope to the injected compiler [act][config][demiurg]") {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "devils_engine_script_resource_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "scripts");
  {
    std::ofstream config(root / "core" / "scripts" / "ready.tavl");
    config << "ret = \"bool\"\n";
    config << "scope = \"test\"\n";
    config << "expr = \"true\"\n";
  }

  test_script_compiler scripts;
  demiurg::module_system modules(root.generic_string() + "/");
  modules.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});
  demiurg::resource_system resources;
  resources.register_type<act::script_resource>("scripts", "tavl", &scripts);
  resources.parse_resources(&modules);

  auto* resource = resources.get<act::script_resource>("scripts/ready");
  REQUIRE(resource != nullptr);
  resource->load(utils::safe_handle_t{});
  CHECK(resource->category() == act::category::predicate);
  CHECK_FALSE(resource->program()->cmds.empty());

  fs::remove_all(root);
}

TEST_CASE("GOAP resource resolves a single-parent config chain [goap][config][demiurg]") {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "devils_engine_goap_inheritance_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "goap");
  {
    std::ofstream base(root / "core" / "goap" / "base.tavl");
    base << "metrics = [ alive = true ]\n";
    base << "actions = [ { name = walk effect = config_effect requirements = [ alive ] next_state = [ done ] weight_state = [ ] } ]\n";
    base << "goals = [ { name = done requirements = [ ] goal = [ done ] } ]\n";
  }
  {
    std::ofstream derived(root / "core" / "goap" / "derived.tavl");
    derived << "base = base\n";
    derived << "disable_actions = [ walk ]\n";
    derived << "actions = [ { name = sleep requirements = [ alive ] next_state = [ done ] weight_state = [ ] } ]\n";
  }

  test_script_compiler scripts;
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

TEST_CASE("GOAP action co-parses an inline void effect program [goap][config][devils_script]") {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "devils_engine_goap_effect_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "goap");
  {
    std::ofstream config(root / "core" / "goap" / "scripted.tavl");
    config << "metrics = [ ready = true ]\n";
    config << "actions = [ { name = scripted effect = config_effect requirements = [ ready ] next_state = [ done ] weight_state = [ ] } ]\n";
    config << "goals = [ { name = done requirements = [ ] goal = [ done ] } ]\n";
  }

  test_script_compiler scripts;
  devils_engine::demiurg::module_system modules(root.generic_string() + "/");
  modules.load_modules({devils_engine::demiurg::module_system::list_entry{"core/", "", ""}});
  devils_engine::demiurg::resource_system resources;
  resources.register_type<goap_resource>("goap", "tavl", &scripts);
  resources.parse_resources(&modules);

  const auto flat = resolve_goap_config(resources, "scripted");
  REQUIRE(flat.actions.size() == 1);
  CHECK(flat.actions[0].name == "scripted");
  CHECK(flat.actions[0].has_effect_program);
  CHECK_FALSE(flat.actions[0].effect_program.cmds.empty());

  fs::remove_all(root);
}
