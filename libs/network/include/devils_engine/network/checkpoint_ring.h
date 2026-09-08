#ifndef DEVILS_ENGINE_NETWORK_CHECKPOINT_RING_H
#define DEVILS_ENGINE_NETWORK_CHECKPOINT_RING_H

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>

#include "devils_engine/network/bounded_history.h"

namespace devils_engine::network {

// Checkpoint storage is intentionally only a semantic wrapper over bounded
// immutable history. The owner decides what a Blob contains (raw canonical
// bytes, a compressed container, or another self-contained representation)
// and supplies its logical retained size.
template <std::totally_ordered Tick, class Blob, class SizeOf>
  requires std::invocable<SizeOf&, const Blob&> &&
           std::same_as<std::invoke_result_t<SizeOf&, const Blob&>, size_t>
class checkpoint_ring {
public:
  using history_type = bounded_history<Tick, Blob>;
  using entry = typename history_type::entry;

  checkpoint_ring(
    const size_t count_budget,
    const size_t byte_budget,
    SizeOf size_of)
    : history_(count_budget, byte_budget), size_of_(std::move(size_of)) {}

  checkpoint_ring(const size_t count_budget, const size_t byte_budget)
    requires std::default_initializable<SizeOf>
    : checkpoint_ring(count_budget, byte_budget, SizeOf{}) {}

  size_t count_budget() const noexcept {
    return history_.count_budget();
  }

  size_t byte_budget() const noexcept {
    return history_.byte_budget();
  }

  size_t retained_count() const noexcept {
    return history_.retained_count();
  }

  size_t retained_bytes() const noexcept {
    return history_.retained_bytes();
  }

  bool empty() const noexcept {
    return history_.empty();
  }

  std::optional<Tick> oldest_tick() const {
    return history_.oldest_tick();
  }

  std::optional<Tick> newest_tick() const {
    return history_.newest_tick();
  }

  auto entries() const noexcept {
    return history_.entries();
  }

  const entry* find_entry(const Tick& tick) const {
    return history_.find_entry(tick);
  }

  const Blob* find(const Tick& tick) const {
    return history_.find(tick);
  }

  // Binary search over the INDEX SPACE, not over `entries()`. A classic STL
  // algorithm on a `views::transform` range depends on what
  // `iterator_traits` makes of that view's iterator, and libstdc++ 14 answers
  // `output_iterator_tag` for exactly this one — after which `std::upper_bound`
  // compiles and silently returns the wrong element. The index space needs no
  // iterator machinery at all, so the question cannot arise.
  const entry* latest_at_or_before(const Tick& tick) const {
    const size_t count = history_.retained_count();
    size_t low = 0, high = count;
    while (low < high) {
      const size_t middle = low + (high - low) / 2;
      if (tick < history_.at_offset(middle).tick) high = middle;
      else low = middle + 1;
    }
    return low == 0 ? nullptr : &history_.at_offset(low - 1);
  }

  [[nodiscard]] history_store_result try_store(const Tick& tick, const Blob& blob) {
    const auto byte_size = size_t(std::invoke(size_of_, std::as_const(blob)));
    return history_.try_store(tick, blob, byte_size);
  }

  [[nodiscard]] history_store_result try_store(const Tick& tick, Blob&& blob) {
    const auto byte_size = size_t(std::invoke(size_of_, std::as_const(blob)));
    return history_.try_store(tick, std::move(blob), byte_size);
  }

  void clear() noexcept {
    history_.clear();
  }

  std::optional<Blob> take_oldest() {
    return history_.take_oldest();
  }

private:
  history_type history_;
  [[no_unique_address]] SizeOf size_of_;
};

} // namespace devils_engine::network

#endif
