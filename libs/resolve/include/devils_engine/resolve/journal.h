#ifndef DEVILS_ENGINE_RESOLVE_JOURNAL_H
#define DEVILS_ENGINE_RESOLVE_JOURNAL_H

#include "devils_engine/utils/error.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "work.h"

namespace devils_engine {
namespace resolve {

enum class journal_phase : uint8_t {
  idle,
  recording,
  sealed
};

// Typed fixed-budget MT journal. Workers only write distinct slots obtained from the atomic physical
// append cursor; seal() restores semantic order and assigns gameplay ids after the worker barrier.
// Physical append order is never observable through records().
template <work_record Item>
class journal {
public:
  journal_phase phase() const noexcept {
    return phase_;
  }

  size_t capacity() const noexcept {
    return capacity_;
  }

  size_t recorded_count() const noexcept {
    return std::min(record_count_.load(std::memory_order_acquire), capacity_);
  }

  bool overflowed() const noexcept {
    return overflowed_.load(std::memory_order_acquire);
  }

  void begin_record(const size_t capacity) {
    if (phase_ == journal_phase::recording) {
      utils::error{}("resolve journal: begin_record({}) during recording, capacity {}, recorded {}", capacity, capacity_, recorded_count());
    }
    if (storage_.size() < capacity) storage_.resize(capacity);
    capacity_ = capacity;
    record_count_.store(0, std::memory_order_relaxed);
    overflowed_.store(false, std::memory_order_relaxed);
    sealed_.clear();
    phase_ = journal_phase::recording;
  }

  // False means the deterministic budget was exceeded. The record is not partially stored, and the
  // following seal() must fail loudly; callers may use the bool only to stop producing extra work.
  bool try_record(const Item& value) noexcept {
    if (phase_ != journal_phase::recording) return false;
    const size_t index = record_count_.fetch_add(1, std::memory_order_relaxed);
    if (index >= capacity_) {
      overflowed_.store(true, std::memory_order_release);
      return false;
    }
    storage_[index] = value;
    return true;
  }

  void seal_ordered() {
    if (phase_ != journal_phase::recording) {
      utils::error{}("resolve journal: seal requires recording phase, current phase {}", static_cast<unsigned>(phase_));
    }
    if (overflowed()) {
      utils::error{}("resolve journal: deterministic capacity {} exceeded by {} attempted records", capacity_, record_count_.load(std::memory_order_acquire));
    }

    const size_t count = record_count_.load(std::memory_order_acquire);
    std::vector<Item> prepared(storage_.begin(), storage_.begin() + count);
    std::sort(prepared.begin(), prepared.end(), semantic_less{});
    for (size_t i = 0; i < prepared.size(); ++i) {
      if (!valid_unassigned_provenance(prepared[i].header)) {
        utils::error{}("resolve journal: record {} has invalid unassigned provenance", i);
      }
      if (i != 0 && semantic_equivalent(prepared[i - 1], prepared[i])) {
        utils::error{}("resolve journal: records {} and {} have duplicate semantic provenance", i - 1, i);
      }
    }
    sealed_.swap(prepared);
    phase_ = journal_phase::sealed;
  }

  void assign_ids(instance_id& next_instance) {
    if (phase_ != journal_phase::sealed) {
      utils::error{}("resolve journal: assign_ids requires sealed phase, current phase {}", static_cast<unsigned>(phase_));
    }
    const instance_id available = std::numeric_limits<instance_id>::max() - next_instance;
    if (next_instance == invalid_instance || sealed_.size() > available) {
      utils::error{}("resolve journal: cannot assign {} ids starting at {}, available {}", sealed_.size(), next_instance, available);
    }
    for (auto& value : sealed_)
      value.header.id = next_instance++;
  }

  void seal(instance_id& next_instance) {
    seal_ordered();
    assign_ids(next_instance);
  }

  std::span<const Item> records() const noexcept {
    return sealed_;
  }

  void reset() noexcept {
    record_count_.store(0, std::memory_order_relaxed);
    overflowed_.store(false, std::memory_order_relaxed);
    sealed_.clear();
    capacity_ = 0;
    phase_ = journal_phase::idle;
  }

private:
  std::vector<Item> storage_;
  std::vector<Item> sealed_;
  std::atomic_size_t record_count_ = 0;
  std::atomic_bool overflowed_ = false;
  size_t capacity_ = 0;
  journal_phase phase_ = journal_phase::idle;
};

} // namespace resolve
} // namespace devils_engine

#endif
