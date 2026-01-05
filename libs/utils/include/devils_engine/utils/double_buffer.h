#ifndef DEVILS_ENGINE_UTILS_DOUBLE_BUFFER_H
#define DEVILS_ENGINE_UTILS_DOUBLE_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <shared_mutex>

// сверху такого буфера на самом деле есть еще одна система
// в которой последовательно вызываем функцию swap у всех буферов под единым shared_mutex
// возможно даже тут он не потребуется в таком случае
// в той системе нужно указать буферы и потребителей буферов

// нинужно

namespace devils_engine {
namespace utils {
struct swap_policy {
  template <typename T>
  void operator()(std::vector<T>& cur, std::vector<T>& staging) const {
    cur.resize(staging.size(), default_value);
    std::swap(cur, staging);
  }
};

struct copy_policy {
  template <typename T>
  void operator()(std::vector<T>& cur, std::vector<T>& staging) const {
    cur = staging;
  }
};

template <typename T, typename Policy = swap_policy>
class double_buffer {
public:
  double_buffer(const size_t initial_size, T default_value_) noexcept : default_value(std::move(default_value_)), cur(initial_size, default_value), staging(initial_size, default_value) {}

  void set(const size_t index, T val) noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    staging[index] = std::move(val);
  }

  void set(std::vector<T> val) noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    staging = std::move(val);
  }

  template <typename F>
  void call(const size_t index, const F& f) const {
    std::shared_lock<std::shared_mutex> l(mutex);
    const auto& val = index >= cur.size() ? default_value : cur[index];
    f(val);
  }

  template <typename F>
  void call(const F &f) const {
    std::shared_lock<std::shared_mutex> l(mutex);
    f(cur);
  }

  T get(const size_t index) const noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    if (index >= cur.size()) return default_value;
    return cur[index];
  }

  std::vector<T> get() const noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    return cur;
  }

  size_t size() const noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    return staging.size();
  }

  void swap() noexcept {
    std::lock_guard<std::shared_mutex> l(mutex);
    //cur.resize(staging.size(), default_value);
    //std::swap(cur, staging);
    Policy p;
    p(cur, staging);
  }

  void resize(const size_t size) noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    staging.resize(size, default_value);
  }

  void resize_all(const size_t size) noexcept {
    std::lock_guard<std::shared_mutex> l(mutex);
    cur.resize(size, default_value);
    staging.resize(size, default_value);
  }
private:
  mutable std::shared_mutex mutex;
  T default_value;
  std::vector<T> cur;
  std::vector<T> staging;
};

class size_buffer {
public:
  inline size_buffer(const uint32_t default_value_ = 0) noexcept : default_value(default_value_), cur_value(default_value_), staging_value(default_value_) {}

  inline uint32_t get() const noexcept {
    return cur_value;
  }

  inline uint32_t get_staging() const noexcept {
    return staging_value.load(std::memory_order::relaxed);
  }

  inline uint32_t inc_staging(const uint32_t val = 1) noexcept {
    return staging_value.fetch_add(val, std::memory_order::relaxed);
  }

  inline void set(const uint32_t val) noexcept {
    staging_value = val;
  }

  inline void swap() noexcept {
    cur_value = staging_value.load(std::memory_order::relaxed);
    staging_value = default_value;
  }
private:
  uint32_t default_value;
  uint32_t cur_value;
  std::atomic<uint32_t> staging_value;
};
}
}

#endif