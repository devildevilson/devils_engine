#ifndef DEVILS_ENGINE_THREAD_SPSC_QUEUE_H
#define DEVILS_ENGINE_THREAD_SPSC_QUEUE_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace devils_engine {
namespace thread {

template <typename T>
class spsc_queue {
public:
  explicit spsc_queue(const size_t capacity_) : buffer(capacity_) {
    if (capacity_ == 0) throw std::invalid_argument("spsc_queue capacity must be greater than zero");
  }

  ~spsc_queue() noexcept { clear(); }

  spsc_queue(const spsc_queue&) = delete;
  spsc_queue(spsc_queue&&) = delete;
  spsc_queue& operator=(const spsc_queue&) = delete;
  spsc_queue& operator=(spsc_queue&&) = delete;

  bool try_push(const T& value) {
    return try_push(std::span<const T>(&value, 1)) == 1;
  }

  bool try_push(T&& value) {
    const uint64_t tail_pos = tail.load(std::memory_order_relaxed);
    const uint64_t head_pos = head.load(std::memory_order_acquire);
    if (tail_pos - head_pos >= buffer.size()) return false;

    new (slot(tail_pos)) T(std::move(value));
    tail.store(tail_pos + 1, std::memory_order_release);
    return true;
  }

  size_t try_push(const std::span<const T> values) {
    const uint64_t tail_pos = tail.load(std::memory_order_relaxed);
    const uint64_t head_pos = head.load(std::memory_order_acquire);
    const size_t available = buffer.size() - static_cast<size_t>(tail_pos - head_pos);
    const size_t count = std::min(values.size(), available);
    if (count == 0) return 0;

    const size_t first_index = static_cast<size_t>(tail_pos % buffer.size());
    const size_t first_count = std::min(count, buffer.size() - first_index);
    copy_construct(tail_pos, values.first(first_count));

    const size_t second_count = count - first_count;
    if (second_count != 0) {
      copy_construct(tail_pos + first_count, values.subspan(first_count, second_count));
    }

    tail.store(tail_pos + count, std::memory_order_release);
    return count;
  }

  template <typename... Args>
  bool emplace(Args&&... args) {
    const uint64_t tail_pos = tail.load(std::memory_order_relaxed);
    const uint64_t head_pos = head.load(std::memory_order_acquire);
    if (tail_pos - head_pos >= buffer.size()) return false;

    new (slot(tail_pos)) T(std::forward<Args>(args)...);
    tail.store(tail_pos + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    return try_pop(std::span<T>(&out, 1)) == 1;
  }

  size_t try_pop(const std::span<T> out) {
    const uint64_t head_pos = head.load(std::memory_order_relaxed);
    const uint64_t tail_pos = tail.load(std::memory_order_acquire);
    const size_t available = static_cast<size_t>(tail_pos - head_pos);
    const size_t count = std::min(out.size(), available);
    if (count == 0) return 0;

    const size_t first_index = static_cast<size_t>(head_pos % buffer.size());
    const size_t first_count = std::min(count, buffer.size() - first_index);
    move_destroy(head_pos, out.first(first_count));

    const size_t second_count = count - first_count;
    if (second_count != 0) {
      move_destroy(head_pos + first_count, out.subspan(first_count, second_count));
    }

    head.store(head_pos + count, std::memory_order_release);
    return count;
  }

  void clear() noexcept {
    uint64_t head_pos = head.load(std::memory_order_relaxed);
    const uint64_t tail_pos = tail.load(std::memory_order_relaxed);
    while (head_pos != tail_pos) {
      slot(head_pos)->~T();
      ++head_pos;
    }

    head.store(tail_pos, std::memory_order_relaxed);
  }

  bool empty() const noexcept {
    return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
  }

  bool full() const noexcept {
    const uint64_t head_pos = head.load(std::memory_order_acquire);
    const uint64_t tail_pos = tail.load(std::memory_order_acquire);
    return tail_pos - head_pos >= buffer.size();
  }

  size_t size_approx() const noexcept {
    const uint64_t head_pos = head.load(std::memory_order_acquire);
    const uint64_t tail_pos = tail.load(std::memory_order_acquire);
    return static_cast<size_t>(tail_pos - head_pos);
  }

  size_t capacity() const noexcept { return buffer.size(); }

private:
  struct storage_t {
    alignas(T) std::byte data[sizeof(T)];
  };

  T* slot(const uint64_t pos) noexcept {
    return std::launder(reinterpret_cast<T*>(buffer[static_cast<size_t>(pos % buffer.size())].data));
  }

  const T* slot(const uint64_t pos) const noexcept {
    return std::launder(reinterpret_cast<const T*>(buffer[static_cast<size_t>(pos % buffer.size())].data));
  }

  void copy_construct(const uint64_t start, const std::span<const T> values) {
    for (size_t i = 0; i < values.size(); ++i) {
      new (slot(start + i)) T(values[i]);
    }
  }

  void move_destroy(const uint64_t start, const std::span<T> out) {
    for (size_t i = 0; i < out.size(); ++i) {
      T* value = slot(start + i);
      out[i] = std::move(*value);
      value->~T();
    }
  }

  std::vector<storage_t> buffer;
  alignas(64) std::atomic<uint64_t> head = 0;
  alignas(64) std::atomic<uint64_t> tail = 0;
};

}
}

#endif
