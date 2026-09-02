#include <cstdint>
#include <string>
#include <vector>

#include <devils_engine/simul/turn_pipeline.h>
#include <doctest/doctest.h>

namespace simul = devils_engine::simul;
namespace utils = devils_engine::utils;

namespace {

enum class beat_step : uint8_t {
  cue,
  commit,
  after
};

struct cursor_t {
  uint32_t group = 0;
  beat_step step = beat_step::cue;
  uint64_t player_action_index = 0;
  uint64_t countdown_pulse_index = 0;
  std::vector<simul::animation_task_id> active_tasks;
  bool operator==(const cursor_t&) const = default;
};

struct mock_host {
  std::vector<std::vector<char>> program;
  std::string sim_log;
  std::vector<simul::animation_task_id> presentation_commands;
  bool animated = false;
  simul::animation_task_id next_task = 1;
  int halts = 0;

  simul::step_control run_step(cursor_t& cursor, simul::turn_pipeline<cursor_t>& pipe) {
    if (cursor.group >= program.size()) {
      ++halts;
      return simul::step_control::halt;
    }

    switch (cursor.step) {
      case beat_step::cue:
        cursor.active_tasks.clear();
        if (animated) {
          for (size_t i = 0; i < program[cursor.group].size(); ++i) {
            const auto task = next_task++;
            cursor.active_tasks.push_back(task);
            pipe.expect_animation_after(
              task, simul::animation_event_kind::gameplay, utils::simulation_duration{2});
            presentation_commands.push_back(task);
          }
        }
        cursor.step = beat_step::commit;
        return simul::step_control::wait;

      case beat_step::commit:
        for (const char effect : program[cursor.group]) sim_log.push_back(effect);
        if (animated) {
          for (const auto task : cursor.active_tasks) {
            pipe.expect_animation_after(
              task,
              simul::animation_event_kind::recovery_finished,
              utils::simulation_duration{3});
          }
        }
        cursor.step = beat_step::after;
        return simul::step_control::wait;

      case beat_step::after:
        cursor.active_tasks.clear();
        ++cursor.group;
        cursor.step = beat_step::cue;
        if (cursor.group == program.size()) ++cursor.player_action_index;
        return simul::step_control::advance;
    }
    return simul::step_control::halt;
  }
};

template <class Pipeline>
concept renderer_can_notify_gameplay = requires(Pipeline& pipe) {
  pipe.notify_presentation(1, 1);
};

} // namespace

static_assert(!renderer_can_notify_gameplay<simul::turn_pipeline<cursor_t>>);

TEST_CASE("headless pipeline runs deterministic boundaries inline [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}, {'c'}, {'d', 'e', 'f'}};

  simul::turn_pipeline<cursor_t> pipe(16);
  pipe.update(host, {0});

  CHECK(host.sim_log == "abcdef");
  CHECK(host.halts == 1);
  CHECK_FALSE(pipe.waiting());
  CHECK_FALSE(pipe.faulted());
  CHECK(pipe.cursor().player_action_index == 1);
}

TEST_CASE("gameplay markers are released by ticks without renderer callbacks [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}, {'c'}};
  host.animated = true;

  simul::turn_pipeline<cursor_t> pipe(16);
  pipe.update(host, {10});
  REQUIRE(pipe.waiting());
  REQUIRE(host.presentation_commands.size() == 2);
  CHECK(host.sim_log.empty());

  pipe.update(host, {11});
  CHECK(host.sim_log.empty());
  pipe.update(host, {12});
  CHECK(host.sim_log == "ab");
  CHECK(pipe.waiting());

  pipe.update(host, {14});
  CHECK(host.sim_log == "ab");
  pipe.update(host, {15});
  CHECK(host.sim_log == "ab");
  CHECK(pipe.cursor().group == 1);
  CHECK(pipe.cursor().step == beat_step::commit);

  pipe.update(host, {17});
  CHECK(host.sim_log == "abc");
  pipe.update(host, {20});
  CHECK_FALSE(pipe.waiting());
  CHECK(pipe.cursor().player_action_index == 1);
}

TEST_CASE("in-flight gameplay animation events survive snapshot [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}};
  host.animated = true;

  simul::turn_pipeline<cursor_t> pipe(8);
  pipe.update(host, {30});
  REQUIRE(pipe.waiting());
  REQUIRE(pipe.cursor().step == beat_step::commit);
  const auto snap = pipe.save();

  mock_host resumed;
  resumed.program = host.program;
  resumed.animated = true;
  resumed.next_task = host.next_task;
  simul::turn_pipeline<cursor_t> restored(8);
  restored.load(snap);

  restored.update(resumed, {31});
  CHECK(resumed.sim_log.empty());
  restored.update(resumed, {32});
  CHECK(resumed.sim_log == "ab");
  CHECK(restored.waiting());
  restored.update(resumed, {35});
  CHECK_FALSE(restored.waiting());
  CHECK(restored.cursor().player_action_index == 1);
  restored.update(resumed, {36});
  CHECK(resumed.sim_log == "ab");
}

TEST_CASE("turn pipeline rejects inconsistent snapshots transactionally [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}};
  host.animated = true;

  simul::turn_pipeline<cursor_t> pipe(8);
  pipe.update(host, {30});
  const auto saved = pipe.save();
  REQUIRE(saved.pending.size() == 2);

  auto missing_barrier_event = saved;
  missing_barrier_event.pending.pop_back();
  CHECK_THROWS_AS(pipe.load(missing_barrier_event), std::invalid_argument);
  CHECK(pipe.save() == saved);

  auto not_waiting = saved;
  not_waiting.waiting = false;
  CHECK_THROWS_AS(pipe.load(not_waiting), std::invalid_argument);
  CHECK(pipe.save() == saved);
}

TEST_CASE("turn pipeline exposes its event budget [turn_pipeline]") {
  mock_host host;
  host.program = {{'a', 'b'}};
  host.animated = true;

  simul::turn_pipeline<cursor_t> pipe(1);
  CHECK_THROWS_AS(pipe.update(host, {1}), std::length_error);
  CHECK(pipe.faulted());
}

TEST_CASE("project cursor owns independent gameplay coordinates [turn_pipeline]") {
  simul::turn_pipeline<cursor_t> pipe(4);
  pipe.cursor().player_action_index = 2;
  pipe.cursor().countdown_pulse_index = 1;

  const auto snap = pipe.save();
  simul::turn_pipeline<cursor_t> restored(4);
  restored.load(snap);

  CHECK(restored.cursor().player_action_index == 2);
  CHECK(restored.cursor().countdown_pulse_index == 1);
}
