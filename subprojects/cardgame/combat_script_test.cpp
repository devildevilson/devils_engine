#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

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

void drive_to_player(cg::combat& game, uint64_t& tick) {
  for (uint32_t guard = 0; guard < 256; ++guard) {
    game.update(++tick);
    const auto commands = game.take_presentation_commands();
    for (const auto& command : commands) {
      const auto event = command.kind == cg::presentation_command_kind::start
                           ? devils_engine::simul::presentation_event_kind::gameplay
                           : devils_engine::simul::presentation_event_kind::finished;
      check(game.notify_presentation(command.task, event),
            "scripted card produced an unexpected presentation event");
    }
    if (game.awaiting_player()) return;
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
    "emit_healing = -2, emit_attribute_damage = { agility, 3 }, "
    "emit_status = { burning, 2, 3 } } }");

  cg::resolution_work work;
  work.report.execution = 42;
  const std::array targets{cg::entity_id{2}, cg::entity_id{3}};
  cg::combat_effect_emit_context invocation{
    &work, cg::entity_id{1}, targets, 8, 0};
  devils_script::context vm;
  cg::run_combat_effect_script(program, vm, invocation);

  check(invocation.emitted_instances == 8 && work.plan.size() == 8,
        "DS emitters did not preserve the per-target semantic work count");
  check(work.plan == std::vector<cg::effect_instance_ref>{
                       {cg::effect_store_kind::attack, 0},
                       {cg::effect_store_kind::healing, 0},
                       {cg::effect_store_kind::attribute_damage, 0},
                       {cg::effect_store_kind::effect, 0},
                       {cg::effect_store_kind::attack, 1},
                       {cg::effect_store_kind::healing, 1},
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
  cg::combat_effect_script_resources script_resources(resources);

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
    missing_failed = std::string_view(error.what()).find(
                       "requires a combat effect script provider") !=
                     std::string_view::npos;
  }
  check(missing_failed,
        "resource-backed card silently fell back when no provider was installed");

  std::puts(
    "cardgame DS effect scope: typed emitters, resource card and resume OK");
  return EXIT_SUCCESS;
}
