#ifndef DEVILS_ENGINE_THREAD_ATOMIC_QUEUE_H
#define DEVILS_ENGINE_THREAD_ATOMIC_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <array>

namespace devils_engine {
namespace thread {

template <typename T, size_t N>
class queue1 {
public:
  queue1() noexcept;
  // может быть лишняя копия, но так мы можем потерять value если вдруг вернется false
  bool enqueue(const T& value) noexcept;
  bool dequeue(T& value) noexcept;
private:
  struct cell_t {
    std::atomic<uint64_t> seq;
    T data;
  };

  std::array<cell_t, N> buffer;
  std::atomic<uint64_t> head;
  std::atomic<uint64_t> tail;
};

// конструктор у атомиков НЕ атомарный...
template <typename T, size_t N>
queue1<T, N>::queue1() noexcept {
  for (size_t i = 0; i < buffer.size(); ++i) {
    buffer[i].seq.store(i, std::memory_order_relaxed);
  }

  head.store(0, std::memory_order_relaxed);
  tail.store(0, std::memory_order_relaxed);
}

template <typename T, size_t N>
bool queue1<T, N>::enqueue(const T& v) noexcept {
  uint64_t pos = tail.load(std::memory_order_relaxed);

  while (true) {
    const size_t index = pos % buffer.size();
    auto& cell = buffer[index];
    uint64_t seq = cell.seq.load(std::memory_order_acquire);
    auto dif = int64_t(seq) - int64_t(pos);

    if (dif < 0) return false; // полная очередь

    if (dif > 0) {
      pos = tail.load(std::memory_order_relaxed);
    } else {
      if (
        tail.compare_exchange_weak(
          pos, pos + 1,
          std::memory_order_relaxed,
          std::memory_order_relaxed
        )
        ) {
        cell.data = v;
        cell.seq.store(pos + 1, std::memory_order_release);
        return true;
      }
    }
  }

  return false;
}

template <typename T, size_t N>
bool queue1<T, N>::dequeue(T& v) noexcept {
  uint64_t pos = head.load(std::memory_order_relaxed);

  while (true) {
    const size_t index = pos % buffer.size();
    auto& cell = buffer[index];
    uint64_t seq = cell.seq.load(std::memory_order_acquire);
    auto dif = int64_t(seq) - int64_t(pos + 1);

    if (dif < 0) return false; // пустая очередь

    if (dif > 0) {
      pos = head.load(std::memory_order_relaxed);
    } else {
      if (
          head.compare_exchange_weak(
          pos, pos + 1,
          std::memory_order_relaxed,
          std::memory_order_relaxed
        )
        ) {
        out = std::move(cell.data);
        cell.seq.store(pos + buffer.size(), std::memory_order_release);
        return true;
      }
    }
  }

  return false;
}
}
}

#endif