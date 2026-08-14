#include <algorithm>
#include <memory>
#include <type_traits>

#include <devils_engine/act/function.h>
#include <devils_engine/act/registry.h>
#include <devils_engine/mood/diagnostics.h>
#include <doctest/doctest.h>

namespace {

int never_calls = 0;
int late_calls = 0;
int always_calls = 0;
int after_calls = 0;
int action_calls = 0;

bool never(const devils_engine::act::exec_context&) {
  ++never_calls;
  return false;
}

bool late(const devils_engine::act::exec_context&) {
  ++late_calls;
  return true;
}

bool always(const devils_engine::act::exec_context&) {
  ++always_calls;
  return true;
}

bool after(const devils_engine::act::exec_context&) {
  ++after_calls;
  return true;
}

void mark(const devils_engine::act::exec_context&) {
  ++action_calls;
}

devils_engine::act::registry make_registry() {
  using namespace devils_engine;
  act::registry out;
  out.reg("never", std::make_unique<act::native_function<bool>>(&never));
  out.reg("late", std::make_unique<act::native_function<bool>>(&late));
  out.reg("always", std::make_unique<act::native_function<bool>>(&always));
  out.reg("after", std::make_unique<act::native_function<bool>>(&after));
  out.reg("mark", std::make_unique<act::native_function<void>>(&mark));
  return out;
}

void reset_calls() {
  never_calls = 0;
  late_calls = 0;
  always_calls = 0;
  after_calls = 0;
  action_calls = 0;
}

} // namespace

TEST_CASE("mood graph diagnostics own names and transition rows") {
  using namespace devils_engine;
  static_assert(!std::is_pointer_v<decltype(mood::transition_view::ordinal)>);
  static_assert(!std::is_pointer_v<decltype(mood::step_view::selected_transition)>);

  auto registry = make_registry();
  mood::graph_view graph;
  {
    mood::system sys(&registry, std::vector<std::string>{
                                  "idle + see [always] / mark = alert",
                                  "alert + calm = idle",
                                });
    graph = mood::inspect_graph(sys);
  }

  REQUIRE(graph.transitions.size() == 2);
  const auto it = std::find_if(graph.transitions.begin(), graph.transitions.end(),
                               [](const mood::transition_view& transition) {
                                 return transition.event_name == "see";
                               });
  REQUIRE(it != graph.transitions.end());
  CHECK(it->current_state_name == "idle");
  CHECK(it->next_state_name == "alert");
  CHECK(it->guards[0] == "always");
  CHECK(it->actions[0] == "mark");
  CHECK(it->source == "idle + see [always] / mark = alert");
  CHECK(graph.states.size() == 2);
  CHECK(graph.events.size() == 2);
}

TEST_CASE("mood step diagnostics evaluate each required guard once") {
  using namespace devils_engine;
  reset_calls();
  auto registry = make_registry();
  mood::system sys(&registry, std::vector<std::string>{
                                "s0 + go [never, late] = rejected",
                                "s0 + go [always] = selected",
                                "s0 + go [after] = not_reached",
                                "any_state + fallback [always] = fallback_target",
                              });
  act::execution_scratch scratch;
  act::exec_context ctx{};
  ctx.scratch = &scratch;

  const auto trace = mood::inspect_step(sys, "s0", "go", ctx);
  REQUIRE(trace.result == mood::step_result::transitioned);
  REQUIRE(trace.candidates.size() == 3);
  CHECK_FALSE(trace.used_any_state);
  CHECK(trace.candidates[0].result == mood::candidate_result::blocked);
  CHECK(trace.candidates[0].guards[0].result == mood::guard_result::failed);
  CHECK(trace.candidates[0].guards[1].result == mood::guard_result::not_evaluated);
  CHECK(trace.candidates[1].result == mood::candidate_result::selected);
  CHECK(trace.candidates[1].guards[0].result == mood::guard_result::passed);
  CHECK(trace.candidates[2].result == mood::candidate_result::not_evaluated);
  CHECK(trace.selected_transition == trace.candidates[1].transition.ordinal);
  CHECK(never_calls == 1);
  CHECK(late_calls == 0);
  CHECK(always_calls == 1);
  CHECK(after_calls == 0);

  const auto fallback = mood::inspect_step(sys, "unknown", "fallback", ctx);
  CHECK(fallback.result == mood::step_result::transitioned);
  CHECK(fallback.used_any_state);

  const auto missing = mood::inspect_step(sys, "unknown", "missing", ctx);
  CHECK(missing.result == mood::step_result::no_transition);
  CHECK_FALSE(missing.used_any_state);
  CHECK(missing.candidates.empty());
}

TEST_CASE("mood settle diagnostics retain the applied chain and stop reason") {
  using namespace devils_engine;
  reset_calls();
  auto registry = make_registry();
  mood::system sys(&registry, std::vector<std::string>{
                                "s0 + go / mark = s1",
                                "s1 + idle = s2",
                                "s2 + idle = s2",
                                "x + idle = y",
                                "y + idle = x",
                                "internal + idle / mark",
                              });
  act::execution_scratch scratch;
  act::exec_context ctx{};
  ctx.scratch = &scratch;

  const auto self_loop = mood::inspect_settle(
    sys, utils::string_hash("s0"), utils::string_hash("go"), ctx);
  CHECK(self_loop.final_state == utils::string_hash("s2"));
  CHECK(self_loop.stop_reason == mood::settle_stop_reason::self_loop);
  REQUIRE(self_loop.steps.size() == 3);
  CHECK(self_loop.steps[0].event == utils::string_hash("go"));
  CHECK(self_loop.steps[1].event == mood::conv::idle);
  CHECK(action_calls == 1);

  const auto limited = mood::inspect_settle(
    sys, utils::string_hash("x"), mood::conv::idle, ctx, 2);
  CHECK(limited.stop_reason == mood::settle_stop_reason::iteration_limit);
  CHECK(limited.final_state == utils::string_hash("y"));

  const auto internal = mood::inspect_settle(
    sys, utils::string_hash("internal"), utils::string_hash("missing"), ctx);
  CHECK(internal.stop_reason == mood::settle_stop_reason::internal_transition);
  CHECK(internal.final_state == utils::string_hash("internal"));
  CHECK(action_calls == 2);
}
