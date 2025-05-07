#ifndef DEVILS_ENGINE_UTILS_INTERPOLATION_BUFFER_H
#define DEVILS_ENGINE_UTILS_INTERPOLATION_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <shared_mutex>

// так ли часто нужна интерполяция на хосте?
// ощущение что нужна она скорее в ГПУ
// и для ГПУ получается нужно завести 2 буфера + 3й на хосте  

namespace devils_engine {
namespace utils {
template <typename T>
class interpolation_buffer {
public:
  interpolation_buffer(const size_t initial_size, T default_value_) noexcept : default_value(std::move(default_value_)), cur(initial_size, default_value), staging(initial_size, default_value) {}

  void set(const size_t index, T val) noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    staging[index] = std::move(val);
  }

  T get_cur(const size_t index) const noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    if (index >= cur.size()) return default_value;
    return cur[index];
  }

  T get_prev(const size_t index) const noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    if (index >= prev.size()) return default_value;
    return prev[index];
  }

  std::tuple<T, T, double> get(const size_t index, const size_t timestamp) const noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    auto prev_value = index >= prev.size() ? default_value : prev[index];
    auto cur_value = index >= cur.size() ? default_value : cur[index];
    const auto diff = cur_timestamp - prev_timestamp;
    const auto diff2 = timestamp - prev_timestamp;
    const double k = double(diff2) / double(diff);
    return std::make_tuple(std::move(prev_value), std::move(cur_value), k);
  }

  template <typename F>
  T interpolate(const size_t index, const size_t timestamp, F f) const noexcept {
    auto [prev, cur, k] = get(index, timestamp);
    return f(std::move(prev), std::move(cur), k);
  }

  size_t size() const noexcept {
    std::shared_lock<std::shared_mutex> l(mutex);
    return staging.size();
  }

  void swap(const size_t timestamp) noexcept {
    std::lock_guard<std::shared_mutex> l(mutex);
    std::swap(prev, cur);
    std::swap(cur, staging);
    prev_timestamp = cur_timestamp;
    cur_timestamp = timestamp;
  }

  void resize(const size_t size) noexcept {
    std::lock_guard<std::shared_mutex> l(mutex);
    prev.resize(size, default_value);
    cur.resize(size, default_value);
    staging.resize(size, default_value);
  }
private:
  mutable std::shared_mutex mutex;
  T default_value;
  size_t prev_timestamp;
  size_t cur_timestamp;
  std::vector<T> prev;
  std::vector<T> cur;
  std::vector<T> staging;
};
}
}

#endif