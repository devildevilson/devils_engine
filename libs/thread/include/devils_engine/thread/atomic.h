#ifndef DEVILS_ENGINE_THREAD_ATOMIC_H
#define DEVILS_ENGINE_THREAD_ATOMIC_H

#include <cstddef>
#include <cstdint>
#include <atomic>

namespace devils_engine {
namespace thread {
template <typename T>
void atomic_max(std::atomic<T> &maximum_value, const T &value) noexcept {
  T prev_value = maximum_value;
  while (prev_value < value && !maximum_value.compare_exchange_weak(prev_value, value));
}
    
template <typename T>
void atomic_min(std::atomic<T> &maximum_value, const T &value) noexcept {
  T prev_value = maximum_value;
  while (prev_value > value && !maximum_value.compare_exchange_weak(prev_value, value));
}
}
}

#endif