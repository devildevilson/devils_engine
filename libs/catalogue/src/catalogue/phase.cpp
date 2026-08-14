#include <algorithm>

#include "devils_engine/utils/core.h"
#include "phase.h"

namespace devils_engine {
namespace catalogue {
namespace {

void validate_accesses(const std::span<const phase_access> accesses,
                       const std::string_view phase_name, const std::string_view kind) {
  for (size_t i = 0; i < accesses.size(); ++i) {
    if (accesses[i].name.empty()) {
      utils::error{}("catalogue phase '{}': empty {} access name", phase_name, kind);
    }
    if (accesses[i].id != utils::murmur_hash64A(accesses[i].name)) {
      utils::error{}("catalogue phase '{}': {} access '{}' has a mismatched id",
                     phase_name, kind, accesses[i].name);
    }
    for (size_t j = i + 1; j < accesses.size(); ++j) {
      if (accesses[i].id == accesses[j].id) {
        utils::error{}("catalogue phase '{}': duplicate {} access '{}'",
                       phase_name, kind, accesses[i].name);
      }
    }
  }
}

} // namespace

void validate_phase_descriptor(const phase_descriptor& descriptor) {
  if (descriptor.name.empty()) {
    utils::error{}("catalogue phase metadata has an empty name");
  }
  if (descriptor.owner.empty()) {
    utils::error{}("catalogue phase '{}': empty owner", descriptor.name);
  }
  if (descriptor.id != utils::murmur_hash64A(descriptor.name)) {
    utils::error{}("catalogue phase '{}': mismatched phase id", descriptor.name);
  }
  validate_accesses(descriptor.reads, descriptor.name, "read");
  validate_accesses(descriptor.writes, descriptor.name, "write");

  for (size_t i = 0; i < descriptor.budgets.size(); ++i) {
    const auto& budget = descriptor.budgets[i];
    if (budget.name.empty() || budget.unit.empty()) {
      utils::error{}("catalogue phase '{}': budget name/unit must not be empty", descriptor.name);
    }
    if (budget.id != utils::murmur_hash64A(budget.name)) {
      utils::error{}("catalogue phase '{}': budget '{}' has a mismatched id",
                     descriptor.name, budget.name);
    }
    if (!budget.dynamic && budget.fixed_limit == 0) {
      utils::error{}("catalogue phase '{}': fixed budget '{}' must be non-zero",
                     descriptor.name, budget.name);
    }
    for (size_t j = i + 1; j < descriptor.budgets.size(); ++j) {
      if (budget.id == descriptor.budgets[j].id) {
        utils::error{}("catalogue phase '{}': duplicate budget '{}'", descriptor.name, budget.name);
      }
    }
  }

  switch (descriptor.write_policy) {
    case phase_write_policy::read_only:
      if (!descriptor.writes.empty()) {
        utils::error{}("catalogue phase '{}': read_only phase declares writes", descriptor.name);
      }
      break;
    case phase_write_policy::disjoint_by_key:
      if (descriptor.writes.empty()) {
        utils::error{}("catalogue phase '{}': disjoint_by_key phase has no writes", descriptor.name);
      }
      break;
    case phase_write_policy::exclusive:
      if (descriptor.execution.commit == phase_commit::parallel_groups) {
        utils::error{}("catalogue phase '{}': exclusive writes cannot use parallel group commit",
                       descriptor.name);
      }
      break;
    case phase_write_policy::structural:
      if (descriptor.execution.commit != phase_commit::serial_structural) {
        utils::error{}("catalogue phase '{}': structural writes require serial_structural commit",
                       descriptor.name);
      }
      break;
  }

  if (descriptor.execution.commit == phase_commit::serial_structural &&
      descriptor.write_policy != phase_write_policy::structural) {
    utils::error{}("catalogue phase '{}': serial_structural commit must declare structural writes",
                   descriptor.name);
  }
}

void phase_registry::add(const phase_descriptor& descriptor) {
  validate_phase_descriptor(descriptor);
  const auto it = std::lower_bound(descriptors_.begin(), descriptors_.end(), descriptor.id,
                                   [](const phase_descriptor& item, const phase_id id) {
                                     return item.id < id;
                                   });
  if (it != descriptors_.end() && it->id == descriptor.id) {
    utils::error{}("catalogue phase registry: duplicate/colliding phase id for '{}' and '{}'",
                   it->name, descriptor.name);
  }
  descriptors_.insert(it, descriptor);
}

const phase_descriptor* phase_registry::find(const phase_id id) const noexcept {
  const auto it = std::lower_bound(descriptors_.begin(), descriptors_.end(), id,
                                   [](const phase_descriptor& item, const phase_id value) {
                                     return item.id < value;
                                   });
  return it != descriptors_.end() && it->id == id ? &*it : nullptr;
}

std::span<const phase_descriptor> phase_registry::descriptors() const noexcept {
  return descriptors_;
}

void phase_registry::clear() noexcept {
  descriptors_.clear();
}

} // namespace catalogue
} // namespace devils_engine
