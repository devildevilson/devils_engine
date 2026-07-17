#ifndef DEVILS_ENGINE_AESTHETICS_SYSTEM_RUNNER_H
#define DEVILS_ENGINE_AESTHETICS_SYSTEM_RUNNER_H

#include <concepts>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

#include "devils_engine/thread/atomic_pool.h"

namespace devils_engine {
namespace aesthetics {

// One indivisible pool task. This is useful for a serial gather/reduce which may
// nevertheless run concurrently with other independent systems in the phase.
template <typename Fn>
class single_task_t {
public:
  explicit single_task_t(Fn fn) noexcept(std::is_nothrow_move_constructible_v<Fn>)
    : fn_(std::move(fn)) {}

  void enqueue(thread::atomic_pool& pool, const size_t time) {
    // run() waits before returning, so referring to this adapter (including a
    // temporary returned by single()) is valid for the whole task lifetime.
    pool.submit(
      [](single_task_t* self, const size_t tick) {
        if constexpr (std::invocable<Fn&, size_t>) {
          std::invoke(self->fn_, tick);
        } else {
          static_assert(std::invocable<Fn&>, "aesthetics::single callable must accept size_t or no arguments");
          std::invoke(self->fn_);
        }
      },
      this, time);
  }

private:
  Fn fn_;
};

template <typename Fn>
auto single(Fn&& fn) {
  return single_task_t<std::decay_t<Fn>>(std::forward<Fn>(fn));
}

namespace detail {
template <typename System>
concept enqueue_system = requires(System& system, thread::atomic_pool& pool, const size_t time) {
  system.enqueue(pool, time);
};

template <typename System>
concept range_system = requires(System& system, const size_t start, const size_t count, const size_t time) {
  { system.size() } -> std::convertible_to<size_t>;
  system.process_range(start, count, time);
};

template <typename System>
concept update_system = requires(System& system, const size_t time) {
  system.update(time);
};

template <typename>
inline constexpr bool dependent_false_v = false;

template <typename System>
void enqueue_one(thread::atomic_pool& pool, const size_t time, System& system) {
  if constexpr (enqueue_system<System>) {
    system.enqueue(pool, time);
  } else if constexpr (range_system<System>) {
    pool.distribute1(
      system.size(),
      [](const size_t start, const size_t count, System* system, const size_t tick) {
        system->process_range(start, count, tick);
      },
      &system, time);
  } else if constexpr (update_system<System>) {
    // A plain serial system is still a pool task. This lets several independent
    // gather/build systems overlap without teaching them about the pool.
    pool.submit(
      [](System* system, const size_t tick) {
        system->update(tick);
      },
      &system, time);
  } else {
    static_assert(dependent_false_v<System>,
                  "aesthetics::run expects enqueue(pool,time), process_range, or update(time)");
  }
}
} // namespace detail

// Submit every independent system into one phase, then join them behind one
// barrier. Workers may start earlier submissions while the phase is still being
// formed, so arguments must not depend on one another's ordering.
template <typename... Systems>
void run(thread::atomic_pool& pool, const size_t time, Systems&&... systems) {
  static_assert(sizeof...(Systems) > 0, "aesthetics::run requires at least one system");
  (detail::enqueue_one(pool, time, systems), ...);
  pool.compute(); // the calling thread participates in the shared phase
  pool.wait();    // exactly one barrier for all systems in this run()
}

} // namespace aesthetics
} // namespace devils_engine

#endif
