#include <string>
#include <vector>

#include <devils_engine/simul/turn_pipeline.h>
#include <doctest/doctest.h>

using devils_engine::simul::presentation_barrier;
using devils_engine::simul::presentation_event_kind;
using devils_engine::simul::presentation_task_id;
using devils_engine::simul::step_control;
using devils_engine::simul::turn_pipeline;

namespace {

enum class beat_step : uint8_t { cue,
                                 commit,
                                 after };

// Project-owned serializable cursor. Gameplay coordinates belong here, not in turn_pipeline.
struct cursor_t {
  uint32_t group = 0;
  beat_step step = beat_step::cue;
  uint64_t player_action_index = 0;
  uint64_t countdown_pulse_index = 0;
};

struct mock_host {
  std::vector<std::vector<char>> program;
  std::string sim_log;
  bool animated = false;

  presentation_task_id next_task = 1;
  std::vector<presentation_task_id> active_tasks;
  uint64_t barrier_budget_ticks = 0;
  uint32_t timeout_calls = 0;
  int halts = 0;

  step_control run_step(cursor_t& cursor, turn_pipeline<cursor_t>& pipe) {
    if (cursor.group >= program.size()) {
      ++halts;
      return step_control::halt;
    }

    switch (cursor.step) {
      case beat_step::cue: {
        active_tasks.clear();
        if (animated) {
          for (size_t i = 0; i < program[cursor.group].size(); ++i) {
            const auto id = next_task++;
            active_tasks.push_back(id);
            pipe.expect_presentation(id, presentation_event_kind::gameplay);
          }
        }
        // Resume after a dropped in-flight cue must run commit exactly once.
        cursor.step = beat_step::commit;
        return step_control::wait;
      }
      case beat_step::commit:
        // Authoritative result is calculated only after every animation reached its gameplay point.
        for (const char effect : program[cursor.group]) {
          sim_log.push_back(effect);
        }
        if (animated) {
          for (const auto id : active_tasks) {
            // Main would send the calculated result back to render here; render cannot finish the
            // task before receiving it, so the same task id is safe for the second checkpoint.
            pipe.expect_presentation(id, presentation_event_kind::finished);
          }
        }
        cursor.step = beat_step::after;
        return step_control::wait;
      case beat_step::after:
        ++cursor.group;
        cursor.step = beat_step::cue;
        if (cursor.group == program.size()) {
          ++cursor.player_action_index;
        }
        return step_control::advance;
    }
    return step_control::halt;
  }

  uint64_t barrier_budget() const noexcept {
    return barrier_budget_ticks;
  }
  void on_barrier_timeout(const cursor_t&, const presentation_barrier&) noexcept {
    ++timeout_calls;
  }
};

} // namespace

TEST_CASE("headless pipeline runs cue commit and finish boundaries inline [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}, {'c'}, {'d', 'e', 'f'}};

  turn_pipeline<cursor_t> pipe;
  pipe.update(host, 0);

  CHECK(host.sim_log == "abcdef");
  CHECK(host.halts == 1);
  CHECK_FALSE(pipe.waiting());
  CHECK_FALSE(pipe.faulted());
  CHECK(pipe.cursor().player_action_index == 1);
}

TEST_CASE("gameplay commit waits for marker and next step waits for animation finish [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}, {'c'}};
  host.animated = true;

  turn_pipeline<cursor_t> pipe;
  pipe.update(host, 1);

  CHECK(host.sim_log.empty());
  CHECK(pipe.waiting());
  REQUIRE(host.active_tasks.size() == 2);
  const auto first_tasks = host.active_tasks;

  // Marker order is presentation-only. No commit until the whole batch reached the marker.
  CHECK(pipe.notify_presentation(first_tasks.back(), presentation_event_kind::gameplay));
  pipe.update(host, 2);
  CHECK(host.sim_log.empty());
  CHECK(pipe.notify_presentation(first_tasks.front(), presentation_event_kind::gameplay));
  pipe.update(host, 3);
  CHECK(host.sim_log == "ab");
  CHECK(pipe.waiting());

  // A gameplay marker cannot accidentally satisfy the distinct finished checkpoint.
  CHECK_FALSE(pipe.notify_presentation(first_tasks.front(), presentation_event_kind::gameplay));
  for (const auto id : first_tasks) {
    CHECK(pipe.notify_presentation(id, presentation_event_kind::finished));
  }
  pipe.update(host, 4);
  CHECK(host.sim_log == "ab");
  CHECK(pipe.waiting()); // group 2 has now started and is waiting at its gameplay point

  REQUIRE(host.active_tasks.size() == 1);
  const auto second = host.active_tasks.front();
  CHECK(pipe.notify_presentation(second, presentation_event_kind::gameplay));
  pipe.update(host, 5);
  CHECK(host.sim_log == "abc");
  CHECK(pipe.notify_presentation(second, presentation_event_kind::finished));
  pipe.update(host, 6);
  CHECK_FALSE(pipe.waiting());
  CHECK(pipe.cursor().player_action_index == 1);
}

TEST_CASE("snapshot at an in-flight cue resumes at deterministic commit exactly once [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}, {'c'}};
  host.animated = true;

  turn_pipeline<cursor_t> pipe;
  pipe.update(host, 1); // cue group 0; commit has not run
  REQUIRE(host.sim_log.empty());
  REQUIRE(pipe.waiting());
  REQUIRE(pipe.cursor().step == beat_step::commit);

  const auto snap = pipe.save();

  mock_host resumed;
  resumed.program = host.program;
  resumed.animated = false; // in-flight presentation is derived and was dropped

  turn_pipeline<cursor_t> restored;
  restored.load(snap);
  CHECK_FALSE(restored.waiting());
  restored.update(resumed, 0);

  CHECK(resumed.sim_log == "abc");
  CHECK(restored.cursor().player_action_index == 1);
  restored.update(resumed, 1); // halt is stable; no duplicate commit
  CHECK(resumed.sim_log == "abc");
}

TEST_CASE("stalled presentation faults once instead of hanging silently [turn_pipeline]") {
  mock_host host;
  host.program = {{'a'}};
  host.animated = true;
  host.barrier_budget_ticks = 5;

  turn_pipeline<cursor_t> pipe;
  pipe.update(host, 10);
  REQUIRE(pipe.waiting());
  CHECK_FALSE(pipe.faulted());

  pipe.update(host, 15); // strict budget: exactly 5 is still allowed
  CHECK(host.timeout_calls == 0);
  pipe.update(host, 16);
  CHECK(pipe.faulted());
  CHECK(host.timeout_calls == 1);

  pipe.update(host, 100);
  CHECK(host.timeout_calls == 1); // latched fault, callback is not repeated every frame
  CHECK(host.sim_log.empty());
}

TEST_CASE("project cursor owns independent action and countdown pulse coordinates [turn_pipeline]") {
  turn_pipeline<cursor_t> pipe;
  pipe.cursor().player_action_index = 2;
  pipe.cursor().countdown_pulse_index = 1; // e.g. one normal + one no_countdown action

  const auto snap = pipe.save();
  turn_pipeline<cursor_t> restored;
  restored.load(snap);

  CHECK(restored.cursor().player_action_index == 2);
  CHECK(restored.cursor().countdown_pulse_index == 1);
}
