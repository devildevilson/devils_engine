#ifndef DEVILS_ENGINE_THREAD_LOCK_H
#define DEVILS_ENGINE_THREAD_LOCK_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace devils_engine {
namespace thread {
namespace semaphore_state {
enum values { unsignaled,
              signaled };
}

class semaphore_interface {
public:
  using clock = std::chrono::high_resolution_clock;
  using tp = clock::time_point;

  virtual ~semaphore_interface() noexcept = default;
  virtual void reset() = 0;
  virtual semaphore_state::values state() const = 0;
  virtual bool wait_until(tp t, const size_t tolerance_in_ns = 1) const = 0;
  template <typename T>
  bool wait_for(T dur, const size_t tolerance_in_ns = 1) const;
};

// ARM?
void cpu_relax() noexcept;
void light_cpu_relax() noexcept;

class spin_mutex {
public:
  static constexpr int max_backoff = 64;

  spin_mutex() noexcept;
  bool try_lock() noexcept;
  void lock() noexcept;
  void unlock() noexcept;

private:
  std::atomic_bool val;
};

class light_spin_mutex {
public:
  light_spin_mutex() noexcept;
  bool try_lock() noexcept;
  void lock() noexcept;
  void unlock() noexcept;

private:
  std::atomic_bool val;
};

template <typename T>
bool semaphore_interface::wait_for(T dur, const size_t tolerance_in_ns) const {
  return wait_until(clock::now() + dur, tolerance_in_ns);
}

template <typename T>
void spin_sleep_for(const T& dur) {
  const size_t amount = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
  const auto tp = std::chrono::high_resolution_clock::now();
  size_t ns = 0;
  while (ns < amount) {
    const auto new_dur = std::chrono::high_resolution_clock::now() - tp;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();

    cpu_relax();
  }
}

template <typename T>
void spin_sleep_until(const T& tp) {
  const auto orig_tp = std::chrono::clock_cast<std::chrono::high_resolution_clock>(tp);
  int64_t ns = -1;
  while (ns < 0) {
    const auto new_dur = std::chrono::high_resolution_clock::now() - orig_tp;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();

    cpu_relax();
  }
}

template <typename T>
void light_spin_sleep_for(const T& dur) {
  const size_t amount = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
  const auto tp = std::chrono::high_resolution_clock::now();
  size_t ns = 0;
  while (ns < amount) {
    const auto new_dur = std::chrono::high_resolution_clock::now() - tp;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();

    light_cpu_relax();
  }
}

template <typename T>
void light_spin_sleep_until(const T& tp) {
  const auto orig_tp = std::chrono::clock_cast<std::chrono::high_resolution_clock>(tp);
  int64_t ns = -1;
  while (ns < 0) {
    const auto new_dur = std::chrono::high_resolution_clock::now() - orig_tp;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();

    light_cpu_relax();
  }
}
} // namespace thread
} // namespace devils_engine

#endif
