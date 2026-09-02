#ifndef DEVILS_ENGINE_SIMUL_TURN_PIPELINE_H
#define DEVILS_ENGINE_SIMUL_TURN_PIPELINE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "gameplay_timeline.h"

// Resumable discrete-event pipeline scaffold. Visible animation and causal
// animation markers are deliberately separate: the project may publish a
// presentation command, but only this main-thread tick timeline releases the
// gameplay marker which advances the cursor.

namespace devils_engine::simul {

using animation_task_id = uint64_t;

enum class animation_event_kind : uint8_t {
  gameplay,
  recovery_finished
};

struct animation_event {
  animation_task_id task = 0;
  animation_event_kind kind = animation_event_kind::gameplay;
  constexpr bool operator==(const animation_event&) const noexcept = default;
};

class gameplay_barrier {
public:
  explicit gameplay_barrier(const size_t capacity)
    : capacity_(capacity) {
    pending_.reserve(capacity);
  }

  void reset() noexcept {
    pending_.clear();
  }

  void expect(const animation_event event) {
    if (std::find(pending_.begin(), pending_.end(), event) != pending_.end()) return;
    if (pending_.size() == capacity_) {
      throw std::length_error("gameplay barrier capacity exceeded");
    }
    pending_.push_back(event);
  }

  bool notify(const animation_event event) noexcept {
    const auto it = std::find(pending_.begin(), pending_.end(), event);
    if (it == pending_.end()) return false;
    pending_.erase(it);
    return true;
  }

  bool resolved() const noexcept {
    return pending_.empty();
  }

  size_t pending_count() const noexcept {
    return pending_.size();
  }

  const std::vector<animation_event>& pending() const noexcept {
    return pending_;
  }

  void load(const std::span<const animation_event> pending) {
    if (pending.size() > capacity_) {
      throw std::length_error("gameplay barrier snapshot exceeds capacity");
    }
    pending_.assign(pending.begin(), pending.end());
  }

private:
  std::vector<animation_event> pending_;
  size_t capacity_ = 0;
};

enum class step_control : uint8_t {
  advance,
  wait,
  yield_frame,
  halt
};

// Host contract:
//   step_control run_step(Cursor& cursor, turn_pipeline& pipe);
//
// Before publishing an animation command, run_step registers its causal marker
// through expect_animation_at/after. The render thread receives only the
// command and never calls back into this pipeline.
template <typename Cursor>
class turn_pipeline {
public:
  static_assert(std::is_nothrow_swappable_v<Cursor>,
                "turn pipeline cursor must support non-throwing staged replacement");

  using timeline_type = gameplay_timeline<animation_event_kind, animation_task_id>;

  struct snapshot {
    Cursor cursor{};
    typename timeline_type::snapshot timeline;
    std::vector<animation_event> pending;
    bool waiting = false;
    bool faulted = false;
    bool operator==(const snapshot&) const = default;
  };

  explicit turn_pipeline(const size_t event_capacity)
    : timeline_(event_capacity), barrier_(event_capacity) {}

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

  utils::simulation_tick current_tick() const noexcept {
    return timeline_.current_tick();
  }

  const gameplay_barrier& barrier() const noexcept {
    return barrier_;
  }

  void expect_animation_at(const animation_task_id task,
                           const animation_event_kind kind,
                           const utils::simulation_tick at) {
    const auto result = timeline_.try_schedule({at, task, uint32_t(kind), kind});
    switch (result) {
      case schedule_result::scheduled:
        try {
          barrier_.expect({task, kind});
        } catch (...) {
          faulted_ = true;
          throw;
        }
        return;
      case schedule_result::duplicate:
        faulted_ = true;
        throw std::invalid_argument("turn pipeline duplicate animation event");
      case schedule_result::past_tick:
        faulted_ = true;
        throw std::invalid_argument("turn pipeline animation event is in the past");
      case schedule_result::capacity_exceeded:
        faulted_ = true;
        throw std::length_error("turn pipeline animation event capacity exceeded");
    }
  }

  void expect_animation_after(const animation_task_id task,
                              const animation_event_kind kind,
                              const utils::simulation_duration delay) {
    expect_animation_at(task, kind, timeline_.current_tick() + delay);
  }

  snapshot save() const {
    return snapshot{cursor_, timeline_.save(), barrier_.pending(), waiting_, faulted_};
  }

  void load(const snapshot& value) {
    Cursor prepared_cursor = value.cursor;
    timeline_type candidate(timeline_.capacity());
    candidate.load(value.timeline);

    const auto scheduled = candidate.pending_ordered();
    if (scheduled.size() != value.pending.size()) {
      throw std::invalid_argument("turn pipeline snapshot barrier/timeline size mismatch");
    }
    if (!scheduled.empty() && !value.waiting) {
      throw std::invalid_argument("turn pipeline snapshot has events without a wait");
    }
    for (size_t i = 0; i < value.pending.size(); ++i) {
      const auto& event = value.pending[i];
      if (std::find(value.pending.begin(), value.pending.begin() + i, event) !=
          value.pending.begin() + i) {
        throw std::invalid_argument("turn pipeline snapshot contains a duplicate barrier event");
      }
      const auto match = std::find_if(scheduled.begin(), scheduled.end(),
        [&](const typename timeline_type::event_type& queued) {
          return queued.source == event.task &&
                 queued.ordinal == uint32_t(event.kind) &&
                 queued.payload == event.kind;
        });
      if (match == scheduled.end()) {
        throw std::invalid_argument("turn pipeline snapshot barrier/timeline event mismatch");
      }
    }

    using std::swap;
    swap(cursor_, prepared_cursor);
    timeline_ = std::move(candidate);
    barrier_.load(value.pending);
    waiting_ = value.waiting;
    faulted_ = value.faulted;
  }

  template <typename Host>
  void update(Host& host, const utils::simulation_tick tick) {
    if (faulted_) return;

    for (;;) {
      for (const auto& due : timeline_.advance_to(tick)) {
        if (!barrier_.notify({due.source, due.payload})) {
          faulted_ = true;
          throw std::logic_error("turn pipeline released an event outside its barrier");
        }
      }

      if (waiting_) {
        if (!barrier_.resolved()) return;
        waiting_ = false;
      }

      barrier_.reset();
      const step_control control = host.run_step(cursor_, *this);
      if (control != step_control::wait && !barrier_.resolved()) {
        faulted_ = true;
        throw std::logic_error("turn pipeline host scheduled an event without waiting");
      }
      switch (control) {
        case step_control::advance:
          continue;
        case step_control::wait:
          waiting_ = true;
          continue;
        case step_control::yield_frame:
        case step_control::halt:
          return;
      }
    }
  }

private:
  Cursor cursor_{};
  timeline_type timeline_;
  gameplay_barrier barrier_;
  bool waiting_ = false;
  bool faulted_ = false;
};

} // namespace devils_engine::simul

#endif
