#ifndef DEVILS_ENGINE_THREAD_LOCK_H
#define DEVILS_ENGINE_THREAD_LOCK_H

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <atomic>
#include <thread>

#include <immintrin.h>

namespace devils_engine {
namespace thread {
namespace semaphore_state {
enum values { unsignaled, signaled };
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
  inline bool wait_for(T dur, const size_t tolerance_in_ns = 1) const { return wait_until(clock::now() + dur, tolerance_in_ns); }
};

// ARM?
inline void cpu_relax() noexcept { _mm_pause(); }
inline void light_cpu_relax() noexcept { std::this_thread::yield(); }

class spin_mutex {
public: 
  static constexpr int max_backoff = 64;

  inline spin_mutex() noexcept : val(false) {}
  inline bool try_lock() noexcept { return !val.exchange(true, std::memory_order_acquire); }
  inline void lock() noexcept { 
    int backoff = 1;

    for (;;) {
      while (val.load(std::memory_order_relaxed)) { 
        for (int i = 0; i < backoff; ++i) cpu_relax(); 
      } 

      if (try_lock()) return;

      backoff = std::min(backoff * 2, max_backoff);
    }
  }

  inline void unlock() noexcept { val.store(false, std::memory_order_release); }
private:
  std::atomic_bool val;
};

class light_spin_mutex {
public: 
  inline light_spin_mutex() noexcept : val(false) {}
  inline bool try_lock() noexcept { return !val.exchange(true, std::memory_order_acquire); }
  inline void lock() noexcept {
    for (;;) {
      while (val.load(std::memory_order_relaxed)) { light_cpu_relax(); }
      if (try_lock()) return;
    }
  }
  inline void unlock() noexcept { val.store(false, std::memory_order_release); }
private:
  std::atomic_bool val;
};

template <typename T>
void spin_sleep_for(const T &dur) {
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
void spin_sleep_until(const T &tp) {
  const auto orig_tp = std::chrono::clock_cast<std::chrono::high_resolution_clock>(tp);
  int64_t ns = -1;
  while (ns < 0) {
    const auto new_dur = std::chrono::high_resolution_clock::now() - orig_tp;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();

    cpu_relax();
  }
}

template <typename T>
void light_spin_sleep_for(const T &dur) {
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
void light_spin_sleep_until(const T &tp) {
  const auto orig_tp = std::chrono::clock_cast<std::chrono::high_resolution_clock>(tp);
  int64_t ns = -1;
  while (ns < 0) {
    const auto new_dur = std::chrono::high_resolution_clock::now() - orig_tp;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(new_dur).count();

    light_cpu_relax();
  }
}
}
}

#endif