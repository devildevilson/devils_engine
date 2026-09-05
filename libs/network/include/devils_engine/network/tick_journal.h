#ifndef DEVILS_ENGINE_NETWORK_TICK_JOURNAL_H
#define DEVILS_ENGINE_NETWORK_TICK_JOURNAL_H

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/utils/core.h>

namespace devils_engine {
namespace network {

enum class tick_journal_phase : uint8_t {
  idle,
  recording,
  sealed,
  consumed,
  faulted
};

enum class tick_record_result : uint8_t {
  recorded,
  wrong_tick,
  capacity_exceeded
};

enum class tick_seal_result : uint8_t {
  sealed,
  capacity_exceeded,
  duplicate,
  ambiguous_order
};

template <class Tick>
struct tick_journal_tag {
  Tick tick{};
  uint64_t generation = 0;

  constexpr bool operator==(const tick_journal_tag&) const noexcept = default;
};

template <class Record, class Tick>
class sealed_tick_batch {
public:
  using tag_type = tick_journal_tag<Tick>;

  sealed_tick_batch(tag_type tag, std::vector<Record>&& records)
    : tag_(tag), records_(std::move(records)) {}

  tag_type tag() const {
    return tag_;
  }

  std::span<const Record> records() const noexcept {
    return records_;
  }

  // Explicit ownership transfer back to a pool/journal. All borrowed views of
  // this batch expire here; a live immutable history must never be recycled.
  std::vector<Record> release_storage() && {
    return std::move(records_);
  }

private:
  tag_type tag_;
  std::vector<Record> records_;
};

// A single-owner, fixed-budget collection phase for one simulation tick.  The
// policies define project semantics; the journal only enforces lifecycle,
// provenance uniqueness and canonical order.
template <
  class Record,
  class Tick,
  class TickOf,
  class SemanticLess,
  class SemanticEquivalent>
  requires std::regular<Tick> &&
           std::invocable<const TickOf&, const Record&> &&
           std::same_as<
             std::remove_cvref_t<std::invoke_result_t<const TickOf&, const Record&>>,
             Tick> &&
           std::predicate<const SemanticLess&, const Record&, const Record&> &&
           std::predicate<const SemanticEquivalent&, const Record&, const Record&> &&
           std::sortable<typename std::vector<Record>::iterator, SemanticLess>
class tick_journal {
public:
  using record_type = Record;
  using tick_type = Tick;
  using tag_type = tick_journal_tag<Tick>;
  using batch_type = sealed_tick_batch<Record, Tick>;

  explicit tick_journal(
    TickOf tick_of = {},
    SemanticLess semantic_less = {},
    SemanticEquivalent semantic_equivalent = {})
    : tick_of_(std::move(tick_of)),
      semantic_less_(std::move(semantic_less)),
      semantic_equivalent_(std::move(semantic_equivalent)) {}

  tick_journal_phase phase() const noexcept {
    return phase_;
  }

  size_t capacity() const noexcept {
    return capacity_;
  }

  size_t recorded_count() const noexcept {
    return records_.size();
  }

  bool overflowed() const noexcept {
    return overflowed_;
  }

  std::optional<tag_type> current_tag() const {
    return current_tag_;
  }

  // Preparation/recycling is an idle-phase operation. Supplying a retired
  // batch's storage avoids allocating on the next begin/consume cycle.
  void recycle(std::vector<Record>&& storage) {
    require_phase(tick_journal_phase::idle, "recycle outside idle phase");
    records_ = std::move(storage);
    records_.clear();
  }

  tag_type begin(const Tick tick, const size_t capacity) {
    if (phase_ != tick_journal_phase::idle) {
      utils::error{}("network::tick_journal begin outside idle phase");
    }
    if (generation_ == std::numeric_limits<uint64_t>::max()) {
      utils::error{}("network::tick_journal generation space exhausted");
    }

    // Reserve before publishing the new tag. A failed allocation leaves the
    // journal idle and does not create a half-open tick.
    records_.reserve(capacity);
    records_.clear();
    capacity_ = capacity;
    overflowed_ = false;
    current_tag_ = tag_type{tick, ++generation_};
    phase_ = tick_journal_phase::recording;
    return *current_tag_;
  }

  tick_record_result try_record(const Record& record) {
    require_phase(tick_journal_phase::recording, "record outside recording phase");

    if (std::invoke(tick_of_, record) != current_tag_->tick) {
      return tick_record_result::wrong_tick;
    }
    if (overflowed_ || records_.size() == capacity_) {
      overflowed_ = true;
      return tick_record_result::capacity_exceeded;
    }

    records_.push_back(record);
    return tick_record_result::recorded;
  }

  tick_record_result try_record(Record&& record) {
    require_phase(tick_journal_phase::recording, "record outside recording phase");

    if (std::invoke(tick_of_, record) != current_tag_->tick) {
      return tick_record_result::wrong_tick;
    }
    if (overflowed_ || records_.size() == capacity_) {
      overflowed_ = true;
      return tick_record_result::capacity_exceeded;
    }

    records_.push_back(std::move(record));
    return tick_record_result::recorded;
  }

  [[nodiscard]] tick_seal_result seal() {
    require_phase(tick_journal_phase::recording, "seal outside recording phase");
    if (overflowed_) {
      phase_ = tick_journal_phase::faulted;
      return tick_seal_result::capacity_exceeded;
    }

    std::sort(records_.begin(), records_.end(), semantic_less_);
    for (size_t i = 1; i < records_.size(); ++i) {
      if (std::invoke(semantic_equivalent_, records_[i - 1], records_[i])) {
        phase_ = tick_journal_phase::faulted;
        return tick_seal_result::duplicate;
      }
      if (!std::invoke(semantic_less_, records_[i - 1], records_[i])) {
        phase_ = tick_journal_phase::faulted;
        return tick_seal_result::ambiguous_order;
      }
    }

    phase_ = tick_journal_phase::sealed;
    return tick_seal_result::sealed;
  }

  std::span<const Record> records(const tag_type tag) const {
    require_tag(tag);
    require_phase(tick_journal_phase::sealed, "records outside sealed phase");
    return records_;
  }

  batch_type consume(const tag_type tag) {
    require_tag(tag);
    require_phase(tick_journal_phase::sealed, "consume outside sealed phase");
    phase_ = tick_journal_phase::consumed;
    return batch_type(tag, std::move(records_));
  }

  // Faulted ticks may be discarded; valid ticks must first transfer their
  // immutable batch through consume().
  void retire(const tag_type tag) {
    require_tag(tag);
    if (phase_ != tick_journal_phase::consumed && phase_ != tick_journal_phase::faulted) {
      utils::error{}("network::tick_journal retire before consume or fault");
    }

    records_.clear();
    capacity_ = 0;
    overflowed_ = false;
    current_tag_.reset();
    phase_ = tick_journal_phase::idle;
  }

private:
  void require_phase(const tick_journal_phase expected, const char* message) const {
    if (phase_ != expected) {
      utils::error{}("network::tick_journal {}", message);
    }
  }

  void require_tag(const tag_type tag) const {
    if (!current_tag_.has_value() || *current_tag_ != tag) {
      utils::error{}("network::tick_journal stale or foreign tag");
    }
  }

  [[no_unique_address]] TickOf tick_of_;
  [[no_unique_address]] SemanticLess semantic_less_;
  [[no_unique_address]] SemanticEquivalent semantic_equivalent_;
  std::vector<Record> records_;
  std::optional<tag_type> current_tag_;
  uint64_t generation_ = 0;
  size_t capacity_ = 0;
  bool overflowed_ = false;
  tick_journal_phase phase_ = tick_journal_phase::idle;
};

} // namespace network
} // namespace devils_engine

#endif
