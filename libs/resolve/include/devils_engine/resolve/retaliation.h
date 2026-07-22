#ifndef DEVILS_ENGINE_RESOLVE_RETALIATION_H
#define DEVILS_ENGINE_RESOLVE_RETALIATION_H

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

#include "journal.h"

namespace devils_engine {
namespace resolve {

struct retaliation_key {
  instance_id triggering_instance = invalid_instance;
  rule_id rule = 0;
  constexpr bool operator==(const retaliation_key&) const noexcept = default;
  constexpr bool operator<(const retaliation_key& other) const noexcept {
    if (triggering_instance != other.triggering_instance) {
      return triggering_instance < other.triggering_instance;
    }
    return rule < other.rule;
  }
};

enum class retaliation_emit_result : uint8_t {
  recorded,
  suppressed_lineage,
  invalid_trigger,
  overflow
};

template <typename Payload>
struct retaliation_record {
  static_assert(std::is_trivially_copyable_v<Payload>);
  work_header header{};
  retaliation_key key{};
  Payload payload{};
  constexpr bool operator==(const retaliation_record&) const noexcept = default;
};

// Hard engine contract:
// - one rule may emit at most once for one triggering instance;
// - a retaliation and every descendant in its lineage can never emit retaliation;
// - multi-hit remains valid because every hit has a different triggering instance id.
template <typename Payload>
class retaliation_journal {
public:
  using record_type = retaliation_record<Payload>;

  void begin_record(const size_t capacity) {
    journal_.begin_record(capacity);
    sealed_.clear();
  }

  retaliation_emit_result emit(
    const work_header& trigger,
    const rule_id rule,
    const uint16_t lane,
    const uint16_t local_ordinal,
    const entity_key source,
    const entity_key target,
    const Payload& payload) noexcept {
    if (trigger.id == invalid_instance || trigger.root == invalid_instance) {
      return retaliation_emit_result::invalid_trigger;
    }
    if (!may_emit_retaliation(trigger)) {
      return retaliation_emit_result::suppressed_lineage;
    }
    record_type record;
    record.header = make_child_header(
      trigger, cause_kind::retaliation, lane, local_ordinal, source, target);
    record.key = retaliation_key{trigger.id, rule};
    record.payload = payload;
    return journal_.try_record(record) ? retaliation_emit_result::recorded
                                       : retaliation_emit_result::overflow;
  }

  void seal(instance_id& next_instance,
            const resolution_limits& limits = {}) {
    journal_.seal_ordered();
    const auto records = journal_.records();
    std::vector<size_t> keyed_order(records.size());
    std::iota(keyed_order.begin(), keyed_order.end(), size_t{0});
    std::sort(keyed_order.begin(), keyed_order.end(), [&](const size_t lhs, const size_t rhs) {
      if (records[lhs].key < records[rhs].key) return true;
      if (records[rhs].key < records[lhs].key) return false;
      return lhs < rhs;
    });

    std::vector<uint8_t> keep(records.size(), uint8_t{0});
    instance_id current_trigger = invalid_instance;
    uint32_t per_trigger = 0;
    size_t begin = 0;
    while (begin < keyed_order.size()) {
      const auto& value = records[keyed_order[begin]];
      if (value.header.generation > limits.max_generation) {
        throw std::length_error("resolve::retaliation generation budget exceeded");
      }
      if (value.key.triggering_instance != current_trigger) {
        current_trigger = value.key.triggering_instance;
        per_trigger = 0;
      }
      ++per_trigger;
      if (per_trigger > limits.max_retaliations_per_trigger) {
        throw std::length_error("resolve::retaliation per-trigger budget exceeded");
      }
      keep[keyed_order[begin]] = 1;

      size_t end = begin + 1;
      while (end < keyed_order.size() &&
             records[keyed_order[end]].key == value.key)
        ++end;
      begin = end;
    }

    std::vector<record_type> prepared;
    prepared.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
      if (keep[i] != 0) prepared.push_back(records[i]);
    }
    const instance_id available = std::numeric_limits<instance_id>::max() - next_instance;
    if (next_instance == invalid_instance || prepared.size() > available) {
      throw std::overflow_error("resolve::retaliation instance id space exhausted");
    }
    for (auto& value : prepared)
      value.header.id = next_instance++;
    sealed_.swap(prepared);
  }

  std::span<const record_type> records() const noexcept {
    return sealed_;
  }

private:
  journal<record_type> journal_;
  std::vector<record_type> sealed_;
};

} // namespace resolve
} // namespace devils_engine

#endif
