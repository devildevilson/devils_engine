#ifndef DEVILS_ENGINE_CARDGAME_EFFECT_PROGRAM_H
#define DEVILS_ENGINE_CARDGAME_EFFECT_PROGRAM_H

#include <cstdint>
#include <span>
#include <vector>

namespace cardgame {
namespace core {

using entity_id = uint32_t;

inline constexpr entity_id invalid_entity = 0;

enum class targeter_kind : uint8_t {
  target,
  random_target,
  all_targets
};

// Zero means that this authored effect owns an independent selection. A non-zero key explicitly
// shares the first matching selection inside the beat. Reusing a key with a different query is a
// data error rather than an implicit retarget.
using target_binding_id = uint32_t;
inline constexpr target_binding_id independent_target_binding = 0;

struct targeter {
  targeter_kind kind = targeter_kind::target;
  target_binding_id binding = independent_target_binding;
  constexpr bool operator==(const targeter&) const noexcept = default;
};

// Runtime input for one authored effect. `eligible_targets` must already be filtered and placed in
// stable project order. Keeping that policy outside this helper lets cards, statuses and follow-ups
// use the same three targeter kinds without pretending that they share one notion of eligibility.
struct target_query {
  targeter selector{};
  entity_id fixed_target = invalid_entity;
  std::vector<entity_id> eligible_targets;
  bool operator==(const target_query&) const noexcept = default;
};

struct target_snapshot {
  std::vector<entity_id> targets;
  bool operator==(const target_snapshot&) const noexcept = default;
};

// Materializes every target set before a beat starts. Independent random targeters use their
// authored-effect ordinal as salt; explicitly bound targeters reuse exactly one snapshot.
std::vector<target_snapshot> materialize_target_sets(
  std::span<const target_query> queries,
  uint64_t deterministic_entropy);

} // namespace core
} // namespace cardgame

#endif
