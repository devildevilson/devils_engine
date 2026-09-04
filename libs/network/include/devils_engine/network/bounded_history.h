#ifndef DEVILS_ENGINE_NETWORK_BOUNDED_HISTORY_H
#define DEVILS_ENGINE_NETWORK_BOUNDED_HISTORY_H

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

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
                  const std::size_t byte_budget) noexcept
    : count_budget_(count_budget), byte_budget_(byte_budget) {}

  std::size_t count_budget() const noexcept {
    return count_budget_;
  }

  std::size_t byte_budget() const noexcept {
    return byte_budget_;
  }

  std::size_t retained_count() const noexcept {
    return entries_.size();
  }

  std::size_t retained_bytes() const noexcept {
    return retained_bytes_;
  }

  bool empty() const noexcept {
    return entries_.empty();
  }

  std::optional<Tick> oldest_tick() const {
    return entries_.empty() ? std::nullopt
                            : std::optional<Tick>{entries_.front().tick};
  }

  std::optional<Tick> newest_tick() const {
    return entries_.empty() ? std::nullopt
                            : std::optional<Tick>{entries_.back().tick};
  }

  const std::deque<entry>& entries() const noexcept {
    return entries_;
  }

  const entry* find_entry(const Tick& tick) const {
    const auto it = lower_bound(tick);
    return it != entries_.end() && it->tick == tick ? &*it : nullptr;
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
    entries_.clear();
    retained_bytes_ = 0;
  }

private:
  auto lower_bound(const Tick& tick) const {
    return std::lower_bound(
      entries_.begin(), entries_.end(), tick,
      [](const entry& value, const Tick& key) { return value.tick < key; });
  }

  template <class Value>
  history_store_result try_store_impl(const Tick& tick,
                                      Value&& bundle,
                                      const std::size_t byte_size) {
    if (!entries_.empty() && !(entries_.back().tick < tick)) {
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
    while (!entries_.empty() &&
           (entries_.size() >= count_budget_ ||
            retained_bytes_ > byte_budget_ - byte_size)) {
      result.evicted_bytes += entries_.front().byte_size;
      ++result.evicted_count;
      retained_bytes_ -= entries_.front().byte_size;
      entries_.pop_front();
    }

    entries_.push_back(std::move(prepared));
    retained_bytes_ += byte_size;
    return result;
  }

  std::deque<entry> entries_;
  std::size_t count_budget_ = 0;
  std::size_t byte_budget_ = 0;
  std::size_t retained_bytes_ = 0;
};

} // namespace devils_engine::network

#endif
