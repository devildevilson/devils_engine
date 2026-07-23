#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include <devils_engine/utils/core.h>

#include "cardgame/combat_script.h"
#include "script_test_fixture.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void check(const bool value, const std::string_view message) {
  if (!value) fail(message);
}

template <typename Ret, typename Scope>
Ret evaluate(devils_script::system& sys,
             const std::string_view name,
             const std::string_view expression,
             const Scope scope) {
  const auto program = sys.parse<Ret, Scope>(name, expression);
  devils_script::context vm;
  vm.set_arg(program.find_arg("root"), scope);
  program.process(&vm);
  if (!vm.template is_return<Ret>()) {
    throw std::runtime_error(
      std::string("execution report expression returned the wrong type: ") +
      std::string(name));
  }
  return vm.template get_return<Ret>();
}

cg::resolution_work make_report_work() {
  cg::resolution_work work;
  work.report.execution = 77;
  work.report.actor = cg::player_entity;
  work.report.selected_target = cg::enemy_entity;
  work.report.executed = true;
  cg::add_category(work.report.categories, cg::execution_category::attack);
  cg::add_category(work.report.categories, cg::execution_category::damage);
  cg::add_category(work.report.categories, cg::execution_category::healing);
  cg::add_category(work.report.categories, cg::execution_category::shield);
  cg::add_category(work.report.categories, cg::execution_category::attribute_change);
  cg::add_category(work.report.categories, cg::execution_category::status);
  cg::add_category(work.report.categories, cg::execution_category::stat_change);

  work.attacks.push_back(cg::attack_instance{
    9, 77, cg::player_entity, cg::enemy_entity, cg::element::fire, 6, true});
  work.healings.push_back(cg::healing_instance{
    10, 77, cg::player_entity, cg::player_entity, 4});
  work.shields.push_back(cg::shield_instance{
    11, 77, cg::player_entity, cg::player_entity, 5});
  work.attribute_damages.push_back(cg::attribute_damage_instance{
    12,
    77,
    cg::player_entity,
    cg::enemy_entity,
    cg::attribute_kind::agility,
    3});
  work.effects.push_back(cg::effect_request{
    13,
    77,
    cg::player_entity,
    cg::enemy_entity,
    cg::effect_kind::burning,
    2,
    3});
  work.plan = {
    {cg::effect_store_kind::attack, 0},
    {cg::effect_store_kind::healing, 0},
    {cg::effect_store_kind::shield, 0},
    {cg::effect_store_kind::attribute_damage, 0},
    {cg::effect_store_kind::effect, 0}};

  cg::damage_outcome damage;
  damage.damage.header.id = 14;
  damage.damage.header.root = 9;
  damage.damage.header.source = cg::player_entity;
  damage.damage.header.target = cg::enemy_entity;
  damage.damage.payload = cg::damage_payload{
    cg::element::fire,
    6,
    cg::damage_channel::primary,
    3,
    cg::damage_destination::health};
  damage.route = cg::stat_change_route{6, 6, 20, 14, 14, 0};
  damage.target_valid = true;
  damage.committed = true;
  work.damage_trace.push_back(damage);

  work.healing_trace.push_back(cg::healing_outcome{
    work.healings.front(),
    cg::stat_change_route{4, 4, 10, 14, 14, 0},
    10000,
    true,
    true});
  work.shield_trace.push_back(cg::shield_outcome{
    work.shields.front(),
    cg::stat_change_route{5, 5, 1, 6, 6, 0},
    true,
    true});
  work.attribute_damage_trace.push_back(cg::attribute_damage_outcome{
    work.attribute_damages.front(),
    cg::stat_change_route{3, 3, 8, 5, 5, 0},
    0,
    true,
    true});
  work.effect_trace.push_back(cg::effect_outcome{
    work.effects.front(), cg::effect_apply_result::added, 0, 2});
  work.outcomes = {
    {cg::outcome_store_kind::damage, 0},
    {cg::outcome_store_kind::healing, 0},
    {cg::outcome_store_kind::shield, 0},
    {cg::outcome_store_kind::attribute_damage, 0},
    {cg::outcome_store_kind::effect, 0}};

  cg::authored_effect_report effect;
  effect.id = 21;
  effect.invoked = true;
  effect.plan_begin = 0;
  effect.plan_count = work.plan.size();
  effect.outcome_begin = 0;
  effect.outcome_count = work.outcomes.size();
  effect.damage_outcome_count = 1;
  effect.healing_outcome_count = 1;
  effect.shield_outcome_count = 1;
  effect.attribute_outcome_count = 1;
  effect.effect_outcome_count = 1;
  work.report.effects.push_back(effect);
  return work;
}

} // namespace

int main() {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  cg::register_combat_effect_script(sys);

  cg::resolution_work work = make_report_work();
  cg::execution_report_view_context view{&work, &work.report};
  const cg::execution_report_scope root{&view};

  check(evaluate<uint64_t>(sys, "report_execution", "execution", root) == 77 &&
          evaluate<cg::entity_id>(sys, "report_actor", "actor", root) ==
            cg::player_entity &&
          evaluate<cg::entity_id>(
            sys, "report_target", "selected_target", root) == cg::enemy_entity &&
          evaluate<bool>(sys, "report_executed", "executed", root),
        "execution report metadata was not visible to DS");
  check(evaluate<bool>(
          sys,
          "report_category",
          "has_category = category_shield",
          root),
        "execution report category mask was not visible to DS");
  check(evaluate<int64_t>(sys, "attack_count", "attack_count", root) == 1 &&
          evaluate<int64_t>(sys, "damage_count", "damage_count", root) == 1 &&
          evaluate<int64_t>(sys, "healing_count", "healing_count", root) == 1 &&
          evaluate<int64_t>(sys, "shield_count", "shield_count", root) == 1 &&
          evaluate<int64_t>(
            sys, "attribute_count", "attribute_damage_count", root) == 1 &&
          evaluate<int64_t>(sys, "status_count", "status_count", root) == 1,
        "execution report typed counts lost semantic refs");

  const cg::report_attack_scope attack{&view, &work.attacks.front()};
  check(evaluate<int64_t>(sys, "attack_requested", "requested", attack) == 6 &&
          evaluate<int64_t>(sys, "attack_element", "element", attack) ==
            static_cast<int64_t>(cg::element::fire) &&
          evaluate<bool>(
            sys, "attack_applies_element", "applies_element", attack),
        "attack report scope lost its typed instance fields");
  const cg::report_damage_scope damage{&view, &work.damage_trace.front()};
  check(evaluate<int64_t>(sys, "damage_delta", "delta", damage) == 6 &&
          evaluate<int64_t>(
            sys, "damage_destination", "destination", damage) ==
            static_cast<int64_t>(cg::damage_destination::health) &&
          evaluate<int64_t>(sys, "damage_channel", "channel", damage) ==
            static_cast<int64_t>(cg::damage_channel::primary),
        "damage report scope lost its routed outcome fields");
  const cg::report_healing_scope healing{&view, &work.healing_trace.front()};
  const cg::report_shield_scope shield{&view, &work.shield_trace.front()};
  const cg::report_attribute_damage_scope attribute{
    &view, &work.attribute_damage_trace.front()};
  const cg::report_status_scope status{&view, &work.effect_trace.front()};
  check(evaluate<int64_t>(sys, "healing_delta", "delta", healing) == 4 &&
          evaluate<int64_t>(sys, "shield_delta", "delta", shield) == 5 &&
          evaluate<int64_t>(
            sys, "attribute_kind", "attribute", attribute) ==
            static_cast<int64_t>(cg::attribute_kind::agility) &&
          evaluate<int64_t>(sys, "status_result", "status_result", status) ==
            static_cast<int64_t>(cg::effect_apply_result::added),
        "typed stat/status report scopes lost their outcome fields");

  cardgame::test::script_fixture scripts;
  const auto* probe = scripts.scripts.find(
    devils_engine::utils::string_hash("scripts/report_probe"));
  check(probe != nullptr && !probe->cmds.empty(),
        "resource-backed execution report consumer did not compile");
  devils_script::context vm;
  cg::run_execution_report_script(*probe, vm, view);

  cg::resolution_work resumed_work = work;
  cg::execution_report_view_context resumed{
    &resumed_work, &resumed_work.report};
  cg::run_execution_report_script(*probe, vm, resumed);
  check(evaluate<int64_t>(
          sys,
          "resumed_damage_count",
          "damage_count",
          cg::execution_report_scope{&resumed}) == 1,
        "execution report view did not rebind after snapshot-style copy");

  cg::resolution_work corrupt = work;
  corrupt.report.effects.front().plan_count = corrupt.plan.size() + 1;
  cg::execution_report_view_context corrupt_view{&corrupt, &corrupt.report};
  bool corrupt_failed = false;
  try {
    cg::run_execution_report_script(*probe, vm, corrupt_view);
  } catch (const std::out_of_range&) {
    corrupt_failed = true;
  }
  check(corrupt_failed, "invalid execution report range did not fail loudly");

  std::puts(
    "cardgame execution report DS scope: metadata, typed views and resume OK");
  return EXIT_SUCCESS;
}
