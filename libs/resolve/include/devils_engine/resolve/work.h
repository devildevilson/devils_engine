#ifndef DEVILS_ENGINE_RESOLVE_WORK_H
#define DEVILS_ENGINE_RESOLVE_WORK_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace devils_engine {
namespace resolve {

using instance_id = uint64_t;
using entity_key = uint64_t;
using rule_id = uint64_t;

inline constexpr instance_id invalid_instance = 0;
inline constexpr entity_key invalid_entity = 0;

// These are provenance categories, not an exhaustive list of gameplay effects. Projects may put
// more detailed meaning into the typed payload while the resolver keeps the non-recursion contract.
enum class cause_kind : uint8_t {
  primary,
  reaction,
  retaliation,
  periodic,
  effect,
  custom
};

// Owned, pointer-free provenance shared by every typed work record. `root` is a stable semantic root
// token supplied by the producer (normally source_index * capacity + local ordinal); `id` is assigned
// by journal::seal in semantic order and therefore never depends on worker arrival order.
struct work_header {
  instance_id id = invalid_instance;
  instance_id root = invalid_instance;
  instance_id parent = invalid_instance;
  uint32_t generation = 0;
  uint16_t lane = 0;
  uint16_t local_ordinal = 0;
  entity_key source = invalid_entity;
  entity_key target = invalid_entity;
  cause_kind cause = cause_kind::primary;
  bool retaliation_lineage = false;
  constexpr bool operator==(const work_header&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<work_header>);

// Deterministic limits are gameplay/config limits. Wall-time is deliberately absent: exceeding a
// budget is a fault, never a request to continue on a different number of frames on another machine.
struct resolution_limits {
  size_t max_root_jobs = 4096;
  size_t max_total_jobs = 1u << 20;
  size_t max_jobs_per_frontier = 1u << 18;
  uint32_t max_generation = 64;
  uint32_t max_retaliations_per_trigger = 1024;
  constexpr bool operator==(const resolution_limits&) const noexcept = default;
};

enum class fault_code : uint8_t {
  none,
  journal_overflow,
  total_budget_exceeded,
  generation_exceeded,
  retaliation_budget_exceeded,
  instance_id_exhausted,
  invalid_provenance
};

struct fault {
  fault_code code = fault_code::none;
  instance_id root = invalid_instance;
  instance_id parent = invalid_instance;
  size_t observed = 0;
  size_t limit = 0;
  constexpr explicit operator bool() const noexcept {
    return code != fault_code::none;
  }
  constexpr bool operator==(const fault&) const noexcept = default;
};

template <typename Payload>
struct work_item {
  static_assert(std::is_trivially_copyable_v<Payload>,
                "MT resolution payloads must own fixed trivially-copyable values");
  work_header header{};
  Payload payload{};
  constexpr bool operator==(const work_item&) const noexcept = default;
};

template <typename Item>
concept work_record = std::is_trivially_copyable_v<Item> && std::default_initializable<Item> &&
                      requires(Item value) {
                        { value.header } -> std::same_as<work_header&>;
                      };

// Semantic ordering before target grouping. Disjoint target groups may execute in any physical
// order; their outcomes are merged back by the ids assigned from this total order.
struct semantic_less {
  template <work_record Item>
  constexpr bool operator()(const Item& lhs, const Item& rhs) const noexcept {
    const auto& a = lhs.header;
    const auto& b = rhs.header;
    if (a.generation != b.generation) return a.generation < b.generation;
    if (a.root != b.root) return a.root < b.root;
    if (a.parent != b.parent) return a.parent < b.parent;
    if (a.lane != b.lane) return a.lane < b.lane;
    if (a.source != b.source) return a.source < b.source;
    if (a.local_ordinal != b.local_ordinal) return a.local_ordinal < b.local_ordinal;
    if (a.target != b.target) return a.target < b.target;
    return static_cast<uint8_t>(a.cause) < static_cast<uint8_t>(b.cause);
  }
};

template <work_record Item>
constexpr bool semantic_equivalent(const Item& lhs, const Item& rhs) noexcept {
  const semantic_less less;
  return !less(lhs, rhs) && !less(rhs, lhs);
}

constexpr bool valid_unassigned_provenance(const work_header& header) noexcept {
  if (header.id != invalid_instance || header.root == invalid_instance) return false;
  if (header.generation == 0 && header.parent != invalid_instance) return false;
  if (header.generation != 0 && header.parent == invalid_instance) return false;
  if (header.cause == cause_kind::retaliation && !header.retaliation_lineage) return false;
  return true;
}

constexpr bool may_emit_retaliation(const work_header& trigger) noexcept {
  return trigger.cause != cause_kind::retaliation && !trigger.retaliation_lineage;
}

constexpr work_header make_child_header(
  const work_header& parent,
  const cause_kind cause,
  const uint16_t lane,
  const uint16_t local_ordinal,
  const entity_key source,
  const entity_key target) noexcept {
  return work_header{
    invalid_instance,
    parent.root,
    parent.id,
    parent.generation == std::numeric_limits<uint32_t>::max()
      ? parent.generation
      : parent.generation + 1,
    lane,
    local_ordinal,
    source,
    target,
    cause,
    parent.retaliation_lineage || cause == cause_kind::retaliation};
}

} // namespace resolve
} // namespace devils_engine

#endif
