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

retaliation_rule_emit_context& checked_invocation(
  const retaliation_rule_scope scope) {
  if (!scope.valid()) {
    throw std::invalid_argument("cardgame DS received an invalid retaliation rule scope");
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

int64_t rule_stacks(const retaliation_rule_scope scope) {
  return checked_invocation(scope).rule->stacks;
}

int64_t trigger_requested(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->route.requested;
}

int64_t trigger_delta(const retaliation_rule_scope scope) {
  const auto& route = checked_invocation(scope).trigger->route;
  return static_cast<int64_t>(route.before) - route.committed_after;
}

int64_t trigger_before(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->route.before;
}

int64_t trigger_after(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->route.committed_after;
}

int64_t trigger_clamped(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->route.clamped;
}

element trigger_element(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->damage.payload.kind;
}

uint64_t trigger_tags(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->damage.payload.tags;
}

damage_destination trigger_destination(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->damage.payload.destination;
}

damage_channel trigger_channel(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->damage.payload.channel;
}

bool trigger_dead(const retaliation_rule_scope scope) {
  return checked_invocation(scope).target_dead;
}

uint64_t trigger_instance(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->damage.header.id;
}

uint64_t trigger_root(const retaliation_rule_scope scope) {
  return checked_invocation(scope).trigger->damage.header.root;
}

entity_id trigger_source(const retaliation_rule_scope scope) {
  return static_cast<entity_id>(
    checked_invocation(scope).trigger->damage.header.source);
}

entity_id trigger_target(const retaliation_rule_scope scope) {
  return static_cast<entity_id>(
    checked_invocation(scope).trigger->damage.header.target);
}

void emit_retaliation_attack(const retaliation_rule_scope scope,
                             const int64_t amount) {
  auto& invocation = checked_invocation(scope);
  if (invocation.emitted_responses >= invocation.max_emitted_responses) {
    throw std::length_error("cardgame DS retaliation response capacity exceeded");
  }
  if (invocation.rule_ordinal == std::numeric_limits<uint16_t>::max() &&
      invocation.emitted_responses != 0) {
    throw std::length_error("cardgame DS retaliation local ordinal exceeded");
  }
  const uint16_t local_ordinal = static_cast<uint16_t>(
    invocation.rule_ordinal + invocation.emitted_responses);
  ++invocation.emitted_responses;
  const auto& trigger = invocation.trigger->damage.header;
  invocation.work->retaliation_requests.push_back(retaliation_request{
    trigger,
    invocation.rule->id,
    static_cast<entity_id>(trigger.target),
    static_cast<entity_id>(trigger.source),
    checked_amount(amount),
    local_ordinal});
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
  if (return_type != "void") {
    throw std::invalid_argument(
      "cardgame combat script resource requires ret=void");
  }
  if (scope == "combat_effect") {
    return {
      sys_.parse<void, combat_effect_scope>(name, expression),
      devils_engine::act::category::effect};
  }
  if (scope == "retaliation_rule") {
    return {
      sys_.parse<void, retaliation_rule_scope>(name, expression),
      devils_engine::act::category::effect};
  }
  throw std::invalid_argument(
    "cardgame combat script resource has an unsupported root scope");
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
  static std::array destination_values{
    std::tuple<std::string_view, damage_destination>{
      "unrouted", damage_destination::unrouted},
    std::tuple<std::string_view, damage_destination>{
      "shield", damage_destination::shield},
    std::tuple<std::string_view, damage_destination>{
      "health", damage_destination::health}};
  static std::array channel_values{
    std::tuple<std::string_view, damage_channel>{"primary", damage_channel::primary},
    std::tuple<std::string_view, damage_channel>{"reaction", damage_channel::reaction},
    std::tuple<std::string_view, damage_channel>{
      "retaliation", damage_channel::retaliation},
    std::tuple<std::string_view, damage_channel>{"periodic", damage_channel::periodic},
    std::tuple<std::string_view, damage_channel>{"cost", damage_channel::cost},
    std::tuple<std::string_view, damage_channel>{"custom", damage_channel::custom}};

  sys.register_enum<element>(element_values);
  sys.register_enum<attribute_kind>(attribute_values);
  sys.register_enum<effect_kind>(effect_values);
  sys.register_enum<damage_destination>(destination_values);
  sys.register_enum<damage_channel>(channel_values);
  sys.register_function_iter<&each_target>("each_target", {"value"});
  sys.register_function<&emit_attack>("emit_attack");
  sys.register_function<&emit_healing>("emit_healing");
  sys.register_function<&emit_attribute_damage>("emit_attribute_damage");
  sys.register_function<&emit_status>("emit_status");
  sys.register_function<&rule_stacks>("rule_stacks");
  sys.register_function<&trigger_requested>("trigger_requested");
  sys.register_function<&trigger_delta>("trigger_delta");
  sys.register_function<&trigger_before>("trigger_before");
  sys.register_function<&trigger_after>("trigger_after");
  sys.register_function<&trigger_clamped>("trigger_clamped");
  sys.register_function<&trigger_element>("trigger_element");
  sys.register_function<&trigger_tags>("trigger_tags");
  sys.register_function<&trigger_destination>("trigger_destination");
  sys.register_function<&trigger_channel>("trigger_channel");
  sys.register_function<&trigger_dead>("trigger_dead");
  sys.register_function<&trigger_instance>("trigger_instance");
  sys.register_function<&trigger_root>("trigger_root");
  sys.register_function<&trigger_source>("trigger_source");
  sys.register_function<&trigger_target>("trigger_target");
  sys.register_function<&emit_retaliation_attack>("emit_retaliation_attack");
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

void run_retaliation_rule_script(const devils_script::container& program,
                                 devils_script::context& vm,
                                 retaliation_rule_emit_context& invocation) {
  if (!invocation.valid()) {
    throw std::invalid_argument("cardgame DS retaliation invocation is invalid");
  }
  invocation.emitted_responses = 0;
  vm.clear();
  vm.userptr = &invocation;
  if (program.max_lists != 0) vm.create_lists(&program);
  vm.set_arg(program.find_arg("root"), retaliation_rule_scope{&invocation});
  program.process(&vm);
}

} // namespace core
} // namespace cardgame
