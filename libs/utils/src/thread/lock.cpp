#include <algorithm>

#include <immintrin.h>

#include "devils_engine/thread/lock.h"

namespace devils_engine {
namespace thread {
void cpu_relax() noexcept {
  _mm_pause();
}

void light_cpu_relax() noexcept {
  std::this_thread::yield();
}

spin_mutex::spin_mutex() noexcept : val(false) {}

bool spin_mutex::try_lock() noexcept {
  return !val.exchange(true, std::memory_order_acquire);
}

void spin_mutex::lock() noexcept {
  auto backoff = 1;

  for (;;) {
    while (val.load(std::memory_order_relaxed)) {
      for (auto i = 0; i < backoff; ++i) {
        cpu_relax();
      }
    }

    if (try_lock()) {
      return;
    }

    backoff = std::min(backoff * 2, max_backoff);
  }
}

void spin_mutex::unlock() noexcept {
  val.store(false, std::memory_order_release);
}

light_spin_mutex::light_spin_mutex() noexcept : val(false) {}

bool light_spin_mutex::try_lock() noexcept {
  return !val.exchange(true, std::memory_order_acquire);
}

void light_spin_mutex::lock() noexcept {
  for (;;) {
    while (val.load(std::memory_order_relaxed)) {
      light_cpu_relax();
    }

    if (try_lock()) {
      return;
    }
  }
}

void light_spin_mutex::unlock() noexcept {
  val.store(false, std::memory_order_release);
}
} // namespace thread
} // namespace devils_engine
