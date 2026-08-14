#ifndef DEVILS_ENGINE_CATALOGUE_PHASE_H
#define DEVILS_ENGINE_CATALOGUE_PHASE_H

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "deferred.h"

namespace devils_engine {
namespace catalogue {

// Passive phase metadata for tests and developer tools. Descriptor/domain ids are diagnostic
// build-local identities, not save/network ids. Executors never look descriptors up and do not
// publish runtime events through this API. A project may keep constexpr descriptors and only add
// them to a caller-owned phase_registry when an inspector is actually opened.
using phase_id = uint64_t;

enum class phase_commit : uint8_t {
  serial,
  parallel_groups,
  serial_structural
};

enum class phase_conflict : uint8_t {
  none,
  target_not_source
};

enum class phase_write_policy : uint8_t {
  read_only,
  disjoint_by_key,
  exclusive,
  structural
};

struct phase_execution {
  mt::arbitration arbitration = mt::arbitration::collect;
  phase_commit commit = phase_commit::serial;
  phase_conflict conflict = phase_conflict::none;
};

struct phase_access {
  uint64_t id = 0;
  std::string_view name;
};

struct phase_budget {
  uint64_t id = 0;
  std::string_view name;
  std::string_view unit;
  uint64_t fixed_limit = 0;
  bool dynamic = false;
};

struct phase_descriptor {
  phase_id id = 0;
  uint64_t domain = 0;
  std::string_view name;
  std::string_view owner;
  phase_execution execution;
  phase_write_policy write_policy = phase_write_policy::exclusive;
  std::span<const phase_access> reads;
  std::span<const phase_access> writes;
  std::span<const phase_budget> budgets;
};

template <utils::template_string_t Name>
consteval phase_access phase_resource() noexcept {
  return phase_access{utils::murmur_hash64A(Name.sv()), Name.sv()};
}

template <utils::template_string_t Name, utils::template_string_t Unit>
consteval phase_budget fixed_phase_budget(const uint64_t limit) noexcept {
  return phase_budget{utils::murmur_hash64A(Name.sv()), Name.sv(), Unit.sv(), limit, false};
}

template <utils::template_string_t Name, utils::template_string_t Unit>
consteval phase_budget dynamic_phase_budget() noexcept {
  return phase_budget{utils::murmur_hash64A(Name.sv()), Name.sv(), Unit.sv(), 0, true};
}

template <typename Strategy>
consteval phase_execution phase_execution_of() noexcept {
  phase_execution out;
  out.arbitration = Strategy::arbitration_policy;
  if constexpr (std::is_same_v<typename Strategy::commit_policy, mt::commit::parallel_groups>) {
    out.commit = phase_commit::parallel_groups;
  } else if constexpr (std::is_same_v<typename Strategy::commit_policy, mt::commit::serial_structural>) {
    out.commit = phase_commit::serial_structural;
  } else {
    static_assert(std::is_same_v<typename Strategy::commit_policy, mt::commit::serial>,
                  "catalogue phase metadata does not know this commit policy");
    out.commit = phase_commit::serial;
  }

  if constexpr (std::is_same_v<typename Strategy::conflict_policy, mt::conflict::target_not_source>) {
    out.conflict = phase_conflict::target_not_source;
  } else {
    static_assert(std::is_same_v<typename Strategy::conflict_policy, mt::conflict::none>,
                  "catalogue phase metadata does not know this conflict policy");
    out.conflict = phase_conflict::none;
  }
  return out;
}

template <typename Domain, size_t ReadCount, size_t WriteCount, size_t BudgetCount>
consteval phase_descriptor make_phase_descriptor(
  const std::string_view name, const std::string_view owner,
  const phase_write_policy write_policy,
  const std::array<phase_access, ReadCount>& reads,
  const std::array<phase_access, WriteCount>& writes,
  const std::array<phase_budget, BudgetCount>& budgets) noexcept {
  return phase_descriptor{
    utils::murmur_hash64A(name),
    utils::type_id<typename Domain::identity_type>(),
    name,
    owner,
    phase_execution_of<typename Domain::strategy_type>(),
    write_policy,
    std::span<const phase_access>(reads),
    std::span<const phase_access>(writes),
    std::span<const phase_budget>(budgets)};
}

// Performs tooling/load-time consistency checks only. It is never called by an executor.
void validate_phase_descriptor(const phase_descriptor& descriptor);

// Explicit, caller-owned inspection registry. Descriptor spans must refer to static storage and stay
// alive as long as the registry; make_phase_descriptor() over static constexpr arrays is the pattern.
class phase_registry {
public:
  void add(const phase_descriptor& descriptor);
  const phase_descriptor* find(phase_id id) const noexcept;
  std::span<const phase_descriptor> descriptors() const noexcept;
  void clear() noexcept;

private:
  std::vector<phase_descriptor> descriptors_;
};

} // namespace catalogue
} // namespace devils_engine

#endif
