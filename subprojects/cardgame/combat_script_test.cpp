#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/utils/core.h>

#include "cardgame/combat_script.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void check(const bool value, const std::string_view message) {
  if (!value) fail(message);
}

std::vector<cg::presentation_command> drive_to_player(
  cg::combat& game, uint64_t& tick) {
  std::vector<cg::presentation_command> observed;
  for (uint32_t guard = 0; guard < 256; ++guard) {
    game.update(++tick);
    const auto commands = game.take_presentation_commands();
    for (const auto& command : commands) {
      observed.push_back(command);
      const auto event = command.kind == cg::presentation_command_kind::start
                           ? devils_engine::simul::presentation_event_kind::gameplay
                           : devils_engine::simul::presentation_event_kind::finished;
      check(game.notify_presentation(command.task, event),
            "scripted card produced an unexpected presentation event");
    }
    if (game.awaiting_player()) return observed;
    check(!game.faulted(), "scripted card pipeline faulted");
  }
  fail("scripted card pipeline did not return to player input");
}

} // namespace

int main() {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  cg::register_combat_effect_script(sys);

  const auto program = sys.parse<void, cg::combat_effect_scope>(
    "typed_emitters",
    "each_target = { value = { emit_attack = { 4, fire, true }, "
    "emit_healing = -2, emit_shield = -4, "
    "emit_attribute_damage = { agility, 3 }, "
    "emit_status = { burning, 2, 3 } } }");

  cg::resolution_work work;
  work.report.execution = 42;
  const std::array targets{cg::entity_id{2}, cg::entity_id{3}};
  cg::combat_effect_emit_context invocation{
    &work, cg::entity_id{1}, targets, 10, 0};
  devils_script::context vm;
  cg::run_combat_effect_script(program, vm, invocation);

  check(invocation.emitted_instances == 10 && work.plan.size() == 10,
        "DS emitters did not preserve the per-target semantic work count");
  check(work.plan == std::vector<cg::effect_instance_ref>{
                       {cg::effect_store_kind::attack, 0},
                       {cg::effect_store_kind::healing, 0},
                       {cg::effect_store_kind::shield, 0},
                       {cg::effect_store_kind::attribute_damage, 0},
                       {cg::effect_store_kind::effect, 0},
                       {cg::effect_store_kind::attack, 1},
                       {cg::effect_store_kind::healing, 1},
                       {cg::effect_store_kind::shield, 1},
                       {cg::effect_store_kind::attribute_damage, 1},
                       {cg::effect_store_kind::effect, 1}},
        "each_target lost target-major script order while appending typed store refs");
  check(work.attacks.size() == 2 && work.attacks[0].root == 0 &&
          work.attacks[0].parent_execution == 42 && work.attacks[0].source == 1 &&
          work.attacks[0].target == 2 && work.attacks[0].base_damage == 4 &&
          work.attacks[0].type == cg::element::fire && work.attacks[0].applies_element &&
          work.attacks[1].target == 3,
        "emit_attack did not create pointer-free unresolved attack instances");
  check(work.healings.size() == 2 && work.healings[0].target == 2 &&
          work.healings[0].amount == -2 && work.healings[1].target == 3,
        "emit_healing changed the signed value or frozen target order");
  check(work.shields.size() == 2 && work.shields[0].target == 2 &&
          work.shields[0].amount == -4 && work.shields[1].target == 3,
        "emit_shield changed the signed value or frozen target order");
  check(work.attribute_damages.size() == 2 &&
          work.attribute_damages[0].attribute == cg::attribute_kind::agility &&
          work.attribute_damages[0].amount == 3 &&
          work.attribute_damages[1].target == 3,
        "emit_attribute_damage lost its typed attribute or target");
  check(work.effects.size() == 2 &&
          work.effects[0].kind == cg::effect_kind::burning &&
          work.effects[0].stacks == 2 && work.effects[0].remaining_pulses == 3 &&
          work.effects[0].target == 2 && work.effects[1].target == 3,
        "emit_status lost its typed status payload or frozen target order");
  check(cg::has_category(work.report.categories, cg::execution_category::attack) &&
          cg::has_category(work.report.categories, cg::execution_category::healing) &&
          cg::has_category(work.report.categories, cg::execution_category::shield) &&
          cg::has_category(
            work.report.categories, cg::execution_category::attribute_change) &&
          cg::has_category(work.report.categories, cg::execution_category::status),
        "DS emitters did not update execution categories");

  cg::resolution_work bounded_work;
  bounded_work.report.execution = 99;
  cg::combat_effect_emit_context bounded{
    &bounded_work, cg::entity_id{1}, targets, 1, 0};
  bool capacity_failed = false;
  try {
    cg::run_combat_effect_script(program, vm, bounded);
  } catch (const std::length_error&) {
    capacity_failed = true;
  }
  check(capacity_failed,
        "DS authored-effect emission overflow did not fail deterministically");
  check(bounded.emitted_instances == 1 && bounded_work.plan.size() == 1,
        "DS authored-effect emission appended work past its capacity");

  const auto follow_up_program = sys.parse<void, cg::follow_up_rule_scope>(
    "follow_up_attack",
    "{ each_execution = { value = { assert = { executed, "
    "follow_up_input_execution } } }, "
    "emit_follow_up_attack = { input_execution_count, none, false } }");
  std::array<cg::resolution_work, 2> follow_up_input;
  follow_up_input[0].report =
    cg::execution_report{100, cg::player_entity, cg::enemy_entity, true, {}};
  follow_up_input[1].report =
    cg::execution_report{101, cg::player_entity, cg::enemy_entity, true, {}};
  cg::resolution_work follow_up_output;
  follow_up_output.report =
    cg::execution_report{102, cg::player_entity, cg::enemy_entity, true, {}};
  cg::follow_up_rule_emit_context follow_up_invocation{
    follow_up_input,
    &follow_up_output,
    cg::player_entity,
    cg::enemy_entity,
    1,
    0,
    0};
  cg::run_follow_up_rule_script(
    follow_up_program, vm, follow_up_invocation);
  check(follow_up_invocation.visited_executions == 2 &&
          follow_up_invocation.emitted_effects == 1 &&
          follow_up_output.attack_effects ==
            std::vector<cg::attack_effect>{
              {cg::player_entity, cg::element::none, 2, 1, false}} &&
          follow_up_output.program.beats.size() == 1 &&
          follow_up_output.program.beats.front().effects.size() == 1 &&
          follow_up_output.program.beats.front().effects.front().body ==
            cg::effect_ref{cg::authored_effect_store_kind::attack, 0},
        "follow-up DS scope did not consume the frozen prefix and prepare one attack");
  cg::resolution_work bounded_follow_up_output;
  bounded_follow_up_output.report =
    cg::execution_report{103, cg::player_entity, cg::enemy_entity, true, {}};
  cg::follow_up_rule_emit_context bounded_follow_up{
    follow_up_input,
    &bounded_follow_up_output,
    cg::player_entity,
    cg::enemy_entity,
    0,
    0,
    0};
  capacity_failed = false;
  try {
    cg::run_follow_up_rule_script(
      follow_up_program, vm, bounded_follow_up);
  } catch (const std::length_error&) {
    capacity_failed = true;
  }
  check(capacity_failed && bounded_follow_up_output.program.beats.empty() &&
          bounded_follow_up_output.attack_effects.empty(),
        "follow-up authored-effect overflow appended partial work");

  // The combat stores only the stable resource id. The compiled container remains in demiurg and
  // can be supplied again when an in-flight snapshot is resumed by another combat host.
  cg::combat_effect_script_compiler compiler;
  devils_engine::demiurg::module_system modules(CARDGAME_RESOURCE_ROOT);
  modules.load_modules({devils_engine::demiurg::module_system::list_entry{"core/", "", ""}});
  devils_engine::demiurg::resource_system resources;
  resources.register_type<devils_engine::act::script_resource>(
    "scripts", "tavl", &compiler);
  resources.parse_resources(&modules);
  auto* scripted_strike = resources.get<devils_engine::act::script_resource>(
    "scripts/scripted_strike");
  check(scripted_strike != nullptr, "scripted strike resource was not discovered");
  scripted_strike->load(devils_engine::utils::safe_handle_t{});
  check(scripted_strike->category() == devils_engine::act::category::effect &&
          !scripted_strike->program()->cmds.empty(),
        "scripted strike resource did not compile as a combat effect");
  auto* scripted_guard = resources.get<devils_engine::act::script_resource>(
    "scripts/scripted_guard");
  check(scripted_guard != nullptr, "scripted guard resource was not discovered");
  scripted_guard->load(devils_engine::utils::safe_handle_t{});
  check(scripted_guard->category() == devils_engine::act::category::effect &&
          !scripted_guard->program()->cmds.empty(),
        "scripted guard resource did not compile as a combat effect");
  auto* thorns_rule = resources.get<devils_engine::act::script_resource>(
    "scripts/thorns_retaliation");
  check(thorns_rule != nullptr, "thorns retaliation resource was not discovered");
  thorns_rule->load(devils_engine::utils::safe_handle_t{});
  check(thorns_rule->category() == devils_engine::act::category::effect &&
          !thorns_rule->program()->cmds.empty(),
        "thorns retaliation resource did not compile as an immediate rule");
  auto* follow_up_rule = resources.get<devils_engine::act::script_resource>(
    "scripts/follow_up_attack");
  check(follow_up_rule != nullptr, "follow-up attack resource was not discovered");
  cg::combat_effect_script_resources script_resources(resources);

  cg::resolution_work retaliation_work;
  cg::effect_state retaliation_rule{
    700, cg::effect_kind::thorns, cg::enemy_entity, 2, 0};
  cg::damage_outcome retaliation_trigger;
  retaliation_trigger.damage.header = devils_engine::resolve::work_header{
    11,
    10,
    9,
    1,
    0,
    0,
    cg::player_entity,
    cg::enemy_entity,
    devils_engine::resolve::cause_kind::primary,
    false};
  retaliation_trigger.damage.payload.destination = cg::damage_destination::shield;
  retaliation_trigger.route = cg::stat_change_route{-3, -3, 4, 7, 7, 0};
  retaliation_trigger.target_valid = true;
  retaliation_trigger.committed = true;
  cg::retaliation_rule_emit_context retaliation_invocation{
    &retaliation_work,
    &retaliation_rule,
    &retaliation_trigger,
    false,
    3,
    1,
    0};
  cg::run_retaliation_rule_script(
    *thorns_rule->program(), vm, retaliation_invocation);
  check(retaliation_invocation.emitted_responses == 1 &&
          retaliation_work.retaliation_requests ==
            std::vector<cg::retaliation_request>{{retaliation_trigger.damage.header,
                                                  700,
                                                  cg::enemy_entity,
                                                  cg::player_entity,
                                                  2,
                                                  3}},
        "retaliation DS rule did not own the decision to emit its response");

  cg::combat in_flight(cg::run_mode::animated, &script_resources);
  uint64_t in_flight_tick = 0;
  drive_to_player(in_flight, in_flight_tick);
  check(in_flight.submit({cg::player_intent_kind::play_card,
                          cg::card_kind::scripted_strike,
                          1}),
        "resource-backed card intent was rejected");
  in_flight.update(++in_flight_tick);
  check(in_flight.waiting_presentation() &&
          in_flight.last_resolution().script_effects.size() == 1 &&
          in_flight.last_resolution().attacks.empty(),
        "resource-backed card did not pause before invoking its DS effect");

  cg::combat resumed(cg::run_mode::headless, &script_resources);
  resumed.load(in_flight.save());
  uint64_t resumed_tick = 0;
  drive_to_player(resumed, resumed_tick);

  cg::combat control(cg::run_mode::headless, &script_resources);
  uint64_t control_tick = 0;
  drive_to_player(control, control_tick);
  check(control.submit({cg::player_intent_kind::play_card,
                        cg::card_kind::scripted_strike,
                        1}),
        "resource-backed control card intent was rejected");
  drive_to_player(control, control_tick);
  check(resumed.state() == control.state() && resumed.state().enemy.hp == 97,
        "resource-backed scripted card changed across resume");
  check(resumed.last_resolution() == control.last_resolution() &&
          resumed.last_resolution().attacks.size() == 1 &&
          resumed.last_resolution().attacks.front().base_damage == 3,
        "resource-backed DS invocation lost its typed attack or stable trace");

  cg::combat guard(cg::run_mode::animated, &script_resources);
  uint64_t guard_tick = 0;
  (void)drive_to_player(guard, guard_tick);
  check(guard.submit({cg::player_intent_kind::play_card,
                      cg::card_kind::scripted_guard,
                      1}),
        "resource-backed shield card intent was rejected");
  guard.update(++guard_tick);
  check(guard.waiting_presentation() &&
          guard.last_resolution().shields.empty(),
        "resource-backed shield card did not pause before its DS invocation");
  const auto guard_snapshot = guard.save();
  const auto guard_commands = drive_to_player(guard, guard_tick);
  const auto& guard_work = guard.last_resolution();
  check(guard.state().player.shield == 5 &&
          guard_work.shields.size() == 1 &&
          guard_work.shields.front().amount == 5 &&
          guard_work.shield_trace.size() == 1 &&
          guard_work.shield_trace.front().route.before == 0 &&
          guard_work.shield_trace.front().route.committed_after == 5 &&
          guard_work.report.effects.size() == 1 &&
          guard_work.report.effects.front().shield_outcome_begin == 0 &&
          guard_work.report.effects.front().shield_outcome_count == 1 &&
          guard_work.outcomes == std::vector<cg::outcome_ref>{
                                   {cg::outcome_store_kind::shield, 0}} &&
          guard_work.death_trace.size() == 1 && guard_work.death_trace.front().kind == cg::outcome_store_kind::shield && cg::has_category(guard_work.report.categories, cg::execution_category::shield) && cg::has_category(guard_work.report.categories, cg::execution_category::stat_change),
        "resource-backed emit_shield did not resolve through its typed outcome");
  const auto guard_result = std::find_if(
    guard_commands.begin(), guard_commands.end(), [](const auto& command) {
      return command.kind == cg::presentation_command_kind::result &&
             !command.results.empty();
    });
  check(guard_result != guard_commands.end() &&
          guard_result->results.size() == 1 &&
          guard_result->results.front().subject ==
            cg::presentation_subject::shield &&
          guard_result->results.front().target == cg::player_entity &&
          guard_result->results.front().value == 5,
        "emit_shield did not publish its typed presentation result");

  cg::combat resumed_guard(cg::run_mode::headless, &script_resources);
  resumed_guard.load(guard_snapshot);
  uint64_t resumed_guard_tick = 0;
  (void)drive_to_player(resumed_guard, resumed_guard_tick);
  check(resumed_guard.state() == guard.state() &&
          resumed_guard.last_resolution() == guard.last_resolution(),
        "emit_shield changed across pre-invocation snapshot resume");

  cg::combat missing_provider(cg::run_mode::headless);
  uint64_t missing_tick = 0;
  drive_to_player(missing_provider, missing_tick);
  check(missing_provider.submit({cg::player_intent_kind::play_card,
                                 cg::card_kind::scripted_strike,
                                 1}),
        "missing-provider card intent was rejected before resolution");
  bool missing_failed = false;
  try {
    drive_to_player(missing_provider, missing_tick);
  } catch (const std::runtime_error& error) {
    missing_failed = std::string_view(error.what()).find("requires a combat effect script provider") !=
                     std::string_view::npos;
  }
  check(missing_failed,
        "resource-backed card silently fell back when no provider was installed");

  // Loading the optional fixture activates one attack follow-up rule for every live participant.
  // Snapshot at the first follow-up cue: the card execution is sealed input, while the source-party
  // segment and its resolution cursor are still open.
  follow_up_rule->load(devils_engine::utils::safe_handle_t{});
  check(follow_up_rule->category() == devils_engine::act::category::effect &&
          !follow_up_rule->program()->cmds.empty(),
        "follow-up attack resource did not compile as a prepare rule");

  cg::combat follow_up_in_flight(cg::run_mode::animated, &script_resources);
  uint64_t follow_up_tick = 0;
  (void)drive_to_player(follow_up_in_flight, follow_up_tick);
  check(follow_up_in_flight.submit({cg::player_intent_kind::play_card,
                                    cg::card_kind::strike,
                                    1}),
        "follow-up integration card was rejected");
  follow_up_in_flight.update(++follow_up_tick);
  auto commands = follow_up_in_flight.take_presentation_commands();
  check(commands.size() == 1 &&
          commands.front().kind == cg::presentation_command_kind::start &&
          follow_up_in_flight.notify_presentation(
            commands.front().task,
            devils_engine::simul::presentation_event_kind::gameplay),
        "follow-up integration did not reach the card gameplay checkpoint");
  follow_up_in_flight.update(++follow_up_tick);
  commands = follow_up_in_flight.take_presentation_commands();
  check(commands.size() == 1 &&
          commands.front().kind == cg::presentation_command_kind::result &&
          follow_up_in_flight.notify_presentation(
            commands.front().task,
            devils_engine::simul::presentation_event_kind::finished),
        "follow-up integration did not finish the card presentation");
  follow_up_in_flight.update(++follow_up_tick);
  commands = follow_up_in_flight.take_presentation_commands();
  check(commands.size() == 1 &&
          commands.front().kind == cg::presentation_command_kind::start &&
          follow_up_in_flight.cursor().group ==
            cg::combat_group::card_player_party_follow_ups &&
          follow_up_in_flight.cursor().step == cg::combat_step::resolve_execution &&
          follow_up_in_flight.cursor().action.report.segment_open &&
          follow_up_in_flight.cursor().action.report.executions.size() == 1,
        "follow-up execution did not pause over one sealed card input");
  const auto follow_up_snapshot = follow_up_in_flight.save();

  cg::combat resumed_follow_up(cg::run_mode::headless, &script_resources);
  resumed_follow_up.load(follow_up_snapshot);
  uint64_t resumed_follow_up_tick = 0;
  (void)drive_to_player(resumed_follow_up, resumed_follow_up_tick);

  cg::combat control_follow_up(cg::run_mode::headless, &script_resources);
  uint64_t control_follow_up_tick = 0;
  (void)drive_to_player(control_follow_up, control_follow_up_tick);
  check(control_follow_up.submit({cg::player_intent_kind::play_card,
                                  cg::card_kind::strike,
                                  1}),
        "follow-up control card was rejected");
  (void)drive_to_player(control_follow_up, control_follow_up_tick);
  check(resumed_follow_up.state() == control_follow_up.state() &&
          resumed_follow_up.last_resolution() ==
            control_follow_up.last_resolution(),
        "follow-up execution changed across its presentation snapshot");
  const auto actor_tick = std::find_if(
    control_follow_up.state().trace.rbegin(),
    control_follow_up.state().trace.rend(),
    [](const cg::combat_trace_event& event) {
      return event.kind == cg::combat_trace_kind::actor_state_tick;
    });
  check(control_follow_up.state().enemy.hp == 96 &&
          control_follow_up.state().player.hp == 28 &&
          actor_tick != control_follow_up.state().trace.rend() &&
          actor_tick->report_execution_count == 3 &&
          control_follow_up.last_resolution().report.actor == cg::enemy_entity &&
          control_follow_up.last_resolution().damage_trace.size() == 1,
        "party follow-ups did not produce card+source+opposing report work");

  check(resumed_follow_up.submit({cg::player_intent_kind::play_card,
                                  cg::card_kind::strike,
                                  2}) &&
          control_follow_up.submit({cg::player_intent_kind::play_card,
                                    cg::card_kind::strike,
                                    2}),
        "mirrored follow-up integration cards were rejected");
  (void)drive_to_player(resumed_follow_up, resumed_follow_up_tick);
  (void)drive_to_player(control_follow_up, control_follow_up_tick);
  check(resumed_follow_up.state() == control_follow_up.state() &&
          resumed_follow_up.last_resolution() ==
            control_follow_up.last_resolution(),
        "mirrored follow-up sequence changed after resumed execution");
  std::vector<cg::combat_trace_event> final_actor_ticks;
  std::copy_if(control_follow_up.state().trace.begin(),
               control_follow_up.state().trace.end(),
               std::back_inserter(final_actor_ticks),
               [](const cg::combat_trace_event& event) {
                 return event.kind == cg::combat_trace_kind::actor_state_tick;
               });
  check(final_actor_ticks.size() == 3 &&
          final_actor_ticks[1].actor == cg::player_entity &&
          final_actor_ticks[2].actor == cg::enemy_entity &&
          final_actor_ticks[1].report_execution_count == 3 &&
          final_actor_ticks[2].report_execution_count == 3 &&
          final_actor_ticks[1].execution != final_actor_ticks[2].execution &&
          control_follow_up.state().enemy.hp == 90 &&
          control_follow_up.state().player.hp == 23 &&
          control_follow_up.last_resolution().report.actor == cg::player_entity,
        "enemy execution did not build a fresh staged follow-up report");

  std::puts(
    "cardgame DS scopes: typed emitters, follow-up prepare and resume OK");
  return EXIT_SUCCESS;
}
