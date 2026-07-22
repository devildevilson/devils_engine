#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cardgame/combat.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "cardgame_headless_smoke: %s\n", message);
  std::exit(EXIT_FAILURE);
}

void check(const bool value, const char* message) {
  if (!value) {
    fail(message);
  }
}

// Fake render thread. It acknowledges presentation commands only between main-thread updates:
// start -> gameplay marker, result -> finished marker.
void drive_to_player(cg::combat& game, uint64_t& engine_tick) {
  for (uint32_t guard = 0; guard < 256; ++guard) {
    game.update(++engine_tick);
    const auto commands = game.take_presentation_commands();
    for (const auto& command : commands) {
      const auto event = command.kind == cg::presentation_command_kind::start
                           ? devils_engine::simul::presentation_event_kind::gameplay
                           : devils_engine::simul::presentation_event_kind::finished;
      check(game.notify_presentation(command.task, event),
            "fake render produced an unexpected presentation event");
    }
    if (game.awaiting_player()) {
      return;
    }
    check(!game.faulted(), "presentation pipeline faulted");
  }
  fail("pipeline did not reach player input within the guard budget");
}

void submit_and_drive(cg::combat& game, const cg::player_intent intent, uint64_t& tick) {
  check(game.submit(intent), "player intent was rejected at an input boundary");
  drive_to_player(game, tick);
}

size_t damage_count(const cg::combat& game, const cg::damage_channel channel) {
  return static_cast<size_t>(std::count_if(
    game.last_resolution().damage_trace.begin(),
    game.last_resolution().damage_trace.end(),
    [channel](const auto& value) {
      return value.damage.payload.channel == channel;
    }));
}

} // namespace

int main() {
  cg::combat headless(cg::run_mode::headless);
  cg::combat animated(cg::run_mode::animated);
  uint64_t headless_tick = 0;
  uint64_t animated_tick = 0;
  drive_to_player(headless, headless_tick);
  drive_to_player(animated, animated_tick);

  // Three turns. quick_strike increments the player-action coordinate but not countdown;
  // end_turn creates forced countdown pulses that are not player actions.
  const std::array script{
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::strike, 1},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::quick_strike, 2},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::strike, 3},
    cg::player_intent{cg::player_intent_kind::end_turn, cg::card_kind::strike, 4},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::strike, 5},
    cg::player_intent{cg::player_intent_kind::end_turn, cg::card_kind::strike, 6},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::quick_strike, 7},
    cg::player_intent{cg::player_intent_kind::end_turn, cg::card_kind::strike, 8},
  };

  for (const auto intent : script) {
    submit_and_drive(headless, intent, headless_tick);
    submit_and_drive(animated, intent, animated_tick);
    check(headless.state() == animated.state(),
          "animated and headless authoritative states diverged");
  }

  check(headless.state().turn_index == 4, "script did not cross three turn boundaries");
  check(headless.state().player_action_index == 5,
        "player action coordinate counted a forced pulse or end-turn");
  check(headless.state().countdown_pulse_index == 6,
        "countdown pulse coordinate did not distinguish quick actions and forced pulses");

  // One attack and its reaction both pass through the target's resistance script operations.
  // Thorns listens to the attack instance only, so the reaction cannot recursively return damage.
  cg::combat elemental_headless(cg::run_mode::headless);
  cg::combat elemental_animated(cg::run_mode::animated);
  uint64_t elemental_headless_tick = 0;
  uint64_t elemental_animated_tick = 0;
  drive_to_player(elemental_headless, elemental_headless_tick);
  drive_to_player(elemental_animated, elemental_animated_tick);
  auto elemental_snapshot = elemental_headless.save();
  elemental_snapshot.state.enemy.elemental_mark = cg::element::water;
  elemental_snapshot.state.enemy.resistance_basis_points[static_cast<size_t>(cg::element::fire)] = 2500;
  elemental_snapshot.state.enemy.effects.push_back(
    {1000, cg::effect_kind::thorns, cg::enemy_entity, 1, 0});
  elemental_snapshot.state.player.effects.push_back(
    {1001, cg::effect_kind::thorns, cg::player_entity, 1, 0});
  elemental_headless.load(elemental_snapshot);
  elemental_animated.load(elemental_snapshot);

  const cg::player_intent fire{
    cg::player_intent_kind::play_card, cg::card_kind::fire_strike, 1};
  submit_and_drive(elemental_headless, fire, elemental_headless_tick);
  submit_and_drive(elemental_animated, fire, elemental_animated_tick);
  check(elemental_headless.state() == elemental_animated.state(),
        "complex animated/headless resolution diverged");
  check(elemental_headless.state().enemy.hp == 96,
        "fire resistance was not applied to attack and reaction damage");
  check(elemental_headless.state().player.hp == 29,
        "thorns did not create exactly one returned instance for the attack");
  check(elemental_headless.state().enemy.elemental_mark == cg::element::none,
        "elemental collision did not consume the previous mark");
  check(damage_count(elemental_headless, cg::damage_channel::primary) == 1,
        "fire strike did not create one attack damage instance");
  check(damage_count(elemental_headless, cg::damage_channel::reaction) == 1,
        "elemental collision did not create one reaction damage instance");
  check(damage_count(elemental_headless, cg::damage_channel::retaliation) == 1,
        "returned damage recursively reflected or was not emitted");
  const auto& elemental_trace = elemental_headless.last_resolution().damage_trace;
  check(elemental_trace.size() == 3,
        "elemental resolution did not materialize three damage outcomes");
  const auto& primary = elemental_trace[0];
  const auto& reaction = elemental_trace[1];
  const auto& retaliation = elemental_trace[2];
  check(primary.damage.header.cause == devils_engine::resolve::cause_kind::primary &&
          primary.damage.header.generation == 0 &&
          primary.route.requested == 4 && primary.route.modified == 3 &&
          primary.committed,
        "primary damage did not preserve resolve provenance and route data");
  check(reaction.damage.header.cause == devils_engine::resolve::cause_kind::reaction &&
          reaction.damage.header.parent == primary.damage.header.id &&
          reaction.damage.header.root == primary.damage.header.root &&
          reaction.damage.header.generation == 1,
        "elemental reaction is not a child of its triggering hit");
  check(retaliation.damage.header.cause == devils_engine::resolve::cause_kind::retaliation &&
          retaliation.damage.header.parent == primary.damage.header.id &&
          retaliation.damage.header.root == primary.damage.header.root &&
          retaliation.damage.header.retaliation_lineage,
        "thorns did not use the hard resolve retaliation contract");
  check(elemental_headless.last_resolution().damage_frontier.complete() &&
          elemental_headless.last_resolution().damage_frontier.total_jobs == 3,
        "primary/reaction/retaliation frontier did not reach its persisted completion boundary");
  check(elemental_headless.last_resolution().effect_trace.size() == 1 &&
          elemental_headless.last_resolution().effect_trace.front().result ==
            cg::effect_apply_result::added,
        "allowed burning effect was not added through the effect resolver");
  const auto& elemental_work = elemental_headless.last_resolution();
  check(elemental_work.program.beats.size() == 1 &&
          elemental_work.program.beats.front().effects.size() == 2,
        "fire card did not preserve its two authored effects in one beat");
  check(elemental_work.report.effects.size() == 2 &&
          elemental_work.report.effects[0].beat_index == 0 &&
          elemental_work.report.effects[1].beat_index == 0 &&
          elemental_work.report.effects[0].damage_outcome_count == 3 &&
          elemental_work.report.effects[0].outcome_count == 3 &&
          elemental_work.report.effects[1].effect_outcome_count == 1 &&
          elemental_work.report.effects[1].outcome_count == 1,
        "execution report did not map authored effects to their typed outcome ranges");
  check(elemental_work.outcomes == std::vector<cg::outcome_ref>{
                                     {cg::outcome_store_kind::damage, 0},
                                     {cg::outcome_store_kind::damage, 1},
                                     {cg::outcome_store_kind::damage, 2},
                                     {cg::outcome_store_kind::effect, 0}},
        "common outcome envelope did not preserve damage/status semantic order");
  const auto categories = elemental_work.report.categories;
  check(cg::has_category(categories, cg::execution_category::attack) &&
          cg::has_category(categories, cg::execution_category::damage) &&
          cg::has_category(categories, cg::execution_category::stat_change) &&
          cg::has_category(categories, cg::execution_category::status) &&
          cg::has_category(categories, cg::execution_category::reaction) &&
          cg::has_category(categories, cg::execution_category::elemental_reaction) &&
          cg::has_category(categories, cg::execution_category::retaliation) &&
          !cg::has_category(categories, cg::execution_category::healing) &&
          !cg::has_category(categories, cg::execution_category::attribute_change),
        "execution report category mask does not describe its emitted work");
  check(elemental_work.death_trace.size() == 4,
        "death predicate was not evaluated after every damage/status outcome");

  // Both authored effects in the fire-card beat cue together. Only after every gameplay marker is
  // present does the sim execute attack then status and publish one aggregated result per effect.
  cg::combat mid_resolution(cg::run_mode::animated);
  mid_resolution.load(elemental_snapshot);
  uint64_t mid_resolution_tick = 0;
  check(mid_resolution.submit(fire), "could not submit mid-resolution snapshot action");
  mid_resolution.update(++mid_resolution_tick);
  auto mid_commands = mid_resolution.take_presentation_commands();
  check(mid_commands.size() == 2 &&
          std::all_of(mid_commands.begin(), mid_commands.end(), [](const auto& command) {
            return command.kind == cg::presentation_command_kind::start &&
                   command.targets == std::vector<cg::entity_id>{cg::enemy_entity};
          }),
        "one beat did not publish both authored-effect cues with frozen targets");
  for (const auto& command : mid_commands) {
    check(mid_resolution.notify_presentation(
            command.task, devils_engine::simul::presentation_event_kind::gameplay),
          "could not deliver one batched gameplay marker");
  }
  mid_resolution.update(++mid_resolution_tick);
  mid_commands = mid_resolution.take_presentation_commands();
  check(mid_commands.size() == 2 &&
          std::all_of(mid_commands.begin(), mid_commands.end(), [](const auto& command) {
            return command.kind == cg::presentation_command_kind::result;
          }),
        "beat commit did not publish one aggregated result per authored effect");
  const auto attack_result = std::find_if(
    mid_commands.begin(), mid_commands.end(), [](const auto& command) {
      return command.subject == cg::presentation_subject::player_attack;
    });
  const auto status_result = std::find_if(
    mid_commands.begin(), mid_commands.end(), [](const auto& command) {
      return command.subject == cg::presentation_subject::effect;
    });
  check(attack_result != mid_commands.end() && attack_result->results.size() == 3,
        "one attack animation did not aggregate primary/reaction/retaliation outcomes");
  check(status_result != mid_commands.end() && status_result->results.size() == 1,
        "status animation did not receive its own outcome range");
  check(mid_resolution.state().enemy.hp == 96 && mid_resolution.state().player.hp == 29,
        "beat did not commit its authored effects sequentially after the shared gameplay barrier");

  // Snapshot after the complete beat commit but before its shared finished barrier. Presentation
  // tasks are dropped, while the already committed report and outcomes must not be repeated.
  cg::combat resumed_resolution(cg::run_mode::headless);
  resumed_resolution.load(mid_resolution.save());
  uint64_t resumed_resolution_tick = 0;
  drive_to_player(resumed_resolution, resumed_resolution_tick);
  check(resumed_resolution.state() == elemental_headless.state(),
        "mid-resolution resume lost or duplicated reaction/return/effect work");
  check(resumed_resolution.last_resolution() == elemental_headless.last_resolution(),
        "mid-resolution resume changed the materialized resolution trace");

  // Two attack instances create two return instances. Returned damage is marked non-recursive,
  // even when both combatants own thorns.
  cg::combat multi_hit(cg::run_mode::headless);
  uint64_t multi_hit_tick = 0;
  drive_to_player(multi_hit, multi_hit_tick);
  auto multi_snapshot = multi_hit.save();
  multi_snapshot.state.enemy.effects.push_back(
    {2000, cg::effect_kind::thorns, cg::enemy_entity, 1, 0});
  multi_snapshot.state.player.effects.push_back(
    {2001, cg::effect_kind::thorns, cg::player_entity, 1, 0});
  multi_hit.load(multi_snapshot);
  submit_and_drive(multi_hit,
                   {cg::player_intent_kind::play_card, cg::card_kind::double_strike, 1},
                   multi_hit_tick);
  check(multi_hit.state().enemy.hp == 96, "multi-hit did not apply both attack instances");
  check(multi_hit.state().player.hp == 28, "multi-hit did not return damage per attack instance");
  check(damage_count(multi_hit, cg::damage_channel::primary) == 2,
        "multi-hit attack instance count is wrong");
  check(damage_count(multi_hit, cg::damage_channel::retaliation) == 2,
        "returned damage count is wrong or recursively looped");
  check(multi_hit.last_resolution().report.effects.size() == 1 &&
          multi_hit.last_resolution().report.effects.front().plan_count == 2,
        "one double-strike authored effect did not emit two ordered attack instances");

  // Death latches after the first beat. Its already materialized work completes, but the next beat
  // gets an empty target snapshot and its authored-effect script is not invoked.
  cg::combat beat_death(cg::run_mode::headless);
  uint64_t beat_death_tick = 0;
  drive_to_player(beat_death, beat_death_tick);
  auto beat_death_snapshot = beat_death.save();
  beat_death_snapshot.state.enemy.hp = 2;
  beat_death.load(beat_death_snapshot);
  submit_and_drive(beat_death,
                   {cg::player_intent_kind::play_card, cg::card_kind::combo_strike, 1},
                   beat_death_tick);
  const auto& beat_death_report = beat_death.last_resolution().report;
  check(beat_death.state().enemy.hp == 0 &&
          damage_count(beat_death, cg::damage_channel::primary) == 1,
        "death between beats did not stop later damage work");
  check(beat_death.last_resolution().death_trace.size() == 1 &&
          beat_death.last_resolution().death_trace.front().dead,
        "lethal instance did not latch its death predicate result");
  check(beat_death_report.effects.size() == 2 &&
          beat_death_report.effects[0].beat_index == 0 &&
          beat_death_report.effects[0].invoked &&
          beat_death_report.effects[1].beat_index == 1 &&
          beat_death_report.effects[1].target_set.targets.empty() &&
          !beat_death_report.effects[1].invoked,
        "next beat was not deterministically cancelled by its dead target boundary");

  // Duplicate discovery of the same rule for one sealed hit must not allocate or commit a second
  // retaliation. The rule id is the stable effect instance id.
  cg::combat duplicate_thorns(cg::run_mode::headless);
  uint64_t duplicate_thorns_tick = 0;
  drive_to_player(duplicate_thorns, duplicate_thorns_tick);
  auto duplicate_snapshot = duplicate_thorns.save();
  duplicate_snapshot.state.enemy.effects.push_back(
    {3000, cg::effect_kind::thorns, cg::enemy_entity, 1, 0});
  duplicate_snapshot.state.enemy.effects.push_back(
    {3000, cg::effect_kind::thorns, cg::enemy_entity, 1, 0});
  duplicate_thorns.load(duplicate_snapshot);
  submit_and_drive(duplicate_thorns,
                   {cg::player_intent_kind::play_card, cg::card_kind::strike, 1},
                   duplicate_thorns_tick);
  check(duplicate_thorns.state().player.hp == 29 &&
          damage_count(duplicate_thorns, cg::damage_channel::retaliation) == 1,
        "retaliation journal did not deduplicate one rule for one hit");

  // Target policy rejects an effect explicitly; damage still resolves and the result is traceable.
  cg::combat immune(cg::run_mode::headless);
  uint64_t immune_tick = 0;
  drive_to_player(immune, immune_tick);
  auto immune_snapshot = immune.save();
  immune_snapshot.state.enemy.effect_immunity_mask =
    uint64_t{1} << static_cast<uint8_t>(cg::effect_kind::burning);
  immune.load(immune_snapshot);
  submit_and_drive(immune, fire, immune_tick);
  check(immune.state().enemy.effects.empty(), "immune target received a forbidden effect");
  check(immune.last_resolution().effect_trace.size() == 1 &&
          immune.last_resolution().effect_trace.front().result ==
            cg::effect_apply_result::immune,
        "forbidden effect did not produce an explicit immune outcome");

  // A second application updates the project-owned status entry instead of adding a duplicate.
  cg::combat effect_update(cg::run_mode::headless);
  uint64_t effect_update_tick = 0;
  drive_to_player(effect_update, effect_update_tick);
  submit_and_drive(effect_update, fire, effect_update_tick);
  auto update_snapshot = effect_update.save();
  update_snapshot.state.enemy_countdown = 2; // keep this focused action before the enemy intent
  effect_update.load(update_snapshot);
  submit_and_drive(effect_update, fire, effect_update_tick);
  check(effect_update.state().enemy.effects.size() == 1 &&
          effect_update.state().enemy.effects.front().stacks == 2,
        "second effect application duplicated the status instead of updating it");
  check(effect_update.last_resolution().effect_trace.size() == 1 &&
          effect_update.last_resolution().effect_trace.front().result ==
            cg::effect_apply_result::updated &&
          effect_update.last_resolution().effect_trace.front().previous_stacks == 1 &&
          effect_update.last_resolution().effect_trace.front().resulting_stacks == 2,
        "effect update outcome did not describe the merge");

  // Resume while the first player attack is flying and has not reached its gameplay point.
  cg::combat in_flight(cg::run_mode::animated);
  uint64_t in_flight_tick = 0;
  drive_to_player(in_flight, in_flight_tick);
  check(in_flight.submit(
          {cg::player_intent_kind::play_card, cg::card_kind::strike, 1}),
        "could not submit in-flight snapshot action");
  in_flight.update(++in_flight_tick); // publishes start and waits; no gameplay marker delivered
  check(in_flight.state().enemy.hp == 100, "damage committed before gameplay marker");
  check(in_flight.waiting_presentation(), "animated attack is not waiting at gameplay marker");

  const auto snap = in_flight.save();
  cg::combat resumed(cg::run_mode::headless);
  resumed.load(snap);
  uint64_t resumed_tick = 0;
  drive_to_player(resumed, resumed_tick);

  cg::combat control(cg::run_mode::headless);
  uint64_t control_tick = 0;
  drive_to_player(control, control_tick);
  submit_and_drive(control,
                   {cg::player_intent_kind::play_card, cg::card_kind::strike, 1},
                   control_tick);
  check(resumed.state() == control.state(),
        "resume at gameplay marker did not commit the action exactly once");

  std::printf(
    "cardgame headless/animated identity: turns=%llu actions=%llu pulses=%llu player_hp=%d enemy_hp=%d\n",
    static_cast<unsigned long long>(headless.state().turn_index),
    static_cast<unsigned long long>(headless.state().player_action_index),
    static_cast<unsigned long long>(headless.state().countdown_pulse_index),
    headless.state().player.hp,
    headless.state().enemy.hp);
  return EXIT_SUCCESS;
}
