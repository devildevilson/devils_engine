#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "devils_engine/demiurg/resource_base.h"
#include "devils_engine/demiurg/resource_loader.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/module_interface.h"
#include "devils_engine/utils/safe_handle.h"

using namespace devils_engine;

namespace {

class staged_pipeline_resource final : public demiurg::resource_interface {
public:
  int read_config_count = 0;
  int compile_cpu_count = 0;
  int commit_gpu_count = 0;
  int release_gpu_count = 0;
  int unload_cpu_count = 0;

  staged_pipeline_resource() {
    set_flag(demiurg::resource_flags::warm_and_hot_same, false);
  }

  int32_t top_state() const override { return 3; }

  bool is_external_step(const int32_t from) const override {
    return from == 2;
  }

  void load_step(const int32_t from, const utils::safe_handle_t&) override {
    switch (from) {
      case 0: ++read_config_count; break;
      case 1: ++compile_cpu_count; break;
      case 2: ++commit_gpu_count; break;
      default: FAIL("unexpected load step");
    }
  }

  void unload_step(const int32_t from, const utils::safe_handle_t&) override {
    switch (from) {
      case 3: ++release_gpu_count; break;
      case 2: ++unload_cpu_count; break;
      default: break;
    }
  }

  void render_commit() {
    load(utils::safe_handle_t(this));
  }

  void render_release() {
    unload(utils::safe_handle_t(this));
  }

  void load_cold(const utils::safe_handle_t&) override {}
  void load_warm(const utils::safe_handle_t&) override {}
  void unload_warm(const utils::safe_handle_t&) override {}
  void unload_hot(const utils::safe_handle_t&) override {}
};

class cpu_ready_resource final : public demiurg::resource_interface {
public:
  int load_count = 0;
  int unload_count = 0;

  cpu_ready_resource() {
    set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  }

  void load_cold(const utils::safe_handle_t&) override { ++load_count; }
  void load_warm(const utils::safe_handle_t&) override {}
  void unload_warm(const utils::safe_handle_t&) override { ++unload_count; }
  void unload_hot(const utils::safe_handle_t&) override {}
};

class manifest_test_resource final : public demiurg::resource_interface {
public:
  std::string loaded_section;

  void load_cold(const utils::safe_handle_t&) override {
    loaded_section = list_section;
  }

  void load_warm(const utils::safe_handle_t&) override {}
  void unload_warm(const utils::safe_handle_t&) override {}
  void unload_hot(const utils::safe_handle_t&) override {}
};

class list_test_resource final : public demiurg::resource_interface {
public:
  void load_cold(const utils::safe_handle_t&) override {}
  void load_warm(const utils::safe_handle_t&) override {}
  void unload_warm(const utils::safe_handle_t&) override {}
  void unload_hot(const utils::safe_handle_t&) override {}
};

}

TEST_CASE("resource_system does not instantiate shadowed module resources [demiurg]") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_demiurg_shadow_test";
  fs::remove_all(root);
  fs::create_directories(root / "high" / "textures");
  fs::create_directories(root / "low" / "textures");

  {
    std::ofstream(root / "high" / "textures" / "grass.png").put('h');
    std::ofstream(root / "high" / "textures" / "grass.meta").put('m');
    std::ofstream(root / "low" / "textures" / "grass.png").put('l');
  }

  demiurg::module_system modules((root.generic_string() + "/"));
  modules.load_modules({
    demiurg::module_system::list_entry{"high/", "", ""},
    demiurg::module_system::list_entry{"low/", "", ""}
  });

  demiurg::resource_system resources;
  resources.register_type<manifest_test_resource>("textures", "png,meta");
  resources.parse_resources(&modules);

  auto* grass = resources.get("textures/grass");
  REQUIRE(grass != nullptr);
  CHECK(resources.resources_count() == 1);
  CHECK(resources.all_resources_count() == 2);
  CHECK(grass->path == "textures/grass.png");
  REQUIRE(grass->module != nullptr);
  CHECK(grass->module->path().find("/high/") != std::string_view::npos);

  auto* supplementary = grass->supplementary_next(grass);
  REQUIRE(supplementary != nullptr);
  CHECK(supplementary->path == "textures/grass.meta");
  CHECK(supplementary->module == grass->module);
  CHECK(supplementary->supplementary_next(grass) == nullptr);

  fs::remove_all(root);
}

TEST_CASE("resource_system expands tavl list resources and aliases indices [demiurg]") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_demiurg_list_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "configs");

  {
    std::ofstream out(root / "core" / "configs" / "abc.tavl");
    out << "name = abc1\n";
    out << "data = 123\n";
    out << "//---\n";
    out << "name = abc2\n";
    out << "data = 456\n";
  }

  demiurg::module_system modules((root.generic_string() + "/"));
  modules.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});

  demiurg::resource_system resources;
  resources.register_type<list_test_resource>("configs", "tavl");
  resources.parse_resources(&modules);

  auto* abc1 = resources.get<list_test_resource>("configs/abc:abc1");
  auto* abc2 = resources.get<list_test_resource>("configs/abc:abc2");
  REQUIRE(abc1 != nullptr);
  REQUIRE(abc2 != nullptr);
  CHECK(resources.get("configs/abc:0") == abc1);
  CHECK(resources.get("configs/abc:1") == abc2);
  CHECK(resources.get("configs/abc") == nullptr);
  CHECK(resources.resources_count() == 2);
  CHECK(resources.all_resources_count() == 2);

  CHECK(abc1->path == "configs/abc.tavl");
  CHECK(abc1->id == "configs/abc:abc1");
  CHECK(abc1->ext == "tavl");
  CHECK(abc1->is_list_entry());
  CHECK(abc1->list_index == 0);
  CHECK(abc1->list_start_line == 1);
  CHECK(abc1->list_name == "abc1");
  CHECK(abc1->list_section.find("data = 123") != std::string::npos);

  CHECK(abc2->list_index == 1);
  CHECK(abc2->list_start_line == 4);
  CHECK(abc2->list_name == "abc2");
  CHECK(abc2->list_section.find("data = 456") != std::string::npos);

  fs::remove_all(root);
}

TEST_CASE("resource_system applies tavl list partial overrides by name without moving index aliases [demiurg]") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_demiurg_list_override_test";
  fs::remove_all(root);
  fs::create_directories(root / "high" / "configs");
  fs::create_directories(root / "low" / "configs");

  {
    std::ofstream out(root / "low" / "configs" / "abc.tavl");
    out << "name = abc1\n";
    out << "data = 123\n";
    out << "//---\n";
    out << "name = abc2\n";
    out << "data = 456\n";
  }

  {
    std::ofstream out(root / "high" / "configs" / "abc.tavl");
    out << "name = abc2\n";
    out << "data = 999\n";
    out << "//---\n";
  }

  demiurg::module_system modules((root.generic_string() + "/"));
  modules.load_modules({
    demiurg::module_system::list_entry{"high/", "", ""},
    demiurg::module_system::list_entry{"low/", "", ""}
  });

  demiurg::resource_system resources;
  resources.register_type<list_test_resource>("configs", "tavl");
  resources.parse_resources(&modules);

  auto* abc1 = resources.get<list_test_resource>("configs/abc:abc1");
  auto* abc2 = resources.get<list_test_resource>("configs/abc:abc2");
  REQUIRE(abc1 != nullptr);
  REQUIRE(abc2 != nullptr);

  CHECK(resources.get("configs/abc:0") == abc1);
  CHECK(resources.get("configs/abc:1") == abc2);
  CHECK(abc1->module->path().find("/low/") != std::string_view::npos);
  CHECK(abc2->module->path().find("/high/") != std::string_view::npos);
  CHECK(abc2->list_index == 1);
  CHECK(abc2->list_start_line == 1);
  CHECK(abc2->list_name == "abc2");
  CHECK(abc2->list_section.find("data = 999") != std::string::npos);
  CHECK(resources.resources_count() == 2);
  CHECK(resources.all_resources_count() == 2);

  fs::remove_all(root);
}

TEST_CASE("resource_system uses empty tavl list sections as index holes [demiurg]") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_demiurg_list_holes_test";
  fs::remove_all(root);
  fs::create_directories(root / "high" / "configs");
  fs::create_directories(root / "low" / "configs");

  {
    std::ofstream out(root / "low" / "configs" / "abc.tavl");
    out << "data = 111\n";
    out << "//---\n";
    out << "data = 222\n";
    out << "//---\n";
    out << "data = 333\n";
  }

  {
    std::ofstream out(root / "high" / "configs" / "abc.tavl");
    out << "//---\n";
    out << "//---\n";
    out << "data = 999\n";
  }

  demiurg::module_system modules((root.generic_string() + "/"));
  modules.load_modules({
    demiurg::module_system::list_entry{"high/", "", ""},
    demiurg::module_system::list_entry{"low/", "", ""}
  });

  demiurg::resource_system resources;
  resources.register_type<list_test_resource>("configs", "tavl");
  resources.parse_resources(&modules);

  auto* abc0 = resources.get<list_test_resource>("configs/abc:0");
  auto* abc1 = resources.get<list_test_resource>("configs/abc:1");
  auto* abc2 = resources.get<list_test_resource>("configs/abc:2");
  REQUIRE(abc0 != nullptr);
  REQUIRE(abc1 != nullptr);
  REQUIRE(abc2 != nullptr);

  CHECK(abc0->module->path().find("/low/") != std::string_view::npos);
  CHECK(abc1->module->path().find("/low/") != std::string_view::npos);
  CHECK(abc2->module->path().find("/high/") != std::string_view::npos);
  CHECK(abc2->list_index == 2);
  CHECK(abc2->list_start_line == 3);
  CHECK(abc2->list_name.empty());
  CHECK(abc2->list_section.find("data = 999") != std::string::npos);
  CHECK(resources.get("configs/abc") == nullptr);
  CHECK(resources.resources_count() == 3);
  CHECK(resources.all_resources_count() == 3);

  fs::remove_all(root);
}

TEST_CASE("resource_system treats all-empty tavl list as list file without base resource [demiurg]") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_demiurg_empty_list_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "configs");

  {
    std::ofstream out(root / "core" / "configs" / "abc.tavl");
    out << "//---\n";
    out << "//---\n";
    out << "//---\n";
  }

  demiurg::module_system modules((root.generic_string() + "/"));
  modules.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});

  demiurg::resource_system resources;
  resources.register_type<list_test_resource>("configs", "tavl");
  resources.parse_resources(&modules);

  CHECK(resources.get("configs/abc") == nullptr);
  CHECK(resources.get("configs/abc:0") == nullptr);
  CHECK(resources.resources_count() == 0);
  CHECK(resources.all_resources_count() == 0);

  fs::remove_all(root);
}

TEST_CASE("resource_loader keeps pipeline GPU commit as render-owned external step [demiurg]") {
  staged_pipeline_resource pipeline;
  demiurg::resource_loader loader;
  std::vector<demiurg::resource_loader::external_job> jobs;

  loader.request(&pipeline, pipeline.final_state());

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 1);
  CHECK(pipeline.read_config_count == 1);
  CHECK(pipeline.compile_cpu_count == 0);
  CHECK(pipeline.commit_gpu_count == 0);

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 2);
  CHECK(pipeline.compile_cpu_count == 1);
  CHECK(pipeline.commit_gpu_count == 0);

  CHECK(loader.update(jobs) == 1);
  REQUIRE(jobs.size() == 1);
  CHECK(jobs[0].res == &pipeline);
  CHECK(jobs[0].load);
  CHECK(pipeline.state() == 2);
  CHECK(pipeline.commit_gpu_count == 0);

  jobs.clear();
  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 2);

  pipeline.render_commit();
  CHECK(pipeline.state() == 3);
  CHECK(pipeline.commit_gpu_count == 1);
  loader.external_done(&pipeline);

  CHECK(loader.update(jobs) == 0);
  CHECK(jobs.empty());
  CHECK(loader.pending_count() == 0);
}

TEST_CASE("resource_loader waits for dependencies before CPU-heavy pipeline prepare [demiurg]") {
  cpu_ready_resource shader_source;
  staged_pipeline_resource pipeline;
  pipeline.add_dependency(&shader_source);

  demiurg::resource_loader loader;
  std::vector<demiurg::resource_loader::external_job> jobs;

  loader.request(&pipeline, pipeline.final_state());

  CHECK(loader.update(jobs) == 2);
  CHECK(jobs.empty());
  CHECK(shader_source.usable());
  CHECK(shader_source.load_count == 1);
  CHECK(pipeline.state() == 0);
  CHECK(pipeline.read_config_count == 0);
  CHECK(pipeline.compile_cpu_count == 0);

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 1);
  CHECK(pipeline.read_config_count == 1);

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 2);
  CHECK(pipeline.compile_cpu_count == 1);
}
