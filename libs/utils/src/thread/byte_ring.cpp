#include "devils_engine/thread/byte_ring.h"

namespace devils_engine {
namespace thread {

byte_ring::byte_ring(const size_t capacity) : buffer(capacity) {}

size_t byte_ring::capacity() const noexcept {
  return buffer.size();
}

int64_t byte_ring::alloc(const size_t size, std::span<std::byte>& out) noexcept {
  out = {};
  const auto cap = buffer.size();
  if (size == 0 || size > cap) {
    return -1;
  }

  const auto tail = tail_.load(std::memory_order_acquire);
  const auto index = head_ % cap;
  const auto padding = index + size > cap ? cap - index : 0;
  const auto required_size = padding + size;
  if ((head_ + required_size) - tail > cap) {
    return -1;
  }

  const auto pos = head_ + padding;
  out = std::span<std::byte>(buffer.data() + static_cast<size_t>(pos % cap), size);
  head_ = pos + size;
  return static_cast<int64_t>(pos);
}

std::span<const std::byte> byte_ring::at(const int64_t pos, const size_t size) const noexcept {
  const auto index = static_cast<size_t>(static_cast<uint64_t>(pos) % buffer.size());
  return std::span<const std::byte>(buffer.data() + index, size);
}

void byte_ring::release(const int64_t pos_end) noexcept {
  tail_.store(static_cast<uint64_t>(pos_end), std::memory_order_release);
}

size_t byte_ring::used_approx() const noexcept {
  return static_cast<size_t>(head_ - tail_.load(std::memory_order_acquire));
}

} // namespace thread
} // namespace devils_engine
