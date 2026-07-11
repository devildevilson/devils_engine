#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

#include <devils_engine/simul/lifecycle.h>

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
}
