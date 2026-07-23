#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

#include "cardgame/combat.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "cardgame_action_pipeline_test: %s\n", message);
  std::exit(EXIT_FAILURE);
}

void check(const bool value, const char* message) {
  if (!value) fail(message);
}

void drive_to_player(cg::combat& game, uint64_t& engine_tick) {
  for (uint32_t guard = 0; guard < 256; ++guard) {
    game.update(++engine_tick);
    for (const auto& command : game.take_presentation_commands()) {
      const auto event = command.kind == cg::presentation_command_kind::start
                           ? devils_engine::simul::presentation_event_kind::gameplay
                           : devils_engine::simul::presentation_event_kind::finished;
      check(game.notify_presentation(command.task, event),
            "fake presentation produced an unexpected event");
    }
    if (game.awaiting_player()) return;
    check(!game.faulted(), "grouped action pipeline faulted");
  }
  fail("grouped action pipeline did not return to player input");
}

void submit_and_drive(cg::combat& game, const cg::player_intent intent, uint64_t& tick) {
  check(game.submit(intent), "player intent was rejected");
  drive_to_player(game, tick);
}

std::vector<cg::combat_trace_event> trace_tail(const cg::combat_state& state,
                                               const size_t begin) {
  return {state.trace.begin() + static_cast<std::ptrdiff_t>(begin), state.trace.end()};
}

std::vector<cg::combat_group> entered_groups(
  const std::vector<cg::combat_trace_event>& trace) {
  std::vector<cg::combat_group> result;
  for (const auto& event : trace) {
    if (event.kind == cg::combat_trace_kind::group_enter) result.push_back(event.group);
  }
  return result;
}

std::vector<cg::combat_trace_event> events_of(
  const std::vector<cg::combat_trace_event>& trace,
  const cg::combat_trace_kind kind) {
  std::vector<cg::combat_trace_event> result;
  std::copy_if(trace.begin(), trace.end(), std::back_inserter(result), [kind](const auto& event) {
    return event.kind == kind;
  });
  return result;
}

cg::resolution_work report_execution(
  const cg::instance_id id, const cg::execution_category category) {
  cg::resolution_work work;
  work.report.execution = id;
  work.report.actor = cg::player_entity;
  work.report.selected_target = cg::enemy_entity;
  work.report.executed = true;
  work.report.categories = cg::category_bit(category);
  return work;
}

void check_action_report_stages() {
  cg::action_report report;
  const cg::resolution_work card =
    report_execution(1, cg::execution_category::attack);
  const cg::resolution_work player_follow_up =
    report_execution(2, cg::execution_category::healing);
  const cg::resolution_work second_player_follow_up =
    report_execution(3, cg::execution_category::shield);
  const cg::resolution_work enemy_follow_up =
    report_execution(4, cg::execution_category::status);

  cg::begin_action_report_segment(report, cg::action_report_stage::execution);
  check(cg::action_report_input(report).empty(),
        "card execution stage received stale report input");
  cg::append_action_report_execution(report, card);
  check(cg::action_report_input(report).empty() &&
          cg::action_report_output(report).size() == 1,
        "card execution became visible inside its own stage");
  cg::seal_action_report_segment(report);

  cg::begin_action_report_segment(
    report, cg::action_report_stage::source_party_follow_ups);
  check(cg::action_report_input(report).size() == 1,
        "source party did not receive the card report");
  cg::append_action_report_execution(report, player_follow_up);
  cg::append_action_report_execution(report, second_player_follow_up);
  check(cg::action_report_input(report).size() == 1 &&
          cg::action_report_output(report).size() == 2,
        "source-party output leaked into its frozen input");
  cg::seal_action_report_segment(report);

  cg::begin_action_report_segment(
    report, cg::action_report_stage::opposing_party_follow_ups);
  check(cg::action_report_input(report).size() == 3,
        "opposing party did not receive card plus source-party output");
  cg::append_action_report_execution(report, enemy_follow_up);
  check(cg::action_report_input(report).size() == 3,
        "opposing-party output leaked into its frozen input");
  cg::seal_action_report_segment(report);

  cg::begin_action_report_segment(
    report, cg::action_report_stage::actor_state_tick);
  check(cg::action_report_input(report).size() == 4,
        "ActorStateTick did not receive the complete action report");
  check(report.categories ==
          (cg::category_bit(cg::execution_category::attack) |
           cg::category_bit(cg::execution_category::healing) |
           cg::category_bit(cg::execution_category::shield) |
           cg::category_bit(cg::execution_category::status)),
        "action report did not accumulate execution categories");
  cg::seal_action_report_segment(report);

  check(report.segments.size() == 4,
        "action report did not seal exactly four stage segments");
  check(report.segments[0] ==
          cg::action_report_segment{
            cg::action_report_stage::execution,
            0,
            0,
            1,
            cg::category_bit(cg::execution_category::attack)},
        "card execution segment has incorrect ranges");
  check(report.segments[1].input_execution_count == 1 &&
          report.segments[1].execution_begin == 1 &&
          report.segments[1].execution_count == 2,
        "source-party segment has incorrect ranges");
  check(report.segments[2].input_execution_count == 3 &&
          report.segments[2].execution_begin == 3 &&
          report.segments[2].execution_count == 1,
        "opposing-party segment has incorrect ranges");
  check(report.segments[3].input_execution_count == 4 &&
          report.segments[3].execution_count == 0,
        "ActorStateTick segment has incorrect ranges");

  cg::reset_action_report(report);
  check(report.executions.empty() && report.segments.empty() &&
          report.categories == 0 && !report.segment_open,
        "action report did not reset after ActorStateTick");
}

} // namespace

int main() {
  check_action_report_stages();

  cg::combat normal(cg::run_mode::headless);
  uint64_t normal_tick = 0;
  drive_to_player(normal, normal_tick);

  size_t trace_begin = normal.state().trace.size();
  submit_and_drive(normal,
                   {cg::player_intent_kind::play_card, cg::card_kind::strike, 1},
                   normal_tick);
  auto action_trace = trace_tail(normal.state(), trace_begin);
  check(entered_groups(action_trace) == std::vector<cg::combat_group>{
                                          cg::combat_group::action_begin,
                                          cg::combat_group::card_effects,
                                          cg::combat_group::card_player_party_follow_ups,
                                          cg::combat_group::card_enemy_party_follow_ups,
                                          cg::combat_group::player_actor_state_tick,
                                          cg::combat_group::action_countdown,
                                          cg::combat_group::action_end,
                                          cg::combat_group::awaiting_action},
        "ordinary card did not traverse groups 3-7 and 12 in order");
  const auto first_follow_ups = events_of(action_trace, cg::combat_trace_kind::follow_up_rule);
  check(first_follow_ups.size() == 2 &&
          first_follow_ups[0].actor == cg::player_entity &&
          first_follow_ups[1].actor == cg::enemy_entity &&
          first_follow_ups[0].execution == first_follow_ups[1].execution,
        "card follow-up parties did not consume one frozen player execution in source-side order");
  const auto first_ticks = events_of(action_trace, cg::combat_trace_kind::actor_state_tick);
  check(first_ticks.size() == 1 &&
          first_ticks.front().actor == cg::player_entity &&
          first_ticks.front().report_execution_count == 1 &&
          (first_ticks.front().report_categories &
           cg::category_bit(cg::execution_category::attack)) != 0,
        "ordinary card ActorStateTick did not consume its complete action report");

  trace_begin = normal.state().trace.size();
  submit_and_drive(normal,
                   {cg::player_intent_kind::play_card, cg::card_kind::strike, 2},
                   normal_tick);
  action_trace = trace_tail(normal.state(), trace_begin);
  check(entered_groups(action_trace) == std::vector<cg::combat_group>{
                                          cg::combat_group::action_begin,
                                          cg::combat_group::card_effects,
                                          cg::combat_group::card_player_party_follow_ups,
                                          cg::combat_group::card_enemy_party_follow_ups,
                                          cg::combat_group::player_actor_state_tick,
                                          cg::combat_group::action_countdown,
                                          cg::combat_group::enemy_execution,
                                          cg::combat_group::enemy_action_enemy_party_follow_ups,
                                          cg::combat_group::enemy_action_player_party_follow_ups,
                                          cg::combat_group::enemy_actor_state_tick,
                                          cg::combat_group::action_end,
                                          cg::combat_group::awaiting_action},
        "ready enemy execution did not traverse mirrored groups 8-11");
  const auto mirrored_follow_ups = events_of(action_trace, cg::combat_trace_kind::follow_up_rule);
  check(mirrored_follow_ups.size() == 4 &&
          mirrored_follow_ups[0].actor == cg::player_entity &&
          mirrored_follow_ups[1].actor == cg::enemy_entity &&
          mirrored_follow_ups[2].actor == cg::enemy_entity &&
          mirrored_follow_ups[3].actor == cg::player_entity &&
          mirrored_follow_ups[0].execution == mirrored_follow_ups[1].execution &&
          mirrored_follow_ups[2].execution == mirrored_follow_ups[3].execution &&
          mirrored_follow_ups[0].execution != mirrored_follow_ups[2].execution,
        "enemy execution did not mirror source/opposing party follow-up order");
  const auto mirrored_ticks = events_of(action_trace, cg::combat_trace_kind::actor_state_tick);
  check(mirrored_ticks.size() == 2 &&
          mirrored_ticks[0].actor == cg::player_entity &&
          mirrored_ticks[1].actor == cg::enemy_entity &&
          mirrored_ticks[0].report_execution_count == 1 &&
          mirrored_ticks[1].report_execution_count == 1 &&
          mirrored_ticks[0].execution != mirrored_ticks[1].execution,
        "player and enemy ActorStateTick groups did not consume separate reports");
  check(normal.state().enemy.hp == 94 && normal.state().player.hp == 28,
        "group orchestration changed authoritative attack results");

  // A stolen quick card is still a complete action and advances countdown, but it creates no card
  // execution or party follow-up window.
  cg::combat stolen(cg::run_mode::headless);
  uint64_t stolen_tick = 0;
  drive_to_player(stolen, stolen_tick);
  auto stolen_snapshot = stolen.save();
  stolen_snapshot.state.intercept_next_card = true;
  stolen.load(stolen_snapshot);
  trace_begin = stolen.state().trace.size();
  submit_and_drive(stolen,
                   {cg::player_intent_kind::play_card, cg::card_kind::quick_strike, 1},
                   stolen_tick);
  const auto stolen_trace = trace_tail(stolen.state(), trace_begin);
  check(entered_groups(stolen_trace) == std::vector<cg::combat_group>{
                                          cg::combat_group::action_begin,
                                          cg::combat_group::player_actor_state_tick,
                                          cg::combat_group::action_countdown,
                                          cg::combat_group::action_end,
                                          cg::combat_group::awaiting_action},
        "stolen card did not skip card and both party follow-up groups");
  check(events_of(stolen_trace, cg::combat_trace_kind::card_stolen).size() == 1 &&
          events_of(stolen_trace, cg::combat_trace_kind::follow_up_rule).empty() &&
          events_of(stolen_trace, cg::combat_trace_kind::actor_state_tick).size() == 1 &&
          events_of(stolen_trace, cg::combat_trace_kind::actor_state_tick)
              .front()
              .report_execution_count == 1,
        "stolen card opened follow-ups or skipped its ActorStateTick");
  check(stolen.state().stolen_card_count == 1 &&
          stolen.state().countdown_pulse_index == 1 &&
          stolen.state().enemy_countdown == 1 &&
          stolen.state().enemy.hp == 100 &&
          !stolen.last_resolution().report.executed &&
          stolen.last_resolution().report.effects.empty(),
        "stolen quick card was not recorded as a full countdown-advancing action");

  // A forced end-turn enemy execution receives its own monotonic cycle token and groups 8-11, but
  // is not a player action and does not enter ActionEnd.
  cg::combat forced(cg::run_mode::headless);
  uint64_t forced_tick = 0;
  drive_to_player(forced, forced_tick);
  trace_begin = forced.state().trace.size();
  submit_and_drive(forced,
                   {cg::player_intent_kind::end_turn, cg::card_kind::strike, 1},
                   forced_tick);
  const auto forced_trace = trace_tail(forced.state(), trace_begin);
  check(entered_groups(forced_trace) == std::vector<cg::combat_group>{
                                          cg::combat_group::turn_end,
                                          cg::combat_group::enemy_execution,
                                          cg::combat_group::enemy_action_enemy_party_follow_ups,
                                          cg::combat_group::enemy_action_player_party_follow_ups,
                                          cg::combat_group::enemy_actor_state_tick,
                                          cg::combat_group::turn_begin,
                                          cg::combat_group::awaiting_action},
        "forced end-turn execution did not use mirrored groups before the next turn");
  const auto forced_follow_ups = events_of(forced_trace, cg::combat_trace_kind::follow_up_rule);
  check(forced_follow_ups.size() == 2 &&
          forced_follow_ups[0].actor == cg::enemy_entity &&
          forced_follow_ups[1].actor == cg::player_entity &&
          forced_follow_ups[0].action_token == 1 &&
          forced_follow_ups[1].action_token == 1,
        "forced enemy execution did not receive one deterministic cycle token");
  check(forced.state().player_action_index == 0 &&
          forced.state().action_cycle_index == 1 &&
          forced.state().countdown_pulse_index == 2,
        "forced drain confused player actions, action cycles and countdown pulses");

  // Snapshot while the card cue is in flight. The serialized group/action token resumes through
  // party passes and ActorStateTick without repeating ActionBegin or card commit.
  cg::combat in_flight(cg::run_mode::animated);
  uint64_t in_flight_tick = 0;
  drive_to_player(in_flight, in_flight_tick);
  check(in_flight.submit(
          {cg::player_intent_kind::play_card, cg::card_kind::strike, 1}),
        "could not submit in-flight grouped action");
  in_flight.update(++in_flight_tick);
  check(in_flight.waiting_presentation() &&
          in_flight.cursor().phase == cg::combat_phase::action_cycle &&
          in_flight.cursor().group == cg::combat_group::card_effects &&
          in_flight.cursor().action.token == 1,
        "in-flight snapshot did not retain the card group and action token");
  const auto in_flight_snapshot = in_flight.save();
  check(in_flight_snapshot.pipeline.cursor.action.report.segment_open &&
          in_flight_snapshot.pipeline.cursor.action.report.active_segment.stage ==
            cg::action_report_stage::execution &&
          in_flight_snapshot.pipeline.cursor.action.report.executions.empty(),
        "in-flight snapshot did not retain the open action-report segment");

  cg::combat resumed(cg::run_mode::headless);
  resumed.load(in_flight_snapshot);
  uint64_t resumed_tick = 0;
  drive_to_player(resumed, resumed_tick);

  cg::combat control(cg::run_mode::headless);
  uint64_t control_tick = 0;
  drive_to_player(control, control_tick);
  submit_and_drive(control,
                   {cg::player_intent_kind::play_card, cg::card_kind::strike, 1},
                   control_tick);
  check(resumed.state() == control.state() &&
          resumed.last_resolution() == control.last_resolution(),
        "resume inside card group changed later party passes or authoritative work");

  std::puts("cardgame grouped action pipeline: card/theft/enemy mirror/forced/resume OK");
  return EXIT_SUCCESS;
}
