#ifndef DEVILS_ENGINE_SIMUL_LIFECYCLE_H
#define DEVILS_ENGINE_SIMUL_LIFECYCLE_H

#include <cstddef>
#include <string_view>

namespace devils_engine {
namespace simul {

// Synchronous boundaries owned by app_runtime. These stages describe construction and
// teardown; boot/loading/game below describe the asynchronous main-loop lifetime.
enum class runtime_stage {
  bootstrap_ready,
  systems_created,
  systems_initialized,
  workers_started,
  main_loop,
  workers_stopped
};

inline std::string_view to_string(const runtime_stage stage) noexcept {
  switch (stage) {
    case runtime_stage::bootstrap_ready: return "bootstrap_ready";
    case runtime_stage::systems_created: return "systems_created";
    case runtime_stage::systems_initialized: return "systems_initialized";
    case runtime_stage::workers_started: return "workers_started";
    case runtime_stage::main_loop: return "main_loop";
    case runtime_stage::workers_stopped: return "workers_stopped";
  }
  return "workers_stopped";
}

enum class app_state { boot, loading, game };

inline std::string_view to_string(const app_state state) noexcept {
  switch (state) {
    case app_state::boot: return "boot";
    case app_state::loading: return "loading";
    case app_state::game: return "game";
  }
  return "game";
}

// Host contract:
//   on_lifecycle_enter(phase)
//   on_lifecycle_tick(phase, time)
//   lifecycle_phase_complete(phase) -> bool
//   on_lifecycle_leave(phase)
// The controller is the only writer of the current phase.
class lifecycle_controller {
public:
  app_state phase() const noexcept { return phase_; }
  bool started() const noexcept { return started_; }

  template <typename Host>
  void start(Host& host) {
    if (started_) return;
    started_ = true;
    host.on_lifecycle_enter(phase_);
  }

  template <typename Host>
  void update(Host& host, const size_t time) {
    if (!started_) start(host);
    host.on_lifecycle_tick(phase_, time);
    if (phase_ == app_state::game || !host.lifecycle_phase_complete(phase_)) return;

    const app_state old = phase_;
    const app_state next = old == app_state::boot ? app_state::loading : app_state::game;
    host.on_lifecycle_leave(old);
    phase_ = next;
    host.on_lifecycle_enter(next);
  }

private:
  app_state phase_ = app_state::boot;
  bool started_ = false;
};

}
}

#endif
