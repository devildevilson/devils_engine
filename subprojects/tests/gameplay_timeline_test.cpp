#include <cstdint>
#include <string>
#include <vector>

#include <devils_engine/simul/gameplay_timeline.h>
#include <doctest/doctest.h>

namespace simul = devils_engine::simul;
namespace utils = devils_engine::utils;

namespace {

enum class marker : uint8_t {
  hit,
  finished
};

using timeline_t = simul::gameplay_timeline<marker>;
using event_t = timeline_t::event_type;

} // namespace

TEST_CASE("gameplay timeline returns due events in causal order [time]") {
  timeline_t timeline(8, {10});
  REQUIRE(timeline.try_schedule({{14}, 8, 0, marker::finished}) ==
          simul::schedule_result::scheduled);
  REQUIRE(timeline.try_schedule({{12}, 9, 1, marker::hit}) ==
          simul::schedule_result::scheduled);
  REQUIRE(timeline.try_schedule({{12}, 3, 1, marker::hit}) ==
          simul::schedule_result::scheduled);
  REQUIRE(timeline.try_schedule({{12}, 3, 0, marker::hit}) ==
          simul::schedule_result::scheduled);

  const auto early = timeline.advance_to({11});
  CHECK(early.empty());
  const auto due = timeline.advance_to({12});
  REQUIRE(due.size() == 3);
  CHECK(due[0].source == 3);
  CHECK(due[0].ordinal == 0);
  CHECK(due[1].source == 3);
  CHECK(due[1].ordinal == 1);
  CHECK(due[2].source == 9);
  CHECK(timeline.pending_count() == 1);

  const auto caught_up = timeline.advance_to({20});
  REQUIRE(caught_up.size() == 1);
  CHECK(caught_up[0].payload == marker::finished);
}

TEST_CASE("gameplay timeline rejects past duplicate and over-budget events [time]") {
  timeline_t timeline(2, {5});
  CHECK(timeline.try_schedule({{4}, 1, 0, marker::hit}) ==
        simul::schedule_result::past_tick);
  REQUIRE(timeline.try_schedule({{6}, 1, 0, marker::hit}) ==
          simul::schedule_result::scheduled);
  CHECK(timeline.try_schedule({{7}, 1, 0, marker::finished}) ==
        simul::schedule_result::duplicate);
  REQUIRE(timeline.try_schedule({{7}, 2, 0, marker::finished}) ==
          simul::schedule_result::scheduled);
  CHECK(timeline.try_schedule({{8}, 3, 0, marker::hit}) ==
        simul::schedule_result::capacity_exceeded);
  CHECK(timeline.pending_count() == 2);
  CHECK_THROWS(timeline.advance_to({4}));
}

TEST_CASE("gameplay timeline accepts and drains an event on the current tick [time]") {
  timeline_t timeline(1, {20});
  REQUIRE(timeline.try_schedule({{20}, 4, 0, marker::hit}) ==
          simul::schedule_result::scheduled);
  const auto due = timeline.advance_to({20});
  REQUIRE(due.size() == 1);
  CHECK(due[0].source == 4);
  CHECK(timeline.advance_to({20}).empty());
}

TEST_CASE("gameplay timeline snapshot is canonical and replacement is transactional [time]") {
  timeline_t timeline(4, {10});
  REQUIRE(timeline.try_schedule({{20}, 2, 0, marker::finished}) ==
          simul::schedule_result::scheduled);
  REQUIRE(timeline.try_schedule({{15}, 9, 0, marker::hit}) ==
          simul::schedule_result::scheduled);
  REQUIRE(timeline.try_schedule({{15}, 1, 0, marker::hit}) ==
          simul::schedule_result::scheduled);

  const auto saved = timeline.save();
  REQUIRE(saved.pending.size() == 3);
  CHECK(saved.pending[0].at == utils::simulation_tick{15});
  CHECK(saved.pending[0].source == 1);
  CHECK(saved.pending[1].source == 9);
  CHECK(saved.pending[2].at == utils::simulation_tick{20});

  timeline_t restored(4);
  REQUIRE(restored.load(saved));
  CHECK(restored.save() == saved);

  auto corrupt = saved;
  corrupt.pending.push_back(corrupt.pending.front());
  CHECK_FALSE(restored.load(corrupt));
  CHECK(restored.save() == saved);
}

TEST_CASE("gameplay timeline payload is project-owned data [time]") {
  struct project_event {
    uint32_t entity = 0;
    std::string command;
    bool operator==(const project_event&) const = default;
  };

  simul::gameplay_timeline<project_event, uint32_t> timeline(1);
  REQUIRE(timeline.try_schedule({{3}, 7, 2, {42, "commit"}}) ==
          simul::schedule_result::scheduled);
  const auto due = timeline.advance_to({3});
  REQUIRE(due.size() == 1);
  CHECK(due.front().payload == project_event{42, "commit"});
}
