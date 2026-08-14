#ifndef DEVILS_ENGINE_MOOD_DIAGNOSTICS_H
#define DEVILS_ENGINE_MOOD_DIAGNOSTICS_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime.h"

namespace devils_engine {
namespace mood {

// Opt-in, owned read models for tests and developer tooling. None of these types is consulted by
// mood::step()/settle(); building a snapshot or trace is an explicit diagnostic operation.
inline constexpr uint32_t invalid_transition_ordinal = UINT32_MAX;

struct named_id_view {
  utils::id id = utils::invalid_id;
  std::string name;
};

struct transition_view {
  uint32_t ordinal = invalid_transition_ordinal; // runtime order inside system::transitions()
  utils::id current_state = utils::invalid_id;
  utils::id event = utils::invalid_id;
  utils::id next_state = utils::invalid_id;
  std::string current_state_name;
  std::string event_name;
  std::string next_state_name;
  std::string source;
  std::array<std::string, 8> guards;
  std::array<std::string, 8> actions;
  uint8_t guard_count = 0;
  uint8_t action_count = 0;
};

struct graph_view {
  std::vector<named_id_view> states;
  std::vector<named_id_view> events;
  std::vector<transition_view> transitions;
};

enum class guard_result : uint8_t {
  not_evaluated,
  passed,
  failed
};

struct guard_view {
  utils::id id = utils::invalid_id;
  std::string name;
  guard_result result = guard_result::not_evaluated;
};

enum class candidate_result : uint8_t {
  not_evaluated,
  blocked,
  selected
};

struct candidate_view {
  transition_view transition;
  std::array<guard_view, 8> guards;
  uint8_t guard_count = 0;
  candidate_result result = candidate_result::not_evaluated;
};

struct step_view {
  utils::id state = utils::invalid_id;
  utils::id event = utils::invalid_id;
  bool used_any_state = false;
  step_result result = step_result::no_transition;
  uint32_t selected_transition = invalid_transition_ordinal;
  std::vector<candidate_view> candidates;
};

enum class settle_stop_reason : uint8_t {
  no_transition,
  blocked,
  internal_transition,
  self_loop,
  iteration_limit
};

struct settle_view {
  utils::id initial_state = utils::invalid_id;
  utils::id event = utils::invalid_id;
  utils::id final_state = utils::invalid_id;
  uint32_t max_idle_iterations = 0;
  settle_stop_reason stop_reason = settle_stop_reason::no_transition;
  std::vector<step_view> steps; // first external event, then zero or more idle steps
};

// Copies names/ids/source rows into an address-independent snapshot. Ordinals identify transitions
// only within this built mood::system; they are diagnostic ids, not save/network ids.
graph_view inspect_graph(const system& sys);

// Evaluates guards once and returns an owned decision trace. Actions are not executed.
step_view inspect_step(const system& sys, utils::id state, utils::id event,
                       const act::exec_context& ctx);
step_view inspect_step(const system& sys, std::string_view state, std::string_view event,
                       const act::exec_context& ctx);

// Diagnostic counterpart of settle(): executes the same actions once while retaining each external/
// completion step and the exact stop reason. Use only for an actual traced apply; this is not preview.
settle_view inspect_settle(const system& sys, utils::id state, utils::id event,
                           const act::exec_context& ctx, uint32_t max_idle_iters = 8);

} // namespace mood
} // namespace devils_engine

#endif
