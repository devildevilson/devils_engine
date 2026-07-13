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

std::string_view to_string(runtime_stage stage) noexcept;

enum class app_state { boot,
                       loading,
                       game };

std::string_view to_string(app_state state) noexcept;

// Host contract:
//   on_lifecycle_enter(phase)
//   on_lifecycle_tick(phase, time)
//   lifecycle_phase_complete(phase) -> bool
//   on_lifecycle_leave(phase)
// The controller is the only writer of the current phase.
class lifecycle_controller {
public:
  app_state phase() const noexcept {
    return phase_;
  }
  bool started() const noexcept {
    return started_;
  }
  bool loading_requested() const noexcept {
    return loading_requested_;
  }

  bool request_loading() noexcept {
    if (!started_ || phase_ != app_state::game || loading_requested_) {
      return false;
    }
    loading_requested_ = true;
    return true;
  }

  template <typename Host>
  void start(Host& host) {
    if (started_) {
      return;
    }
    started_ = true;
    host.on_lifecycle_enter(phase_);
  }

  template <typename Host>
  void update(Host& host, const size_t time) {
    if (!started_) {
      start(host);
    }
    host.on_lifecycle_tick(phase_, time);

    if (phase_ == app_state::game) {
      if (!loading_requested_) {
        return;
      }
      loading_requested_ = false;
      transition(host, app_state::loading);
      return;
    }

    if (!host.lifecycle_phase_complete(phase_)) {
      return;
    }

    transition(host, phase_ == app_state::boot ? app_state::loading : app_state::game);
  }

private:
  template <typename Host>
  void transition(Host& host, const app_state next) {
    host.on_lifecycle_leave(phase_);
    phase_ = next;
    host.on_lifecycle_enter(phase_);
  }

  app_state phase_ = app_state::boot;
  bool started_ = false;
  bool loading_requested_ = false;
};

} // namespace simul
} // namespace devils_engine

#endif
