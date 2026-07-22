#include "cardgame/combat_script.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include <tavl/parser.h>

#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/resource_system.h>

namespace cardgame {
namespace core {
namespace {

int32_t checked_amount(const int64_t value) {
  if (value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max()) {
    throw std::out_of_range("cardgame DS emitted amount does not fit int32");
  }
  return static_cast<int32_t>(value);
}

combat_effect_emit_context& checked_invocation(const combat_effect_scope scope) {
  if (!scope.valid()) {
    throw std::invalid_argument("cardgame DS iterator received an invalid effect scope");
  }
  return *scope.invocation;
}

combat_effect_emit_context& checked_invocation(const combat_target_scope scope) {
  if (!scope.valid()) {
    throw std::invalid_argument("cardgame DS typed emitter received an invalid target scope");
  }
  return *scope.invocation;
}

void reserve_instances(combat_effect_emit_context& invocation, const size_t count) {
  const size_t remaining = invocation.emitted_instances < invocation.max_emitted_instances
                             ? invocation.max_emitted_instances - invocation.emitted_instances
                             : 0;
  if (count > remaining) {
    throw std::length_error("cardgame DS authored-effect emission capacity exceeded");
  }
  invocation.emitted_instances += count;
}

void each_target(const combat_effect_scope scope,
                 const devils_script::script_function<void(combat_target_scope)> value) {
  auto& invocation = checked_invocation(scope);
  if (!value) {
    throw std::invalid_argument("cardgame DS each_target requires a value effect");
  }
  for (const entity_id target : invocation.target_snapshot) {
    if (target == invalid_entity) {
      throw std::invalid_argument("cardgame DS target snapshot contains an invalid entity");
    }
    value(combat_target_scope{&invocation, target});
  }
}

void emit_attack(const combat_target_scope scope,
                 const int64_t amount,
                 const element kind,
                 const bool applies_element) {
  auto& invocation = checked_invocation(scope);
  reserve_instances(invocation, 1);
  const int32_t checked = checked_amount(amount);
  const size_t index = invocation.work->attacks.size();
  invocation.work->attacks.push_back(attack_instance{
    0,
    invocation.work->report.execution,
    invocation.source,
    scope.target,
    kind,
    checked,
    applies_element});
  invocation.work->plan.push_back(
    effect_instance_ref{effect_store_kind::attack, index});
  add_category(invocation.work->report.categories, execution_category::attack);
}

void emit_healing(const combat_target_scope scope, const int64_t amount) {
  auto& invocation = checked_invocation(scope);
  reserve_instances(invocation, 1);
  const int32_t checked = checked_amount(amount);
  const size_t index = invocation.work->healings.size();
  invocation.work->healings.push_back(healing_instance{
    0,
    invocation.work->report.execution,
    invocation.source,
    scope.target,
    checked});
  invocation.work->plan.push_back(
    effect_instance_ref{effect_store_kind::healing, index});
  add_category(invocation.work->report.categories, execution_category::healing);
}

void emit_attribute_damage(const combat_target_scope scope,
                           const attribute_kind attribute,
                           const int64_t amount) {
  if (attribute == attribute_kind::count) {
    throw std::invalid_argument("cardgame DS emitted an invalid attribute kind");
  }
  auto& invocation = checked_invocation(scope);
  reserve_instances(invocation, 1);
  const int32_t checked = checked_amount(amount);
  const size_t index = invocation.work->attribute_damages.size();
  invocation.work->attribute_damages.push_back(attribute_damage_instance{
    0,
    invocation.work->report.execution,
    invocation.source,
    scope.target,
    attribute,
    checked});
  invocation.work->plan.push_back(
    effect_instance_ref{effect_store_kind::attribute_damage, index});
  add_category(
    invocation.work->report.categories, execution_category::attribute_change);
}

void emit_status(const combat_target_scope scope,
                 const effect_kind kind,
                 const int64_t stacks,
                 const int64_t remaining_pulses) {
  if (kind == effect_kind::count) {
    throw std::invalid_argument("cardgame DS emitted an invalid status kind");
  }
  auto& invocation = checked_invocation(scope);
  reserve_instances(invocation, 1);
  const size_t index = invocation.work->effects.size();
  invocation.work->effects.push_back(effect_request{
    0,
    invocation.work->report.execution,
    invocation.source,
    scope.target,
    kind,
    checked_amount(stacks),
    checked_amount(remaining_pulses)});
  invocation.work->plan.push_back(
    effect_instance_ref{effect_store_kind::effect, index});
  add_category(invocation.work->report.categories, execution_category::status);
}

} // namespace

combat_effect_script_compiler::combat_effect_script_compiler() {
  sys_.init_basic_functions();
  sys_.init_math();
  register_combat_effect_script(sys_);
}

combat_effect_script_resources::combat_effect_script_resources(
  const devils_engine::demiurg::resource_system& resources) noexcept
  : resources_(&resources) {}

const devils_script::container* combat_effect_script_resources::find(
  const devils_engine::utils::id script) const noexcept {
  auto* base = resources_->get(script);
  if (base == nullptr ||
      !base->is_type(
        devils_engine::utils::type_id<devils_engine::act::script_resource>())) {
    return nullptr;
  }
  auto* resource = static_cast<devils_engine::act::script_resource*>(base);
  if (resource->category() != devils_engine::act::category::effect) {
    return nullptr;
  }
  return resource->program();
}

void combat_effect_script_compiler::configure_parser(tavl::parser& parser) const {
  sys_.configure_parser(parser);
}

devils_engine::act::compiled_script combat_effect_script_compiler::compile(
  const std::string_view name,
  const std::string_view return_type,
  const std::string_view scope,
  const std::string_view expression) const {
  if (return_type != "void" || scope != "combat_effect") {
    throw std::invalid_argument(
      "cardgame script resource requires ret=void and scope=combat_effect");
  }
  return {
    sys_.parse<void, combat_effect_scope>(name, expression),
    devils_engine::act::category::effect};
}

devils_script::container combat_effect_script_compiler::compile_predicate(
  const std::string_view,
  tavl::parser&) const {
  throw std::invalid_argument(
    "cardgame combat-effect compiler does not support predicate roots");
}

devils_script::container combat_effect_script_compiler::compile_effect(
  const std::string_view name,
  tavl::parser& parser) const {
  devils_script::container program;
  devils_script::system::parse_context ctx;
  sys_.parse<void, combat_effect_scope>(name, parser, ctx, program);
  return program;
}

void register_combat_effect_script(devils_script::system& sys) {
  static std::array element_values{
    std::tuple<std::string_view, element>{"none", element::none},
    std::tuple<std::string_view, element>{"fire", element::fire},
    std::tuple<std::string_view, element>{"water", element::water},
    std::tuple<std::string_view, element>{"ice", element::ice}};
  static std::array attribute_values{
    std::tuple<std::string_view, attribute_kind>{"agility", attribute_kind::agility}};
  static std::array effect_values{
    std::tuple<std::string_view, effect_kind>{"burning", effect_kind::burning},
    std::tuple<std::string_view, effect_kind>{"thorns", effect_kind::thorns}};

  sys.register_enum<element>(element_values);
  sys.register_enum<attribute_kind>(attribute_values);
  sys.register_enum<effect_kind>(effect_values);
  sys.register_function_iter<&each_target>("each_target", {"value"});
  sys.register_function<&emit_attack>("emit_attack");
  sys.register_function<&emit_healing>("emit_healing");
  sys.register_function<&emit_attribute_damage>("emit_attribute_damage");
  sys.register_function<&emit_status>("emit_status");
}

void run_combat_effect_script(const devils_script::container& program,
                              devils_script::context& vm,
                              combat_effect_emit_context& invocation) {
  if (!invocation.valid()) {
    throw std::invalid_argument("cardgame DS emit invocation is invalid");
  }
  invocation.emitted_instances = 0;
  vm.clear();
  vm.userptr = &invocation;
  if (program.max_lists != 0) vm.create_lists(&program);
  vm.set_arg(program.find_arg("root"), combat_effect_scope{&invocation});
  program.process(&vm);
}

} // namespace core
} // namespace cardgame
