#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include "cardgame/combat_script.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void check(const bool value, const std::string_view message) {
  if (!value) fail(message);
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
    "emit_healing = -2, emit_attribute_damage = { agility, 3 } } }");

  cg::resolution_work work;
  work.report.execution = 42;
  const std::array targets{cg::entity_id{2}, cg::entity_id{3}};
  cg::combat_effect_emit_context invocation{
    &work, cg::entity_id{1}, targets, 6, 0};
  devils_script::context vm;
  cg::run_combat_effect_script(program, vm, invocation);

  check(invocation.emitted_instances == 6 && work.plan.size() == 6,
        "DS emitters did not preserve the per-target semantic work count");
  check(work.plan == std::vector<cg::effect_instance_ref>{
                       {cg::effect_store_kind::attack, 0},
                       {cg::effect_store_kind::healing, 0},
                       {cg::effect_store_kind::attribute_damage, 0},
                       {cg::effect_store_kind::attack, 1},
                       {cg::effect_store_kind::healing, 1},
                       {cg::effect_store_kind::attribute_damage, 1}},
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
  check(work.attribute_damages.size() == 2 &&
          work.attribute_damages[0].attribute == cg::attribute_kind::agility &&
          work.attribute_damages[0].amount == 3 &&
          work.attribute_damages[1].target == 3,
        "emit_attribute_damage lost its typed attribute or target");
  check(cg::has_category(work.report.categories, cg::execution_category::attack) &&
          cg::has_category(work.report.categories, cg::execution_category::healing) &&
          cg::has_category(
            work.report.categories, cg::execution_category::attribute_change),
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

  std::puts("cardgame DS effect scope: each_target, unary minus and typed emitters OK");
  return EXIT_SUCCESS;
}
