#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/app_runtime.h>
#include <devils_engine/simul/boot_config.h>
#include <devils_engine/simul/lifecycle.h>
#include <devils_engine/simul/pause.h>
#include <devils_engine/simul/startup_resources.h>
#include <devils_engine/utils/safe_handle.h>
#include <doctest/doctest.h>

namespace {

using devils_engine::simul::app_state;

struct lifecycle_host {
  std::vector<app_state> entered;
  std::vector<app_state> left;
  std::vector<app_state> ticked;
  bool boot_ready = false;
  bool loading_ready = false;

  void on_lifecycle_enter(const app_state phase) {
    entered.push_back(phase);
  }
  void on_lifecycle_tick(const app_state phase, const size_t) {
    ticked.push_back(phase);
  }
  bool lifecycle_phase_complete(const app_state phase) const {
    if (phase == app_state::boot) {
      return boot_ready;
    }
    if (phase == app_state::loading) {
      return loading_ready;
    }
    return false;
  }
  void on_lifecycle_leave(const app_state phase) {
    left.push_back(phase);
  }
};

struct runtime_test_broker {};

struct runtime_test_state {
  std::vector<devils_engine::simul::runtime_stage> stages;
  bool main_initialized = false;
  bool worker_initialized = false;
  bool worker_discovered = false;
  bool optional_worker_absent = false;
  bool workers_started = false;
  std::atomic_size_t main_updates = 0;
};

struct runtime_test_bootstrap {
  devils_engine::simul::engine_boot_config engine;
  struct settings_type {} settings;
  runtime_test_state state;
};

class runtime_test_worker : public devils_engine::simul::brokered_advancer<runtime_test_broker> {
public:
  explicit runtime_test_worker(runtime_test_state* state) : state_(state) {}

  void init() override {
    state_->worker_initialized = broker() != nullptr;
  }
  bool stop_predicate() const override {
    return false;
  }
  void update(const size_t) override {}

private:
  runtime_test_state* state_;
};

class omitted_runtime_worker : public devils_engine::simul::brokered_advancer<runtime_test_broker> {
public:
  void init() override {}
  bool stop_predicate() const override {
    return false;
  }
  void update(const size_t) override {}
};

class runtime_test_main : public devils_engine::simul::main_system<runtime_test_broker> {
public:
  explicit runtime_test_main(runtime_test_bootstrap* boot) : boot_(boot) {}

  void init() override {
    auto& state = boot_->state;
    state.main_initialized = broker() != nullptr;
    state.worker_discovered = runtime_system<runtime_test_worker>() != nullptr;
    state.optional_worker_absent = runtime_system<omitted_runtime_worker>() == nullptr;
  }
  bool stop_predicate() const override {
    return stop_requested_;
  }
  void update(const size_t) override {
    boot_->state.main_updates.fetch_add(1, std::memory_order_relaxed);
  }
  void workers_started() override {
    boot_->state.workers_started = true;
    stop_requested_ = true;
  }
  int exit_code() const noexcept override {
    return 17;
  }

private:
  runtime_test_bootstrap* boot_;
  bool stop_requested_ = false;
};

struct runtime_test_traits {
  using bootstrap_type = runtime_test_bootstrap;
  using broker_type = runtime_test_broker;
  using main_type = runtime_test_main;

  static void init_bootstrap(bootstrap_type&) {}

  static devils_engine::simul::worker_systems<broker_type> make_workers(bootstrap_type& boot) {
    devils_engine::simul::worker_systems<broker_type> workers;
    workers.add(std::make_unique<runtime_test_worker>(&boot.state));
    return workers;
  }

  static void runtime_stage_changed(
    const devils_engine::simul::runtime_stage stage,
    devils_engine::simul::app_runtime<runtime_test_traits>& runtime) {
    runtime.bootstrap()->state.stages.push_back(stage);
  }
};

} // namespace

TEST_CASE("app_runtime wires an extensible optional worker set") {
  using devils_engine::simul::runtime_stage;

  devils_engine::simul::app_runtime<runtime_test_traits> runtime;
  CHECK(runtime.run() == 17);

  const auto& state = runtime.bootstrap()->state;
  CHECK(state.main_initialized);
  CHECK(state.worker_initialized);
  CHECK(state.worker_discovered);
  CHECK(state.optional_worker_absent);
  CHECK(state.workers_started);
  CHECK(state.main_updates.load(std::memory_order_relaxed) == 0);
  CHECK(state.stages == std::vector<runtime_stage>{
                          runtime_stage::bootstrap_ready,
                          runtime_stage::systems_created,
                          runtime_stage::systems_initialized,
                          runtime_stage::workers_started,
                          runtime_stage::main_loop,
                          runtime_stage::workers_stopped,
                        });
}

TEST_CASE("pause_state keeps gameplay and presentation as independent engine domains") {
  using namespace devils_engine::simul;
  pause_state pause;
  CHECK_FALSE(pause.paused(pause_domain::gameplay));
  CHECK_FALSE(pause.paused(pause_domain::presentation));
  pause.set(pause_domain::gameplay, true);
  CHECK(pause.paused(pause_domain::gameplay));
  CHECK_FALSE(pause.paused(pause_domain::presentation));
  pause.set_world(true);
  CHECK(pause.world_paused());
  pause.set(pause_domain::presentation, false);
  CHECK_FALSE(pause.world_paused());
  CHECK(pause.paused(pause_domain::gameplay));
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
  modules.load_modules({demiurg::module_system::list_entry{"mod/", "", ""},
                        demiurg::module_system::list_entry{"core/", "", ""}});

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
                                    "ui/mod_entry", "fonts/mod", "textures/mod_bg"});
  CHECK(ui->config().scene == "scenes/mod_scene");

  fs::remove_all(root);
}
