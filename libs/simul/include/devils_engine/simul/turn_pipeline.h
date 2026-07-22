#ifndef DEVILS_ENGINE_SIMUL_TURN_PIPELINE_H
#define DEVILS_ENGINE_SIMUL_TURN_PIPELINE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Resumable discrete-event pipeline scaffold.
//
// The project owns the concrete cursor and every gameplay coordinate (turns, player actions,
// countdown pulses, forced-drain generations, ...). The engine owns only control flow and the
// transient bridge to presentation. Keeping those concerns separate matters: a card can be a
// player action without advancing countdown, while a forced end-turn pulse advances countdown
// without being a player action.
//
// A visible gameplay beat normally has two presentation checkpoints:
//
//   start animation -> gameplay checkpoint -> deterministic commit/result
//                   -> finished checkpoint -> next gameplay step
//
// The render/flow side runs the animation and publishes checkpoint events. At the gameplay
// checkpoint the main thread computes the authoritative result and may send it back to render
// (damage numbers, impact reaction, new visual state). The pipeline advances to the next gameplay
// step only after the finished checkpoint.
//
// The host must move Cursor to the step that should run AFTER a barrier before returning wait.
// This makes save/resume well-defined: the barrier and in-flight animations are transient and are
// dropped on load, while Cursor resumes at the deterministic boundary exactly once.

namespace devils_engine {
namespace simul {

using presentation_task_id = uint64_t;

enum class presentation_event_kind : uint8_t {
  gameplay, // animation reached the point where main must compute the gameplay result
  finished  // animation (including result presentation) completed
};

struct presentation_event {
  presentation_task_id task = 0;
  presentation_event_kind kind = presentation_event_kind::gameplay;
  constexpr bool operator==(const presentation_event&) const noexcept = default;
};

// Transient set of presentation checkpoints the current pipeline step is waiting for.
// It is deliberately absent from snapshots: presentation is derived state.
class presentation_barrier {
public:
  void reset() noexcept {
    pending_.clear();
    opened_ = false;
    open_tick_ = 0;
  }

  void expect(const presentation_event event) {
    if (std::find(pending_.begin(), pending_.end(), event) == pending_.end()) {
      pending_.push_back(event);
    }
  }

  // Unknown/stale events are ignored. Returns true only when this checkpoint was pending.
  bool notify(const presentation_event event) noexcept {
    const auto it = std::find(pending_.begin(), pending_.end(), event);
    if (it == pending_.end()) {
      return false;
    }
    pending_.erase(it);
    return true;
  }

  bool resolved() const noexcept {
    return pending_.empty();
  }
  size_t pending_count() const noexcept {
    return pending_.size();
  }
  const std::vector<presentation_event>& pending() const noexcept {
    return pending_;
  }

  void mark_opened(const uint64_t engine_tick) noexcept {
    opened_ = true;
    open_tick_ = engine_tick;
  }

  // `budget` is measured in engine ticks; zero disables the watchdog.
  bool timed_out(const uint64_t now, const uint64_t budget) const noexcept {
    return opened_ && budget != 0 && !resolved() && now >= open_tick_ && now - open_tick_ > budget;
  }

private:
  std::vector<presentation_event> pending_;
  uint64_t open_tick_ = 0;
  bool opened_ = false;
};

enum class step_control : uint8_t {
  advance,     // run the cursor's next step immediately
  wait,        // wait for every checkpoint registered by this step
  yield_frame, // cursor advanced, but deliberately pace execution by one host frame
  halt         // stable rest point: player input, battle over, etc.
};

// Host contract:
//   step_control run_step(Cursor& cursor, turn_pipeline& pipe);
//   uint64_t barrier_budget() const;
//   void on_barrier_timeout(const Cursor&, const presentation_barrier&);
//
// `run_step` performs any synchronous deterministic work belonging to Cursor, moves Cursor to the
// next resumable boundary, registers expected presentation checkpoints, and returns a control.
// A host publishing work to another thread must register its checkpoint BEFORE publishing the task,
// otherwise a very fast response can race ahead of `expect_presentation` and be discarded as stale.
template <typename Cursor>
class turn_pipeline {
public:
  const Cursor& cursor() const noexcept {
    return cursor_;
  }
  Cursor& cursor() noexcept {
    return cursor_;
  }
  bool waiting() const noexcept {
    return waiting_;
  }
  bool faulted() const noexcept {
    return faulted_;
  }

  void expect_presentation(const presentation_task_id task, const presentation_event_kind kind) {
    barrier_.expect(presentation_event{task, kind});
  }
  bool notify_presentation(const presentation_task_id task, const presentation_event_kind kind) noexcept {
    return barrier_.notify(presentation_event{task, kind});
  }
  const presentation_barrier& barrier() const noexcept {
    return barrier_;
  }

  struct snapshot {
    Cursor cursor{};
  };

  snapshot save() const {
    return snapshot{cursor_};
  }

  void load(const snapshot& s) {
    cursor_ = s.cursor;
    waiting_ = false;
    faulted_ = false;
    barrier_.reset();
  }

  template <typename Host>
  void update(Host& host, const uint64_t engine_tick) {
    if (faulted_) {
      return;
    }

    for (;;) {
      if (waiting_) {
        if (!barrier_.resolved()) {
          if (barrier_.timed_out(engine_tick, host.barrier_budget())) {
            faulted_ = true; // latch: report once and stop driving a desynchronized pipeline
            host.on_barrier_timeout(cursor_, barrier_);
          }
          return;
        }
        waiting_ = false;
      }

      barrier_.reset();
      const step_control control = host.run_step(cursor_, *this);
      switch (control) {
        case step_control::advance:
          continue;
        case step_control::wait:
          // Headless steps register no checkpoints and therefore continue inline. Animated steps
          // return here until render/flow publishes every expected event.
          waiting_ = true;
          barrier_.mark_opened(engine_tick);
          continue;
        case step_control::yield_frame:
        case step_control::halt:
          return;
      }
      return;
    }
  }

private:
  Cursor cursor_{};
  presentation_barrier barrier_;
  bool waiting_ = false;
  bool faulted_ = false;
};

} // namespace simul
} // namespace devils_engine

#endif
