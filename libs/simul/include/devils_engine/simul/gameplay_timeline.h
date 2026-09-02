#ifndef DEVILS_ENGINE_SIMUL_GAMEPLAY_TIMELINE_H
#define DEVILS_ENGINE_SIMUL_GAMEPLAY_TIMELINE_H

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <devils_engine/utils/simulation_time.h>

namespace devils_engine::simul {

enum class schedule_result : uint8_t {
  scheduled,
  duplicate,
  past_tick,
  capacity_exceeded
};

template <class Payload, class Key = uint64_t>
struct scheduled_gameplay_event {
  utils::simulation_tick at{};
  Key source{};
  uint32_t ordinal = 0;
  Payload payload{};

  constexpr bool operator==(const scheduled_gameplay_event&) const = default;
};

// Single-owner bounded min-heap. It contains data, not callbacks, and returns
// every event crossed by advance_to() in canonical (tick, source, ordinal)
// order. Render/presentation code has no path into this queue.
template <class Payload, class Key = uint64_t>
requires std::regular<Payload> && std::regular<Key> && std::totally_ordered<Key>
class gameplay_timeline {
public:
  using event_type = scheduled_gameplay_event<Payload, Key>;

  struct snapshot {
    utils::simulation_tick current{};
    std::vector<event_type> pending;

    bool operator==(const snapshot&) const = default;
  };

  explicit gameplay_timeline(const size_t capacity,
                             const utils::simulation_tick start = {})
    : current_(start), capacity_(capacity) {
    heap_.reserve(capacity);
  }

  utils::simulation_tick current_tick() const noexcept {
    return current_;
  }

  size_t capacity() const noexcept {
    return capacity_;
  }

  size_t pending_count() const noexcept {
    return heap_.size();
  }

  schedule_result try_schedule(const event_type& event) {
    return try_schedule_impl(event);
  }

  schedule_result try_schedule(event_type&& event) {
    return try_schedule_impl(std::move(event));
  }

  std::vector<event_type> advance_to(const utils::simulation_tick now) {
    if (now < current_) {
      throw std::invalid_argument("gameplay timeline cannot move backwards");
    }

    std::vector<event_type> due;
    // Allocation is the only fallible preparation. Do it before changing the
    // causal clock or removing events, so a failed advance is retryable.
    due.reserve(heap_.size());
    current_ = now;
    while (!heap_.empty() && heap_.front().at <= now) {
      std::pop_heap(heap_.begin(), heap_.end(), heap_later{});
      due.push_back(std::move(heap_.back()));
      heap_.pop_back();
    }
    std::sort(due.begin(), due.end(), event_before{});
    return due;
  }

  std::vector<event_type> pending_ordered() const {
    auto out = heap_;
    std::sort(out.begin(), out.end(), event_before{});
    return out;
  }

  snapshot save() const {
    return snapshot{current_, pending_ordered()};
  }

  // Transactional replacement: validation/build happens in temporary storage.
  void load(const snapshot& value) {
    if (value.pending.size() > capacity_) {
      throw std::length_error("gameplay timeline snapshot exceeds capacity");
    }

    std::vector<event_type> prepared;
    prepared.reserve(capacity_);
    for (const auto& event : value.pending) {
      if (event.at < value.current) {
        throw std::invalid_argument("gameplay timeline snapshot contains a past event");
      }
      if (contains_identity(prepared, event)) {
        throw std::invalid_argument("gameplay timeline snapshot contains a duplicate event");
      }
      prepared.push_back(event);
    }
    std::make_heap(prepared.begin(), prepared.end(), heap_later{});

    heap_.swap(prepared);
    current_ = value.current;
  }

private:
  struct event_before {
    constexpr bool operator()(const event_type& left, const event_type& right) const {
      if (left.at != right.at) return left.at < right.at;
      if (left.source != right.source) return left.source < right.source;
      return left.ordinal < right.ordinal;
    }
  };

  struct heap_later {
    constexpr bool operator()(const event_type& left, const event_type& right) const {
      return event_before{}(right, left);
    }
  };

  static bool same_identity(const event_type& left, const event_type& right) {
    return left.source == right.source && left.ordinal == right.ordinal;
  }

  static bool contains_identity(const std::span<const event_type> events,
                                const event_type& candidate) {
    return std::any_of(events.begin(), events.end(), [&](const event_type& event) {
      return same_identity(event, candidate);
    });
  }

  template <class Event>
  schedule_result try_schedule_impl(Event&& event) {
    if (event.at < current_) return schedule_result::past_tick;
    if (contains_identity(heap_, event)) return schedule_result::duplicate;
    if (heap_.size() == capacity_) return schedule_result::capacity_exceeded;

    heap_.push_back(std::forward<Event>(event));
    std::push_heap(heap_.begin(), heap_.end(), heap_later{});
    return schedule_result::scheduled;
  }

  std::vector<event_type> heap_;
  utils::simulation_tick current_{};
  size_t capacity_ = 0;
};

} // namespace devils_engine::simul

#endif
