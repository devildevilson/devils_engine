#ifndef DEVILS_ENGINE_RESOLVE_JOURNAL_H
#define DEVILS_ENGINE_RESOLVE_JOURNAL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <stdexcept>
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
      throw std::logic_error("resolve::journal begin_record during an active record phase");
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
      throw std::logic_error("resolve::journal seal outside a record phase");
    }
    if (overflowed()) {
      throw std::length_error("resolve::journal deterministic capacity exceeded");
    }

    const size_t count = record_count_.load(std::memory_order_acquire);
    std::vector<Item> prepared(storage_.begin(), storage_.begin() + count);
    std::sort(prepared.begin(), prepared.end(), semantic_less{});
    for (size_t i = 0; i < prepared.size(); ++i) {
      if (!valid_unassigned_provenance(prepared[i].header)) {
        throw std::invalid_argument("resolve::journal invalid unassigned provenance");
      }
      if (i != 0 && semantic_equivalent(prepared[i - 1], prepared[i])) {
        throw std::invalid_argument("resolve::journal duplicate semantic provenance");
      }
    }
    sealed_.swap(prepared);
    phase_ = journal_phase::sealed;
  }

  void assign_ids(instance_id& next_instance) {
    if (phase_ != journal_phase::sealed) {
      throw std::logic_error("resolve::journal assign_ids before seal_ordered");
    }
    const instance_id available = std::numeric_limits<instance_id>::max() - next_instance;
    if (next_instance == invalid_instance || sealed_.size() > available) {
      throw std::overflow_error("resolve::journal instance id space exhausted");
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
