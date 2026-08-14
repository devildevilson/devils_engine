#include <algorithm>
#include <utility>

#include "devils_engine/utils/string_id.h"
#include "diagnostics.h"

namespace devils_engine {
namespace mood {
namespace {

uint32_t transition_ordinal(const system& sys, const system::transition& transition) {
  const auto all = sys.transitions();
  return static_cast<uint32_t>(&transition - all.data());
}

uint8_t name_count(const std::array<std::string_view, 8>& names) {
  uint8_t count = 0;
  while (count < names.size() && !names[count].empty())
    ++count;
  return count;
}

transition_view copy_transition(const system& sys, const system::transition& transition) {
  transition_view out;
  out.ordinal = transition_ordinal(sys, transition);
  out.current_state = transition.current_hash;
  out.event = transition.event_hash;
  out.next_state = transition.next_hash;
  out.current_state_name = transition.current_state;
  out.event_name = transition.event;
  out.next_state_name = transition.next_state;
  out.source = transition.full_line;
  out.guard_count = name_count(transition.guards);
  out.action_count = name_count(transition.actions);
  for (uint8_t i = 0; i < out.guard_count; ++i)
    out.guards[i] = transition.guards[i];
  for (uint8_t i = 0; i < out.action_count; ++i)
    out.actions[i] = transition.actions[i];
  return out;
}

void append_named(std::vector<named_id_view>& out, const utils::id id,
                  const std::string_view name) {
  const auto it = std::find_if(out.begin(), out.end(), [id, name](const named_id_view& item) {
    return item.id == id && item.name == name;
  });
  if (it == out.end()) out.push_back(named_id_view{id, std::string(name)});
}

struct inspected_step {
  step_outcome runtime;
  step_view view;
};

inspected_step evaluate_step(const system& sys, const utils::id state, const utils::id event,
                             const act::exec_context& ctx) {
  inspected_step out;
  out.view.state = state;
  out.view.event = event;

  auto candidates = sys.find_transitions(state, event);
  if (candidates.empty()) {
    candidates = sys.find_transitions(conv::any_state, event);
    out.view.used_any_state = !candidates.empty();
  }

  out.runtime.candidates = static_cast<uint16_t>(candidates.size());
  out.view.candidates.reserve(candidates.size());
  for (const auto& transition : candidates) {
    candidate_view candidate;
    candidate.transition = copy_transition(sys, transition);
    candidate.guard_count = candidate.transition.guard_count;
    for (uint8_t i = 0; i < candidate.guard_count; ++i) {
      candidate.guards[i].id = utils::string_hash(transition.guards[i]);
      candidate.guards[i].name = transition.guards[i];
    }
    out.view.candidates.push_back(std::move(candidate));
  }

  if (candidates.empty()) {
    out.runtime.result = step_result::no_transition;
    out.view.result = step_result::no_transition;
    return out;
  }

  for (size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
    const auto& transition = candidates[candidate_index];
    auto& candidate = out.view.candidates[candidate_index];
    bool accepted = true;
    for (uint8_t guard_index = 0; guard_index < candidate.guard_count; ++guard_index) {
      const bool passed = transition.guards_ptr[guard_index]->invoke(ctx);
      candidate.guards[guard_index].result = passed ? guard_result::passed : guard_result::failed;
      if (!passed) {
        accepted = false;
        break;
      }
    }

    if (accepted) {
      candidate.result = candidate_result::selected;
      out.runtime.result = step_result::transitioned;
      out.runtime.next_state = transition.next_hash;
      out.runtime.taken = &transition;
      out.view.result = step_result::transitioned;
      out.view.selected_transition = candidate.transition.ordinal;
      return out;
    }

    candidate.result = candidate_result::blocked;
    ++out.runtime.blocked;
  }

  out.runtime.result = step_result::blocked;
  out.view.result = step_result::blocked;
  return out;
}

} // namespace

graph_view inspect_graph(const system& sys) {
  graph_view out;
  const auto transitions = sys.transitions();
  out.transitions.reserve(transitions.size());
  for (const auto& transition : transitions) {
    append_named(out.states, transition.current_hash, transition.current_state);
    append_named(out.events, transition.event_hash, transition.event);
    if (transition.next_hash != utils::invalid_id) {
      append_named(out.states, transition.next_hash, transition.next_state);
    }
    out.transitions.push_back(copy_transition(sys, transition));
  }

  const auto less = [](const named_id_view& a, const named_id_view& b) {
    if (a.id != b.id) return a.id < b.id;
    return a.name < b.name;
  };
  std::sort(out.states.begin(), out.states.end(), less);
  std::sort(out.events.begin(), out.events.end(), less);
  return out;
}

step_view inspect_step(const system& sys, const utils::id state, const utils::id event,
                       const act::exec_context& ctx) {
  return evaluate_step(sys, state, event, ctx).view;
}

step_view inspect_step(const system& sys, const std::string_view state,
                       const std::string_view event, const act::exec_context& ctx) {
  return inspect_step(sys, utils::string_hash(state), utils::string_hash(event), ctx);
}

settle_view inspect_settle(const system& sys, const utils::id initial_state,
                           const utils::id event, const act::exec_context& ctx,
                           const uint32_t max_idle_iters) {
  settle_view out;
  out.initial_state = initial_state;
  out.event = event;
  out.final_state = initial_state;
  out.max_idle_iterations = max_idle_iters;

  auto initial = evaluate_step(sys, out.final_state, event, ctx);
  out.steps.push_back(std::move(initial.view));
  if (initial.runtime.result == step_result::transitioned && initial.runtime.taken != nullptr) {
    const auto next = apply_transition(sys, out.final_state, *initial.runtime.taken, ctx);
    if (next != utils::invalid_id) out.final_state = next;
  }

  for (uint32_t i = 0; i < max_idle_iters; ++i) {
    auto idle = evaluate_step(sys, out.final_state, conv::idle, ctx);
    const auto result = idle.runtime.result;
    const auto* taken = idle.runtime.taken;
    out.steps.push_back(std::move(idle.view));

    if (result == step_result::no_transition || taken == nullptr) {
      out.stop_reason = result == step_result::blocked ? settle_stop_reason::blocked
                                                       : settle_stop_reason::no_transition;
      return out;
    }

    const auto next = apply_transition(sys, out.final_state, *taken, ctx);
    if (next == utils::invalid_id) {
      out.stop_reason = settle_stop_reason::internal_transition;
      return out;
    }
    if (next == out.final_state) {
      out.stop_reason = settle_stop_reason::self_loop;
      return out;
    }
    out.final_state = next;
  }

  out.stop_reason = settle_stop_reason::iteration_limit;
  return out;
}

} // namespace mood
} // namespace devils_engine
