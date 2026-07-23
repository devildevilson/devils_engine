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

void emit_shield(const combat_target_scope scope, const int64_t amount) {
  auto& invocation = checked_invocation(scope);
  reserve_instances(invocation, 1);
  const int32_t checked = checked_amount(amount);
  const size_t index = invocation.work->shields.size();
  invocation.work->shields.push_back(shield_instance{
    0,
    invocation.work->report.execution,
    invocation.source,
    scope.target,
    checked});
  invocation.work->plan.push_back(
    effect_instance_ref{effect_store_kind::shield, index});
  add_category(invocation.work->report.categories, execution_category::shield);
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

execution_report_view_context& checked_invocation(
  const execution_report_scope scope) {
  if (!scope.valid()) {
    throw std::invalid_argument("cardgame DS received an invalid execution report scope");
  }
  return *scope.invocation;
}

template <typename Scope>
execution_report_view_context& checked_report_item(const Scope scope) {
  if (!scope.valid()) {
    throw std::invalid_argument("cardgame DS received an invalid report item scope");
  }
  return *scope.invocation;
}

template <typename Fn>
void for_each_report_plan(
  execution_report_view_context& invocation, Fn&& fn) {
  for (const authored_effect_report& effect : invocation.report->effects) {
    if (effect.plan_begin > invocation.work->plan.size() ||
        effect.plan_count > invocation.work->plan.size() - effect.plan_begin) {
      throw std::out_of_range("cardgame execution report plan range is invalid");
    }
    for (size_t i = 0; i < effect.plan_count; ++i) {
      fn(invocation.work->plan[effect.plan_begin + i]);
    }
  }
}

template <typename Fn>
void for_each_report_outcome(
  execution_report_view_context& invocation, Fn&& fn) {
  for (const authored_effect_report& effect : invocation.report->effects) {
    if (effect.outcome_begin > invocation.work->outcomes.size() ||
        effect.outcome_count >
          invocation.work->outcomes.size() - effect.outcome_begin) {
      throw std::out_of_range("cardgame execution report outcome range is invalid");
    }
    for (size_t i = 0; i < effect.outcome_count; ++i) {
      fn(invocation.work->outcomes[effect.outcome_begin + i]);
    }
  }
}

uint64_t report_execution(const execution_report_scope scope) {
  return checked_invocation(scope).report->execution;
}

entity_id report_actor(const execution_report_scope scope) {
  return checked_invocation(scope).report->actor;
}

entity_id report_selected_target(const execution_report_scope scope) {
  return checked_invocation(scope).report->selected_target;
}

bool report_executed(const execution_report_scope scope) {
  return checked_invocation(scope).report->executed;
}

bool report_has_category(
  const execution_report_scope scope, const execution_category category) {
  return has_category(checked_invocation(scope).report->categories, category);
}

int64_t report_attack_count(const execution_report_scope scope) {
  auto& invocation = checked_invocation(scope);
  int64_t count = 0;
  for_each_report_plan(invocation, [&](const effect_instance_ref ref) {
    if (ref.kind == effect_store_kind::attack) ++count;
  });
  return count;
}

template <outcome_store_kind Kind>
int64_t report_outcome_count(const execution_report_scope scope) {
  auto& invocation = checked_invocation(scope);
  int64_t count = 0;
  for_each_report_outcome(invocation, [&](const outcome_ref ref) {
    if (ref.kind == Kind) ++count;
  });
  return count;
}

void each_report_attack(
  const execution_report_scope scope,
  const devils_script::script_function<void(report_attack_scope)> value) {
  auto& invocation = checked_invocation(scope);
  if (!value) {
    throw std::invalid_argument("cardgame DS each_attack requires a value effect");
  }
  for_each_report_plan(invocation, [&](const effect_instance_ref ref) {
    if (ref.kind != effect_store_kind::attack) return;
    if (ref.index >= invocation.work->attacks.size()) {
      throw std::out_of_range("cardgame execution report attack ref is invalid");
    }
    value(report_attack_scope{&invocation, &invocation.work->attacks[ref.index]});
  });
}

void each_report_damage(
  const execution_report_scope scope,
  const devils_script::script_function<void(report_damage_scope)> value) {
  auto& invocation = checked_invocation(scope);
  if (!value) {
    throw std::invalid_argument("cardgame DS each_damage requires a value effect");
  }
  for_each_report_outcome(invocation, [&](const outcome_ref ref) {
    if (ref.kind != outcome_store_kind::damage) return;
    if (ref.index >= invocation.work->damage_trace.size()) {
      throw std::out_of_range("cardgame execution report damage ref is invalid");
    }
    value(report_damage_scope{
      &invocation, &invocation.work->damage_trace[ref.index]});
  });
}

void each_report_healing(
  const execution_report_scope scope,
  const devils_script::script_function<void(report_healing_scope)> value) {
  auto& invocation = checked_invocation(scope);
  if (!value) {
    throw std::invalid_argument("cardgame DS each_healing requires a value effect");
  }
  for_each_report_outcome(invocation, [&](const outcome_ref ref) {
    if (ref.kind != outcome_store_kind::healing) return;
    if (ref.index >= invocation.work->healing_trace.size()) {
      throw std::out_of_range("cardgame execution report healing ref is invalid");
    }
    value(report_healing_scope{
      &invocation, &invocation.work->healing_trace[ref.index]});
  });
}

void each_report_shield(
  const execution_report_scope scope,
  const devils_script::script_function<void(report_shield_scope)> value) {
  auto& invocation = checked_invocation(scope);
  if (!value) {
    throw std::invalid_argument("cardgame DS each_shield requires a value effect");
  }
  for_each_report_outcome(invocation, [&](const outcome_ref ref) {
    if (ref.kind != outcome_store_kind::shield) return;
    if (ref.index >= invocation.work->shield_trace.size()) {
      throw std::out_of_range("cardgame execution report shield ref is invalid");
    }
    value(report_shield_scope{
      &invocation, &invocation.work->shield_trace[ref.index]});
  });
}

void each_report_attribute_damage(
  const execution_report_scope scope,
  const devils_script::script_function<void(report_attribute_damage_scope)> value) {
  auto& invocation = checked_invocation(scope);
  if (!value) {
    throw std::invalid_argument(
      "cardgame DS each_attribute_damage requires a value effect");
  }
  for_each_report_outcome(invocation, [&](const outcome_ref ref) {
    if (ref.kind != outcome_store_kind::attribute_damage) return;
    if (ref.index >= invocation.work->attribute_damage_trace.size()) {
      throw std::out_of_range(
        "cardgame execution report attribute damage ref is invalid");
    }
    value(report_attribute_damage_scope{
      &invocation, &invocation.work->attribute_damage_trace[ref.index]});
  });
}

void each_report_status(
  const execution_report_scope scope,
  const devils_script::script_function<void(report_status_scope)> value) {
  auto& invocation = checked_invocation(scope);
  if (!value) {
    throw std::invalid_argument("cardgame DS each_status requires a value effect");
  }
  for_each_report_outcome(invocation, [&](const outcome_ref ref) {
    if (ref.kind != outcome_store_kind::effect) return;
    if (ref.index >= invocation.work->effect_trace.size()) {
      throw std::out_of_range("cardgame execution report status ref is invalid");
    }
    value(report_status_scope{
      &invocation, &invocation.work->effect_trace[ref.index]});
  });
}

uint64_t report_attack_instance(const report_attack_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->root;
}

entity_id report_attack_source(const report_attack_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->source;
}

entity_id report_attack_target(const report_attack_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->target;
}

int64_t report_attack_requested(const report_attack_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->base_damage;
}

element report_attack_element(const report_attack_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->type;
}

bool report_attack_applies_element(const report_attack_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->applies_element;
}

uint64_t report_damage_instance(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->damage.header.id;
}

uint64_t report_damage_root(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->damage.header.root;
}

entity_id report_damage_source(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return static_cast<entity_id>(scope.value->damage.header.source);
}

entity_id report_damage_target(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return static_cast<entity_id>(scope.value->damage.header.target);
}

int64_t report_damage_requested(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.requested;
}

int64_t report_damage_delta(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return static_cast<int64_t>(scope.value->route.before) -
         scope.value->route.committed_after;
}

int64_t report_damage_before(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.before;
}

int64_t report_damage_after(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.committed_after;
}

int64_t report_damage_clamped(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.clamped;
}

element report_damage_element(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->damage.payload.kind;
}

uint64_t report_damage_tags(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->damage.payload.tags;
}

damage_destination report_damage_destination(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->damage.payload.destination;
}

damage_channel report_damage_channel(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->damage.payload.channel;
}

bool report_damage_committed(const report_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->committed;
}

template <typename Scope, typename Instance>
const Instance& checked_stat_instance(const Scope scope, const Instance* value) {
  (void)checked_report_item(scope);
  return *value;
}

uint64_t report_healing_instance(const report_healing_scope scope) {
  return checked_stat_instance(scope, &scope.value->healing).id;
}

entity_id report_healing_source(const report_healing_scope scope) {
  return checked_stat_instance(scope, &scope.value->healing).source;
}

entity_id report_healing_target(const report_healing_scope scope) {
  return checked_stat_instance(scope, &scope.value->healing).target;
}

int64_t report_healing_requested(const report_healing_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.requested;
}

int64_t report_healing_delta(const report_healing_scope scope) {
  (void)checked_report_item(scope);
  return static_cast<int64_t>(scope.value->route.committed_after) -
         scope.value->route.before;
}

int64_t report_healing_before(const report_healing_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.before;
}

int64_t report_healing_after(const report_healing_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.committed_after;
}

int64_t report_healing_clamped(const report_healing_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.clamped;
}

int64_t report_healing_effectiveness(const report_healing_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->effectiveness_basis_points;
}

bool report_healing_committed(const report_healing_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->committed;
}

uint64_t report_shield_instance(const report_shield_scope scope) {
  return checked_stat_instance(scope, &scope.value->shield).id;
}

entity_id report_shield_source(const report_shield_scope scope) {
  return checked_stat_instance(scope, &scope.value->shield).source;
}

entity_id report_shield_target(const report_shield_scope scope) {
  return checked_stat_instance(scope, &scope.value->shield).target;
}

int64_t report_shield_requested(const report_shield_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.requested;
}

int64_t report_shield_delta(const report_shield_scope scope) {
  (void)checked_report_item(scope);
  return static_cast<int64_t>(scope.value->route.committed_after) -
         scope.value->route.before;
}

int64_t report_shield_before(const report_shield_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.before;
}

int64_t report_shield_after(const report_shield_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.committed_after;
}

int64_t report_shield_clamped(const report_shield_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.clamped;
}

bool report_shield_committed(const report_shield_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->committed;
}

uint64_t report_attribute_instance(const report_attribute_damage_scope scope) {
  return checked_stat_instance(scope, &scope.value->damage).id;
}

entity_id report_attribute_source(const report_attribute_damage_scope scope) {
  return checked_stat_instance(scope, &scope.value->damage).source;
}

entity_id report_attribute_target(const report_attribute_damage_scope scope) {
  return checked_stat_instance(scope, &scope.value->damage).target;
}

int64_t report_attribute_requested(const report_attribute_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.requested;
}

int64_t report_attribute_delta(const report_attribute_damage_scope scope) {
  (void)checked_report_item(scope);
  return static_cast<int64_t>(scope.value->route.before) -
         scope.value->route.committed_after;
}

attribute_kind report_attribute_kind(const report_attribute_damage_scope scope) {
  return checked_stat_instance(scope, &scope.value->damage).attribute;
}

int64_t report_attribute_before(const report_attribute_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.before;
}

int64_t report_attribute_after(const report_attribute_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.committed_after;
}

int64_t report_attribute_clamped(const report_attribute_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->route.clamped;
}

int64_t report_attribute_resistance(const report_attribute_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->resistance_basis_points;
}

bool report_attribute_committed(const report_attribute_damage_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->committed;
}

uint64_t report_status_instance(const report_status_scope scope) {
  return checked_stat_instance(scope, &scope.value->request).id;
}

entity_id report_status_source(const report_status_scope scope) {
  return checked_stat_instance(scope, &scope.value->request).source;
}

entity_id report_status_target(const report_status_scope scope) {
  return checked_stat_instance(scope, &scope.value->request).target;
}

effect_kind report_status_kind(const report_status_scope scope) {
  return checked_stat_instance(scope, &scope.value->request).kind;
}

int64_t report_status_requested_stacks(const report_status_scope scope) {
  return checked_stat_instance(scope, &scope.value->request).stacks;
}

int64_t report_status_remaining_pulses(const report_status_scope scope) {
  return checked_stat_instance(scope, &scope.value->request).remaining_pulses;
}

int64_t report_status_previous_stacks(const report_status_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->previous_stacks;
}

int64_t report_status_resulting_stacks(const report_status_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->resulting_stacks;
}

effect_apply_result report_status_result(const report_status_scope scope) {
  (void)checked_report_item(scope);
  return scope.value->result;
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
  if (scope == "execution_report") {
    return {
      sys_.parse<void, execution_report_scope>(name, expression),
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
  static std::array category_values{
    std::tuple<std::string_view, execution_category>{
      "category_attack", execution_category::attack},
    std::tuple<std::string_view, execution_category>{
      "category_damage", execution_category::damage},
    std::tuple<std::string_view, execution_category>{
      "category_healing", execution_category::healing},
    std::tuple<std::string_view, execution_category>{
      "category_shield", execution_category::shield},
    std::tuple<std::string_view, execution_category>{
      "category_attribute_change", execution_category::attribute_change},
    std::tuple<std::string_view, execution_category>{
      "category_status", execution_category::status},
    std::tuple<std::string_view, execution_category>{
      "category_stat_change", execution_category::stat_change},
    std::tuple<std::string_view, execution_category>{
      "category_reaction", execution_category::reaction},
    std::tuple<std::string_view, execution_category>{
      "category_elemental_reaction", execution_category::elemental_reaction},
    std::tuple<std::string_view, execution_category>{
      "category_retaliation", execution_category::retaliation}};
  static std::array status_result_values{
    std::tuple<std::string_view, effect_apply_result>{
      "added", effect_apply_result::added},
    std::tuple<std::string_view, effect_apply_result>{
      "updated", effect_apply_result::updated},
    std::tuple<std::string_view, effect_apply_result>{
      "immune", effect_apply_result::immune},
    std::tuple<std::string_view, effect_apply_result>{
      "invalid_target", effect_apply_result::invalid_target},
    std::tuple<std::string_view, effect_apply_result>{
      "rejected", effect_apply_result::rejected}};

  sys.register_enum<element>(element_values);
  sys.register_enum<attribute_kind>(attribute_values);
  sys.register_enum<effect_kind>(effect_values);
  sys.register_enum<damage_destination>(destination_values);
  sys.register_enum<damage_channel>(channel_values);
  sys.register_enum<execution_category>(category_values);
  sys.register_enum<effect_apply_result>(status_result_values);
  sys.register_function_iter<&each_target>("each_target", {"value"});
  sys.register_function<&emit_attack>("emit_attack");
  sys.register_function<&emit_healing>("emit_healing");
  sys.register_function<&emit_shield>("emit_shield");
  sys.register_function<&emit_attribute_damage>("emit_attribute_damage");
  sys.register_function<&emit_status>("emit_status");
  sys.register_function<&report_execution>("execution");
  sys.register_function<&report_actor>("actor");
  sys.register_function<&report_selected_target>("selected_target");
  sys.register_function<&report_executed>("executed");
  sys.register_function<&report_has_category>("has_category");
  sys.register_function<&report_attack_count>("attack_count");
  sys.register_function<
    &report_outcome_count<outcome_store_kind::damage>>("damage_count");
  sys.register_function<
    &report_outcome_count<outcome_store_kind::healing>>("healing_count");
  sys.register_function<
    &report_outcome_count<outcome_store_kind::shield>>("shield_count");
  sys.register_function<
    &report_outcome_count<outcome_store_kind::attribute_damage>>(
    "attribute_damage_count");
  sys.register_function<
    &report_outcome_count<outcome_store_kind::effect>>("status_count");
  sys.register_function_iter<&each_report_attack>("each_attack", {"value"});
  sys.register_function_iter<&each_report_damage>("each_damage", {"value"});
  sys.register_function_iter<&each_report_healing>("each_healing", {"value"});
  sys.register_function_iter<&each_report_shield>("each_shield", {"value"});
  sys.register_function_iter<&each_report_attribute_damage>(
    "each_attribute_damage", {"value"});
  sys.register_function_iter<&each_report_status>("each_status", {"value"});
  sys.register_function<&report_attack_instance>("instance");
  sys.register_function<&report_attack_source>("source");
  sys.register_function<&report_attack_target>("target");
  sys.register_function<&report_attack_requested>("requested");
  sys.register_function<&report_attack_element>("element");
  sys.register_function<&report_attack_applies_element>("applies_element");
  sys.register_function<&report_damage_instance>("instance");
  sys.register_function<&report_damage_root>("root");
  sys.register_function<&report_damage_source>("source");
  sys.register_function<&report_damage_target>("target");
  sys.register_function<&report_damage_requested>("requested");
  sys.register_function<&report_damage_delta>("delta");
  sys.register_function<&report_damage_before>("before");
  sys.register_function<&report_damage_after>("after");
  sys.register_function<&report_damage_clamped>("clamped");
  sys.register_function<&report_damage_element>("element");
  sys.register_function<&report_damage_tags>("tags");
  sys.register_function<&report_damage_destination>("destination");
  sys.register_function<&report_damage_channel>("channel");
  sys.register_function<&report_damage_committed>("committed");
  sys.register_function<&report_healing_instance>("instance");
  sys.register_function<&report_healing_source>("source");
  sys.register_function<&report_healing_target>("target");
  sys.register_function<&report_healing_requested>("requested");
  sys.register_function<&report_healing_delta>("delta");
  sys.register_function<&report_healing_before>("before");
  sys.register_function<&report_healing_after>("after");
  sys.register_function<&report_healing_clamped>("clamped");
  sys.register_function<&report_healing_effectiveness>("effectiveness");
  sys.register_function<&report_healing_committed>("committed");
  sys.register_function<&report_shield_instance>("instance");
  sys.register_function<&report_shield_source>("source");
  sys.register_function<&report_shield_target>("target");
  sys.register_function<&report_shield_requested>("requested");
  sys.register_function<&report_shield_delta>("delta");
  sys.register_function<&report_shield_before>("before");
  sys.register_function<&report_shield_after>("after");
  sys.register_function<&report_shield_clamped>("clamped");
  sys.register_function<&report_shield_committed>("committed");
  sys.register_function<&report_attribute_instance>("instance");
  sys.register_function<&report_attribute_source>("source");
  sys.register_function<&report_attribute_target>("target");
  sys.register_function<&report_attribute_requested>("requested");
  sys.register_function<&report_attribute_delta>("delta");
  sys.register_function<&report_attribute_kind>("attribute");
  sys.register_function<&report_attribute_before>("before");
  sys.register_function<&report_attribute_after>("after");
  sys.register_function<&report_attribute_clamped>("clamped");
  sys.register_function<&report_attribute_resistance>("resistance");
  sys.register_function<&report_attribute_committed>("committed");
  sys.register_function<&report_status_instance>("instance");
  sys.register_function<&report_status_source>("source");
  sys.register_function<&report_status_target>("target");
  sys.register_function<&report_status_kind>("status_kind");
  sys.register_function<&report_status_requested_stacks>("requested_stacks");
  sys.register_function<&report_status_remaining_pulses>("remaining_pulses");
  sys.register_function<&report_status_previous_stacks>("previous_stacks");
  sys.register_function<&report_status_resulting_stacks>("resulting_stacks");
  sys.register_function<&report_status_result>("status_result");
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

void run_execution_report_script(
  const devils_script::container& program,
  devils_script::context& vm,
  execution_report_view_context& invocation) {
  if (!invocation.valid()) {
    throw std::invalid_argument(
      "cardgame DS execution report invocation is invalid");
  }
  vm.clear();
  vm.userptr = &invocation;
  if (program.max_lists != 0) vm.create_lists(&program);
  vm.set_arg(program.find_arg("root"), execution_report_scope{&invocation});
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
