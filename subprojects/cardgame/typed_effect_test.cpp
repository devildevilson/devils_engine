#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cardgame/combat.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "cardgame_typed_effect_test: %s\n", message);
  std::exit(EXIT_FAILURE);
}

void check(const bool value, const char* message) {
  if (!value) fail(message);
}

std::vector<cg::presentation_command> drive_to_player(
  cg::combat& game,
  uint64_t& engine_tick) {
  std::vector<cg::presentation_command> observed;
  for (uint32_t guard = 0; guard < 256; ++guard) {
    game.update(++engine_tick);
    auto commands = game.take_presentation_commands();
    for (const auto& command : commands) {
      const auto event = command.kind == cg::presentation_command_kind::start
                           ? devils_engine::simul::presentation_event_kind::gameplay
                           : devils_engine::simul::presentation_event_kind::finished;
      check(game.notify_presentation(command.task, event),
            "fake presentation produced an unexpected event");
    }
    observed.insert(observed.end(), commands.begin(), commands.end());
    if (game.awaiting_player()) return observed;
    check(!game.faulted(), "typed effect pipeline faulted");
  }
  fail("typed effect pipeline did not return to player input");
}

std::vector<cg::presentation_command> submit_and_drive(
  cg::combat& game,
  const cg::card_kind card,
  uint64_t& tick) {
  check(game.submit({cg::player_intent_kind::play_card, card, tick + 1}),
        "typed card intent was rejected");
  return drive_to_player(game, tick);
}

size_t trace_count(const cg::combat_state& state, const cg::combat_trace_kind kind) {
  return static_cast<size_t>(std::count_if(
    state.trace.begin(), state.trace.end(), [kind](const auto& event) {
      return event.kind == kind;
    }));
}

} // namespace

int main() {
  // A negative attack remains attack/damage and uses damage resistance/routing. It does not consume
  // shield and must not silently become a healing instance.
  cg::combat inverse(cg::run_mode::headless);
  uint64_t inverse_tick = 0;
  (void)drive_to_player(inverse, inverse_tick);
  auto inverse_snapshot = inverse.save();
  inverse_snapshot.state.enemy.hp = 90;
  inverse_snapshot.state.enemy.shield = 4;
  inverse_snapshot.state.enemy_countdown = 10;
  inverse.load(inverse_snapshot);
  (void)submit_and_drive(inverse, cg::card_kind::inverse_strike, inverse_tick);
  const auto& inverse_work = inverse.last_resolution();
  check(inverse.state().enemy.hp == 95 && inverse.state().enemy.shield == 4 &&
          inverse_work.damage_trace.size() == 1 &&
          inverse_work.damage_trace.front().route.requested == -5 &&
          inverse_work.damage_trace.front().route.modified == -5 &&
          inverse_work.damage_trace.front().route.shield_absorbed == 0 &&
          inverse_work.damage_trace.front().route.committed_hp_after == 95,
        "negative attack was clamped, retyped or routed through shield");
  check(cg::has_category(
          inverse_work.report.categories, cg::execution_category::attack) &&
          cg::has_category(
            inverse_work.report.categories, cg::execution_category::damage) &&
          cg::has_category(
            inverse_work.report.categories, cg::execution_category::stat_change) &&
          !cg::has_category(
            inverse_work.report.categories, cg::execution_category::healing),
        "negative attack changed semantic category based on its sign");

  // Healing owns its target domain and resistance/effectiveness route. Presentation sees the same
  // signed committed delta as headless, while the semantic category remains healing.
  cg::combat healing_headless(cg::run_mode::headless);
  cg::combat healing_animated(cg::run_mode::animated);
  uint64_t healing_headless_tick = 0;
  uint64_t healing_animated_tick = 0;
  (void)drive_to_player(healing_headless, healing_headless_tick);
  (void)drive_to_player(healing_animated, healing_animated_tick);
  auto healing_snapshot = healing_headless.save();
  healing_snapshot.state.player.hp = 20;
  healing_snapshot.state.player.healing_effectiveness_basis_points = 5000;
  healing_snapshot.state.enemy_countdown = 10;
  healing_headless.load(healing_snapshot);
  healing_animated.load(healing_snapshot);

  (void)submit_and_drive(healing_headless, cg::card_kind::mend, healing_headless_tick);
  const auto healing_commands =
    submit_and_drive(healing_animated, cg::card_kind::mend, healing_animated_tick);
  check(healing_headless.state() == healing_animated.state() &&
          healing_headless.last_resolution() == healing_animated.last_resolution(),
        "healing diverged between headless and animated execution");
  check(healing_headless.state().player.hp == 23,
        "healing effectiveness did not scale the positive healing instance");
  const auto& healing_work = healing_headless.last_resolution();
  check(healing_work.healing_trace.size() == 1 &&
          healing_work.healing_trace.front().healing.amount == 6 &&
          healing_work.healing_trace.front().route.requested == 6 &&
          healing_work.healing_trace.front().route.modified == 3 &&
          healing_work.healing_trace.front().route.before == 20 &&
          healing_work.healing_trace.front().route.committed_after == 23 &&
          healing_work.healing_trace.front().committed,
        "positive healing outcome lost its typed route");
  check(healing_work.outcomes == std::vector<cg::outcome_ref>{
                                   {cg::outcome_store_kind::healing, 0}},
        "healing outcome was not inserted into semantic outcome order");
  check(healing_work.report.effects.size() == 1 &&
          healing_work.report.effects.front().target_set.targets ==
            std::vector<cg::entity_id>{cg::player_entity} &&
          healing_work.report.effects.front().healing_outcome_count == 1 &&
          healing_work.report.effects.front().outcome_count == 1 &&
          cg::has_category(
            healing_work.report.categories, cg::execution_category::healing) &&
          cg::has_category(
            healing_work.report.categories, cg::execution_category::stat_change) &&
          !cg::has_category(
            healing_work.report.categories, cg::execution_category::damage),
        "healing report lost its self target, typed range or categories");
  const auto healing_result = std::find_if(
    healing_commands.begin(), healing_commands.end(), [](const auto& command) {
      return command.kind == cg::presentation_command_kind::result;
    });
  check(healing_result != healing_commands.end() &&
          healing_result->subject == cg::presentation_subject::healing &&
          healing_result->results.size() == 1 &&
          healing_result->results.front().value == 3,
        "healing presentation did not receive its committed signed outcome");

  // Negative healing is still healing, uses the healing effectiveness path and can latch death.
  cg::combat negative(cg::run_mode::headless);
  uint64_t negative_tick = 0;
  (void)drive_to_player(negative, negative_tick);
  auto negative_snapshot = negative.save();
  negative_snapshot.state.player.hp = 5;
  negative_snapshot.state.enemy_countdown = 10;
  negative.load(negative_snapshot);
  const size_t tick_count_before =
    trace_count(negative.state(), cg::combat_trace_kind::actor_state_tick);
  (void)submit_and_drive(negative, cg::card_kind::cursed_mend, negative_tick);
  const auto& negative_work = negative.last_resolution();
  check(negative.state().player.hp == 0 &&
          negative_work.healing_trace.size() == 1 &&
          negative_work.healing_trace.front().healing.amount == -7 &&
          negative_work.healing_trace.front().route.modified == -7 &&
          negative_work.healing_trace.front().route.proposed_after == -2 &&
          negative_work.healing_trace.front().route.committed_after == 0,
        "negative healing was retyped or did not use the healing route");
  check(negative_work.death_trace.size() == 1 &&
          negative_work.death_trace.front().kind == cg::outcome_store_kind::healing &&
          negative_work.death_trace.front().dead &&
          trace_count(negative.state(), cg::combat_trace_kind::actor_state_tick) ==
            tick_count_before,
        "negative healing did not latch death before ActorStateTick");
  check(cg::has_category(
          negative_work.report.categories, cg::execution_category::healing) &&
          !cg::has_category(
            negative_work.report.categories, cg::execution_category::damage),
        "negative healing changed semantic category based on its sign");

  // Attribute damage has its own resistance and outcome store; it is a stat change but not HP
  // damage. A 50% resistance turns agility damage 4 into 2.
  cg::combat attribute(cg::run_mode::headless);
  uint64_t attribute_tick = 0;
  (void)drive_to_player(attribute, attribute_tick);
  auto attribute_snapshot = attribute.save();
  attribute_snapshot.state.enemy_countdown = 10;
  attribute_snapshot.state.enemy.attribute_resistance_basis_points[static_cast<size_t>(cg::attribute_kind::agility)] = 5000;
  attribute.load(attribute_snapshot);
  (void)submit_and_drive(attribute, cg::card_kind::cripple, attribute_tick);
  const auto& attribute_work = attribute.last_resolution();
  check(attribute.state().enemy.agility == 8 &&
          attribute_work.attribute_damage_trace.size() == 1 &&
          attribute_work.attribute_damage_trace.front().route.requested == 4 &&
          attribute_work.attribute_damage_trace.front().route.modified == 2 &&
          attribute_work.attribute_damage_trace.front().route.before == 10 &&
          attribute_work.attribute_damage_trace.front().route.committed_after == 8,
        "agility damage did not use its typed resistance route");
  check(attribute_work.outcomes == std::vector<cg::outcome_ref>{
                                     {cg::outcome_store_kind::attribute_damage, 0}} &&
          attribute_work.death_trace.size() == 1 && attribute_work.death_trace.front().kind == cg::outcome_store_kind::attribute_damage && cg::has_category(attribute_work.report.categories, cg::execution_category::attribute_change) && cg::has_category(attribute_work.report.categories, cg::execution_category::stat_change) && !cg::has_category(attribute_work.report.categories, cg::execution_category::damage),
        "attribute damage lost semantic outcome order, death check or categories");

  std::puts("cardgame typed effects: healing signs/resistance/death and agility damage OK");
  return EXIT_SUCCESS;
}
