#ifndef DEVILS_ENGINE_NETWORK_BOUNDED_HISTORY_H
#define DEVILS_ENGINE_NETWORK_BOUNDED_HISTORY_H

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace devils_engine::network {

enum class history_store_status : unsigned char {
  stored,
  duplicate_tick,
  out_of_order,
  budget_exceeded
};

struct history_store_result {
  history_store_status status = history_store_status::stored;
  std::size_t evicted_count = 0;
  std::size_t evicted_bytes = 0;

  constexpr bool stored() const noexcept {
    return status == history_store_status::stored;
  }
};

// Single-owner history of project-sealed bundles. Tick ordering is ordinary
// strict ordering, not modular sequence ordering. byte_size is a logical cost
// supplied by the owner because a generic Bundle has no universal serialized
// or memory size. Budgets cover entries retained here, not external copies.
// All access is const; borrowed pointers/references remain valid only until
// their entry is evicted, clear() is called, or the history is destroyed.
template <std::totally_ordered Tick, class Bundle>
class bounded_history {
public:
  struct entry {
    Tick tick{};
    std::size_t byte_size = 0;
    Bundle bundle;
  };

  bounded_history(const std::size_t count_budget,
                  const std::size_t byte_budget)
    : slots_(count_budget), count_budget_(count_budget), byte_budget_(byte_budget) {}

  bounded_history(const bounded_history&) = default;
  bounded_history& operator=(const bounded_history&) = default;
  bounded_history(bounded_history&& other) noexcept
    : slots_(std::move(other.slots_)),
      head_(std::exchange(other.head_, 0)), count_(std::exchange(other.count_, 0)),
      count_budget_(std::exchange(other.count_budget_, 0)),
      byte_budget_(std::exchange(other.byte_budget_, 0)),
      retained_bytes_(std::exchange(other.retained_bytes_, 0)) {}

  bounded_history& operator=(bounded_history&& other) noexcept {
    if (this == &other) return *this;
    slots_ = std::move(other.slots_);
    head_ = std::exchange(other.head_, 0);
    count_ = std::exchange(other.count_, 0);
    count_budget_ = std::exchange(other.count_budget_, 0);
    byte_budget_ = std::exchange(other.byte_budget_, 0);
    retained_bytes_ = std::exchange(other.retained_bytes_, 0);
    return *this;
  }

  std::size_t count_budget() const noexcept {
    return count_budget_;
  }

  std::size_t byte_budget() const noexcept {
    return byte_budget_;
  }

  std::size_t retained_count() const noexcept {
    return count_;
  }

  std::size_t retained_bytes() const noexcept {
    return retained_bytes_;
  }

  bool empty() const noexcept {
    return count_ == 0;
  }

  std::optional<Tick> oldest_tick() const {
    return empty() ? std::nullopt : std::optional<Tick>{at(0).tick};
  }

  std::optional<Tick> newest_tick() const {
    return empty() ? std::nullopt : std::optional<Tick>{at(count_ - 1).tick};
  }

  // The range's extent is borrowed until the next mutation. Entry addresses
  // themselves stay stable until their own eviction (fixed ring slots).
  auto entries() const noexcept {
    return std::views::iota(std::size_t{0}, count_) |
           std::views::transform([this](const std::size_t i) -> const entry& {
             return at(i);
           });
  }

  const entry* find_entry(const Tick& tick) const {
    const auto values = entries();
    const auto it = std::lower_bound(values.begin(), values.end(), tick,
                                     [](const entry& value, const Tick& key) {
                                       return value.tick < key;
                                     });
    return it != values.end() && (*it).tick == tick ? &*it : nullptr;
  }

  const Bundle* find(const Tick& tick) const {
    const entry* value = find_entry(tick);
    return value != nullptr ? &value->bundle : nullptr;
  }

  [[nodiscard]] history_store_result try_store(const Tick& tick,
                                               const Bundle& bundle,
                                               const std::size_t byte_size) {
    return try_store_impl(tick, bundle, byte_size);
  }

  [[nodiscard]] history_store_result try_store(const Tick& tick,
                                               Bundle&& bundle,
                                               const std::size_t byte_size) {
    return try_store_impl(tick, std::move(bundle), byte_size);
  }

  void clear() noexcept {
    for (auto& slot : slots_)
      slot.reset();
    head_ = 0;
    count_ = 0;
    retained_bytes_ = 0;
  }

  // Return the oldest payload's ownership to the caller for reuse. No copy,
  // and the other entries retain their addresses. An empty history is normal.
  std::optional<Bundle> take_oldest() {
    if (empty()) return std::nullopt;
    std::optional<Bundle> result(std::move(slots_[head_]->bundle));
    pop_front();
    return result;
  }

private:
  const entry& at(const std::size_t offset) const noexcept {
    return *slots_[(head_ + offset) % count_budget_];
  }

  void pop_front() {
    retained_bytes_ -= slots_[head_]->byte_size;
    slots_[head_].reset();
    head_ = (head_ + 1) % count_budget_;
    --count_;
  }

  template <class Value>
  history_store_result try_store_impl(const Tick& tick,
                                      Value&& bundle,
                                      const std::size_t byte_size) {
    if (!empty() && !(at(count_ - 1).tick < tick)) {
      return {
        find_entry(tick) != nullptr ? history_store_status::duplicate_tick
                                    : history_store_status::out_of_order,
        0,
        0,
      };
    }
    if (count_budget_ == 0 || byte_size > byte_budget_) {
      return {history_store_status::budget_exceeded, 0, 0};
    }

    // Construct before eviction so every ordinary rejection and every bundle
    // copy failure leaves the retained history untouched.
    entry prepared{tick, byte_size, std::forward<Value>(bundle)};
    history_store_result result;
    while (!empty() &&
           (count_ >= count_budget_ ||
            retained_bytes_ > byte_budget_ - byte_size)) {
      result.evicted_bytes += at(0).byte_size;
      ++result.evicted_count;
      pop_front();
    }

    slots_[(head_ + count_) % count_budget_].emplace(std::move(prepared));
    ++count_;
    retained_bytes_ += byte_size;
    return result;
  }

  std::vector<std::optional<entry>> slots_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::size_t count_budget_ = 0;
  std::size_t byte_budget_ = 0;
  std::size_t retained_bytes_ = 0;
};

} // namespace devils_engine::network

#endif
