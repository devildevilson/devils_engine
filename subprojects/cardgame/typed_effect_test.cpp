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
          inverse_work.damage_preparations.size() == 1 &&
          inverse_work.damage_trace.size() == 1 &&
          inverse_work.damage_preparations.front().route.requested == -5 &&
          inverse_work.damage_preparations.front().route.modified == -5 &&
          inverse_work.damage_preparations.front().route.shield_absorbed == 0 &&
          inverse_work.damage_trace.front().damage.payload.destination ==
            cg::damage_destination::health &&
          inverse_work.damage_trace.front().route.before == 90 &&
          inverse_work.damage_trace.front().route.committed_after == 95,
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

  // A fully absorbed hit produces one shield leaf and no synthetic HP outcome. Presentation can
  // therefore distinguish shield loss from health loss without reverse engineering an aggregate.
  cg::combat shield_only(cg::run_mode::animated);
  uint64_t shield_tick = 0;
  (void)drive_to_player(shield_only, shield_tick);
  auto shield_snapshot = shield_only.save();
  shield_snapshot.state.enemy.hp = 10;
  shield_snapshot.state.enemy.shield = 5;
  shield_snapshot.state.enemy.effects.push_back(
    {3000, cg::effect_kind::thorns, cg::enemy_entity, 3, 0});
  shield_snapshot.state.enemy_countdown = 10;
  shield_only.load(shield_snapshot);
  const auto shield_commands =
    submit_and_drive(shield_only, cg::card_kind::strike, shield_tick);
  const auto& shield_work = shield_only.last_resolution();
  check(shield_only.state().enemy.hp == 10 && shield_only.state().enemy.shield == 2 &&
          shield_work.damage_preparations.size() == 1 &&
          shield_work.damage_preparations.front().route.modified == 3 &&
          shield_work.damage_preparations.front().route.shield_absorbed == 3 &&
          shield_work.damage_trace.size() == 1 &&
          shield_work.damage_trace.front().damage.payload.destination ==
            cg::damage_destination::shield &&
          shield_work.damage_trace.front().route.before == 5 &&
          shield_work.damage_trace.front().route.committed_after == 2,
        "fully absorbed damage did not produce exactly one shield leaf");
  check(shield_work.outcomes == std::vector<cg::outcome_ref>{
                                  {cg::outcome_store_kind::damage, 0}} &&
          shield_work.death_trace.size() == 1 && !shield_work.death_trace.front().dead && shield_work.damage_preparations.size() == 1,
        "shield leaf opened thorns or lost its semantic outcome/death boundary");
  const auto shield_result = std::find_if(
    shield_commands.begin(), shield_commands.end(), [](const auto& command) {
      return command.kind == cg::presentation_command_kind::result &&
             command.subject == cg::presentation_subject::player_attack;
    });
  check(shield_result != shield_commands.end() &&
          shield_result->results.size() == 1 &&
          shield_result->results.front().subject ==
            cg::presentation_subject::shield_damage &&
          shield_result->results.front().value == 3,
        "presentation did not expose the shield leaf separately");
  check(std::none_of(
          shield_commands.begin(), shield_commands.end(), [](const auto& command) {
            return command.subject == cg::presentation_subject::returned_damage;
          }),
        "shield absorption incorrectly triggered HP-loss retaliation");

  // Preserve the previous elemental policy: the terminal shield leaf can open one intrinsic
  // reaction even when neither the primary hit nor the reaction reaches HP.
  cg::combat shield_element(cg::run_mode::headless);
  uint64_t shield_element_tick = 0;
  (void)drive_to_player(shield_element, shield_element_tick);
  auto shield_element_snapshot = shield_element.save();
  shield_element_snapshot.state.enemy.shield = 10;
  shield_element_snapshot.state.enemy.elemental_mark = cg::element::water;
  shield_element_snapshot.state.enemy_countdown = 10;
  shield_element.load(shield_element_snapshot);
  (void)submit_and_drive(
    shield_element, cg::card_kind::fire_strike, shield_element_tick);
  const auto& shield_element_work = shield_element.last_resolution();
  check(shield_element.state().enemy.hp == 100 &&
          shield_element.state().enemy.shield == 4 &&
          shield_element.state().enemy.elemental_mark == cg::element::none &&
          shield_element_work.damage_preparations.size() == 2 &&
          shield_element_work.damage_trace.size() == 2 &&
          std::all_of(
            shield_element_work.damage_trace.begin(),
            shield_element_work.damage_trace.end(),
            [](const auto& outcome) {
              return outcome.damage.payload.destination ==
                     cg::damage_destination::shield;
            }),
        "terminal shield leaf did not open exactly one intrinsic elemental reaction");

  // Resistance/vulnerability is applied once to the unrouted root. The modified magnitude is then
  // partitioned into shield and HP leaves without running the modifier row again.
  cg::combat overflow(cg::run_mode::headless);
  uint64_t overflow_tick = 0;
  (void)drive_to_player(overflow, overflow_tick);
  auto overflow_snapshot = overflow.save();
  overflow_snapshot.state.enemy.hp = 10;
  overflow_snapshot.state.enemy.shield = 2;
  overflow_snapshot.state.enemy.resistance_basis_points[static_cast<size_t>(cg::element::none)] = -10000;
  overflow_snapshot.state.enemy_countdown = 10;
  overflow.load(overflow_snapshot);
  (void)submit_and_drive(overflow, cg::card_kind::strike, overflow_tick);
  const auto& overflow_work = overflow.last_resolution();
  check(overflow.state().enemy.shield == 0 && overflow.state().enemy.hp == 6 &&
          overflow_work.damage_preparations.size() == 1 &&
          overflow_work.damage_preparations.front().route.requested == 3 &&
          overflow_work.damage_preparations.front().route.modified == 6 &&
          overflow_work.damage_preparations.front().route.shield_absorbed == 2 &&
          overflow_work.damage_trace.size() == 2,
        "shield overflow changed the once-modified attack magnitude");
  const auto& shield_leaf = overflow_work.damage_trace[0];
  const auto& health_leaf = overflow_work.damage_trace[1];
  check(shield_leaf.damage.payload.destination == cg::damage_destination::shield &&
          shield_leaf.damage.payload.amount == 2 &&
          shield_leaf.route.before == 2 && shield_leaf.route.committed_after == 0 &&
          health_leaf.damage.payload.destination == cg::damage_destination::health &&
          health_leaf.damage.payload.amount == 4 &&
          health_leaf.route.before == 10 && health_leaf.route.committed_after == 6 &&
          shield_leaf.damage.header.parent ==
            overflow_work.damage_preparations.front().damage.header.id &&
          health_leaf.damage.header.parent == shield_leaf.damage.header.parent &&
          shield_leaf.damage.header.id < health_leaf.damage.header.id,
        "routed leaves lost shield→HP order, amounts or provenance");
  check(overflow_work.outcomes == std::vector<cg::outcome_ref>{
                                    {cg::outcome_store_kind::damage, 0},
                                    {cg::outcome_store_kind::damage, 1}} &&
          overflow_work.death_trace.size() == 2 && overflow_work.report.effects.front().damage_outcome_count == 2,
        "shield overflow did not expose both leaf outcomes to report/death listeners");

  // Retaliation remains one immediate authored attack, but its result can contain both routed
  // leaves. Its lineage prevents the returned HP leaf from opening another retaliation.
  cg::combat routed_retaliation(cg::run_mode::animated);
  uint64_t retaliation_tick = 0;
  (void)drive_to_player(routed_retaliation, retaliation_tick);
  auto retaliation_snapshot = routed_retaliation.save();
  retaliation_snapshot.state.player.shield = 1;
  retaliation_snapshot.state.enemy.effects.push_back(
    {3001, cg::effect_kind::thorns, cg::enemy_entity, 3, 0});
  retaliation_snapshot.state.player.effects.push_back(
    {3002, cg::effect_kind::thorns, cg::player_entity, 3, 0});
  retaliation_snapshot.state.enemy_countdown = 10;
  routed_retaliation.load(retaliation_snapshot);
  const auto retaliation_commands = submit_and_drive(
    routed_retaliation, cg::card_kind::strike, retaliation_tick);
  const auto& retaliation_work = routed_retaliation.last_resolution();
  check(routed_retaliation.state().player.shield == 0 &&
          routed_retaliation.state().player.hp == 28 &&
          retaliation_work.damage_preparations.size() == 2 &&
          retaliation_work.damage_trace.size() == 3 &&
          std::count_if(
            retaliation_work.damage_trace.begin(),
            retaliation_work.damage_trace.end(),
            [](const auto& outcome) {
              return outcome.damage.payload.channel ==
                     cg::damage_channel::retaliation;
            }) == 2,
        "routed retaliation did not commit shield+HP exactly once");
  const auto retaliation_result = std::find_if(
    retaliation_commands.begin(), retaliation_commands.end(), [](const auto& command) {
      return command.kind == cg::presentation_command_kind::result &&
             command.subject == cg::presentation_subject::returned_damage;
    });
  check(retaliation_result != retaliation_commands.end() &&
          retaliation_result->results.size() == 2 &&
          retaliation_result->results[0].subject ==
            cg::presentation_subject::shield_damage &&
          retaliation_result->results[0].value == 1 &&
          retaliation_result->results[1].subject ==
            cg::presentation_subject::returned_damage &&
          retaliation_result->results[1].value == 2,
        "retaliation animation did not expose shield→HP routed results");

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

  std::puts("cardgame typed effects: routed shield/HP, healing and agility damage OK");
  return EXIT_SUCCESS;
}
