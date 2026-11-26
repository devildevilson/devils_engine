#pragma once

#include "jobs.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <devils_engine/utils/deterministic_sort.h>
#include <devils_engine/utils/hash.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <tuple>
#include <vector>

namespace devils_engine::phy02 {

struct pair_key {
  uint32_t first;
  uint32_t first_shape;
  uint32_t second;
  uint32_t second_shape;
  auto operator<=>(const pair_key&) const = default;
};

struct shape_pair {
  uint64_t first;
  uint64_t second;
  uint32_t first_shape;
  uint32_t second_shape;

  auto operator<=>(const shape_pair&) const = default;
};

enum class contact_phase : uint8_t { added, persisted, removed };

struct contact_event {
  shape_pair pair;
  contact_phase phase;
  auto operator<=>(const contact_event&) const = default;
};

enum class pair_storage { linear, hash };
enum class event_storage { shared, lanes };

class active_pairs {
public:
  active_pairs(size_t capacity, pair_storage mode) : capacity_(capacity), mode_(mode) {
    if (mode_ == pair_storage::linear) linear_.reserve(capacity);
    else table_.resize(std::bit_ceil(std::max(size_t{2}, capacity * 2)));
  }

  bool upsert(pair_key key, shape_pair pair) {
    if (mode_ == pair_storage::linear) {
      auto it = std::find_if(linear_.begin(), linear_.end(), [key](const auto& item) { return item.key == key; });
      if (it != linear_.end()) {
        it->pair = pair;
        return true;
      }
      if (size_ == capacity_) return false;
      linear_.push_back({key, pair});
    } else {
      auto& slot = table_[locate(key)];
      if (slot.pair.first != 0) {
        slot.pair = pair;
        return true;
      }
      if (size_ == capacity_) return false;
      slot = {key, pair};
    }
    peak_ = std::max(peak_, ++size_);
    return true;
  }

  bool erase(pair_key key, shape_pair& pair) {
    if (mode_ == pair_storage::linear) {
      auto it = std::find_if(linear_.begin(), linear_.end(), [key](const auto& item) { return item.key == key; });
      if (it == linear_.end()) return false;
      pair = it->pair;
      *it = linear_.back();
      linear_.pop_back();
    } else {
      const auto found = locate(key);
      if (table_[found].pair.first == 0) return false;
      pair = table_[found].pair;
      table_[found] = {};
      // Repair the probe cluster after removing a slot, without accumulating tombstones.
      for (auto i = next(found); table_[i].pair.first != 0; i = next(i)) {
        const auto entry = table_[i];
        table_[i] = {};
        table_[locate(entry.key)] = entry;
      }
    }
    --size_;
    return true;
  }

  size_t peak() const noexcept { return peak_; }
  size_t bytes() const noexcept { return (table_.capacity() + linear_.capacity()) * sizeof(entry); }

private:
  struct entry {
    pair_key key{};
    shape_pair pair{};
  };

  size_t next(size_t i) const noexcept { return (i + 1) & (table_.size() - 1); }

  size_t locate(pair_key key) const noexcept {
    uint64_t hash = 0;
    for (const uint32_t field : {key.first, key.first_shape, key.second, key.second_shape}) {
      hash = utils::hash_combine(hash, field);
    }
    size_t i = utils::fmix64(hash) & (table_.size() - 1);
    while (table_[i].pair.first != 0 && table_[i].key != key) i = next(i);
    return i;
  }

  size_t capacity_;
  pair_storage mode_;
  size_t size_ = 0;
  size_t peak_ = 0;
  std::vector<entry> linear_;
  std::vector<entry> table_;
};

enum class contact_status { ok, event_capacity, pair_capacity, unresolved_removal };

class contact_log final : public JPH::ContactListener {
public:
  contact_log(size_t pair_limit, size_t event_limit, pair_storage pairs, event_storage events)
    : active_(pair_limit, pairs), nodes_(event_limit), events_(events) {
    sorted_.reserve(event_limit);
    output_.reserve(event_limit);
    heads_.fill(no_node);
  }

  void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                      const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
    record(a, b, manifold, contact_phase::added);
  }

  void OnContactPersisted(const JPH::Body& a, const JPH::Body& b,
                          const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
    record(a, b, manifold, contact_phase::persisted);
  }

  void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
    append({{pair.GetBody1ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID1().GetValue(),
             pair.GetBody2ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID2().GetValue()},
            {}, contact_phase::removed});
  }

  // Call after Update, when the backend no longer invokes callbacks.
  contact_status gather() {
    output_.clear();
    sorted_.clear();
    const size_t count = offered_.exchange(0, std::memory_order_relaxed);
    event_peak_ = std::max(event_peak_, count);
    if (events_ == event_storage::lanes) {
      for (auto& head : heads_) {
        for (auto i = head; i != no_node; i = nodes_[i].next) sorted_.push_back(nodes_[i].value);
        head = no_node;
      }
    } else {
      for (size_t i = 0; i < std::min(count, nodes_.size()); ++i) sorted_.push_back(nodes_[i].value);
    }
    return count <= nodes_.size() ? contact_status::ok : contact_status::event_capacity;
  }

  contact_status seal() {
    utils::deterministic_sort(sorted_.begin(), sorted_.end(), [](const auto& a, const auto& b) {
      return std::tie(a.key, a.phase, a.pair) < std::tie(b.key, b.phase, b.pair);
    });
    // In a single discrete step removed keys belong to the previous contact
    // cache. Free them before admitting the current contacts at capacity.
    for (auto& raw : sorted_) {
      if (raw.phase == contact_phase::removed && !active_.erase(raw.key, raw.pair)) {
        return contact_status::unresolved_removal;
      }
    }
    for (const auto& raw : sorted_) {
      if (raw.phase != contact_phase::removed && !active_.upsert(raw.key, raw.pair)) {
        return contact_status::pair_capacity;
      }
      output_.push_back({raw.pair, raw.phase});
    }
    utils::deterministic_sort(output_.begin(), output_.end(), std::less{});
    return contact_status::ok;
  }

  std::span<const contact_event> output() const noexcept { return output_; }
  size_t event_peak() const noexcept { return event_peak_; }
  size_t pair_peak() const noexcept { return active_.peak(); }
  size_t pair_bytes() const noexcept { return active_.bytes(); }
  size_t reserved_bytes() const noexcept {
    return pair_bytes() + nodes_.capacity() * sizeof(node)
      + sorted_.capacity() * sizeof(raw_event) + output_.capacity() * sizeof(contact_event);
  }

private:
  struct raw_event {
    pair_key key;
    shape_pair pair;
    contact_phase phase;
  };

  struct node {
    raw_event value{};
    size_t next = no_node;
  };

  static constexpr size_t no_node = std::numeric_limits<size_t>::max();

  void record(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold, contact_phase phase) {
    const uint32_t sa = manifold.mSubShapeID1.GetValue();
    const uint32_t sb = manifold.mSubShapeID2.GetValue();
    shape_pair pair{a.GetUserData(), b.GetUserData(), sa, sb};
    if (pair.first > pair.second) {
      std::swap(pair.first, pair.second);
      std::swap(pair.first_shape, pair.second_shape);
    }
    append({{a.GetID().GetIndexAndSequenceNumber(), sa, b.GetID().GetIndexAndSequenceNumber(), sb},
            pair, phase});
  }

  void append(raw_event event) {
    if (events_ == event_storage::shared) {
      const std::lock_guard lock(mutex_);
      const auto index = offered_.fetch_add(1, std::memory_order_relaxed);
      if (index < nodes_.size()) nodes_[index].value = event;
    } else {
      const auto index = offered_.fetch_add(1, std::memory_order_relaxed);
      if (index < nodes_.size()) {
        // Each executor has one private lane, backed by the whole bounded arena.
        auto& head = heads_[contact_lane];
        nodes_[index] = {event, head};
        head = index;
      }
    }
  }

  active_pairs active_;
  std::vector<node> nodes_;
  std::vector<raw_event> sorted_;
  std::vector<contact_event> output_;
  event_storage events_;
  std::array<size_t, 5> heads_{};
  std::atomic<size_t> offered_{0};
  std::mutex mutex_;
  size_t event_peak_ = 0;
};

} // namespace devils_engine::phy02
