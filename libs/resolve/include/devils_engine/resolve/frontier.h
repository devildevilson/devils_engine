#ifndef DEVILS_ENGINE_RESOLVE_FRONTIER_H
#define DEVILS_ENGINE_RESOLVE_FRONTIER_H

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

#include "work.h"

namespace devils_engine {
namespace resolve {

enum class frontier_status : uint8_t {
  idle,
  active,
  complete,
  faulted
};

// Owning deterministic control state. A project may serialize this object directly when its Item is
// registered, or mirror these fields in a project-owned schema. MT recording remains in journal;
// this state is only replaced at a barrier after child work has been semantically sealed.
template <work_record Item>
struct frontier_state {
  std::vector<Item> current;
  instance_id next_instance = 1;
  size_t total_jobs = 0;
  uint64_t frontier_index = 0;
  fault error{};
  frontier_status status = frontier_status::idle;

  bool active() const noexcept {
    return status == frontier_status::active;
  }

  bool complete() const noexcept {
    return status == frontier_status::complete;
  }
};

namespace detail {

template <work_record Item>
bool prepare_frontier(std::vector<Item>& values,
                      instance_id& next_instance,
                      fault& error,
                      const resolution_limits& limits,
                      const bool roots) {
  std::sort(values.begin(), values.end(), semantic_less{});
  for (size_t i = 0; i < values.size(); ++i) {
    const auto& header = values[i].header;
    if (!valid_unassigned_provenance(header) ||
        (roots && (header.generation != 0 || header.parent != invalid_instance)) ||
        (!roots && (header.generation == 0 || header.parent == invalid_instance))) {
      error = fault{fault_code::invalid_provenance, header.root, header.parent, i, values.size()};
      return false;
    }
    if (header.generation > limits.max_generation) {
      error = fault{fault_code::generation_exceeded, header.root, header.parent,
                    header.generation, limits.max_generation};
      return false;
    }
    if (i != 0 && semantic_equivalent(values[i - 1], values[i])) {
      error = fault{fault_code::invalid_provenance, header.root, header.parent, i, values.size()};
      return false;
    }
  }

  const instance_id available = std::numeric_limits<instance_id>::max() - next_instance;
  if (next_instance == invalid_instance || values.size() > available) {
    error = fault{fault_code::instance_id_exhausted, invalid_instance, invalid_instance,
                  values.size(), static_cast<size_t>(available)};
    return false;
  }
  for (auto& value : values)
    value.header.id = next_instance++;
  return true;
}

} // namespace detail

template <work_record Item>
bool begin(frontier_state<Item>& state,
           const std::span<const Item> roots,
           const resolution_limits& limits = {},
           const instance_id first_instance = 1) {
  state = frontier_state<Item>{};
  state.next_instance = first_instance;
  if (roots.size() > limits.max_root_jobs) {
    state.error = fault{fault_code::total_budget_exceeded, invalid_instance, invalid_instance,
                        roots.size(), limits.max_root_jobs};
    state.status = frontier_status::faulted;
    return false;
  }
  if (roots.size() > limits.max_jobs_per_frontier) {
    state.error = fault{fault_code::journal_overflow, invalid_instance, invalid_instance,
                        roots.size(), limits.max_jobs_per_frontier};
    state.status = frontier_status::faulted;
    return false;
  }
  if (roots.size() > limits.max_total_jobs) {
    state.error = fault{fault_code::total_budget_exceeded, invalid_instance, invalid_instance,
                        roots.size(), limits.max_total_jobs};
    state.status = frontier_status::faulted;
    return false;
  }

  state.current.assign(roots.begin(), roots.end());
  if (!detail::prepare_frontier(
        state.current, state.next_instance, state.error, limits, true)) {
    state.status = frontier_status::faulted;
    return false;
  }
  state.total_jobs = state.current.size();
  state.status = state.current.empty() ? frontier_status::complete : frontier_status::active;
  return true;
}

// Replace the current frontier after the resolver's worker barrier and deterministic child merge.
// Empty children finish the resolution. The function performs no work itself, so an outer turn FSM
// may pause between calls while a real-time host may call it repeatedly in one simulation tick.
template <work_record Item>
bool advance(frontier_state<Item>& state,
             const std::span<const Item> children,
             const resolution_limits& limits = {}) {
  if (!state.active()) return false;
  if (children.size() > limits.max_jobs_per_frontier) {
    state.error = fault{fault_code::journal_overflow, invalid_instance, invalid_instance,
                        children.size(), limits.max_jobs_per_frontier};
    state.status = frontier_status::faulted;
    return false;
  }
  if (children.size() > limits.max_total_jobs - std::min(state.total_jobs, limits.max_total_jobs)) {
    state.error = fault{fault_code::total_budget_exceeded, invalid_instance, invalid_instance,
                        state.total_jobs + children.size(), limits.max_total_jobs};
    state.status = frontier_status::faulted;
    return false;
  }

  std::vector<Item> prepared(children.begin(), children.end());
  if (!detail::prepare_frontier(
        prepared, state.next_instance, state.error, limits, false)) {
    state.status = frontier_status::faulted;
    return false;
  }

  state.total_jobs += prepared.size();
  ++state.frontier_index;
  state.current.swap(prepared);
  state.status = state.current.empty() ? frontier_status::complete : frontier_status::active;
  return true;
}

} // namespace resolve
} // namespace devils_engine

#endif
