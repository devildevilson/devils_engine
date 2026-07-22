#include "cardgame/combat.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cardgame {
namespace core {
namespace {

constexpr int32_t enemy_attack_damage = 2;
constexpr int32_t enemy_start_countdown = 2;
constexpr int32_t basis_points = 10000;
constexpr uint16_t reaction_lane = 0;
constexpr uint16_t retaliation_lane = 1;
constexpr uint64_t applies_element_tag = uint64_t{1} << 0;
constexpr resolve::resolution_limits combat_resolution_limits{
  16, 128, 32, 8, 16};

constexpr uint64_t effect_bit(const effect_kind kind) noexcept {
  return uint64_t{1} << static_cast<uint8_t>(kind);
}

presentation_subject damage_subject(const damage_instance& damage) noexcept {
  switch (damage.payload.channel) {
    case damage_channel::primary:
      return damage.header.source == player_entity ? presentation_subject::player_attack
                                                   : presentation_subject::enemy_attack;
    case damage_channel::reaction:
      return presentation_subject::elemental_reaction;
    case damage_channel::retaliation:
      return presentation_subject::returned_damage;
    default:
      return presentation_subject::elemental_reaction;
  }
}

} // namespace

combat::combat(const run_mode mode) noexcept : mode_(mode) {}

bool combat::submit(player_intent intent) {
  if (!awaiting_player() || pending_intent_.has_value()) {
    return false;
  }
  pending_intent_ = intent;
  return true;
}

void combat::update(const uint64_t engine_tick) {
  pipeline_.update(*this, engine_tick);
}

bool combat::notify_presentation(const simul::presentation_task_id task,
                                 const simul::presentation_event_kind kind) noexcept {
  return pipeline_.notify_presentation(task, kind);
}

std::vector<presentation_command> combat::take_presentation_commands() {
  return std::exchange(presentation_outbox_, std::vector<presentation_command>{});
}

const combat_state& combat::state() const noexcept {
  return state_;
}

const combat_cursor& combat::cursor() const noexcept {
  return pipeline_.cursor();
}

const resolution_work& combat::last_resolution() const noexcept {
  return resolution_;
}

bool combat::awaiting_player() const noexcept {
  return pipeline_.cursor().phase == combat_phase::awaiting_action &&
         !pipeline_.waiting() && !pipeline_.faulted();
}

bool combat::waiting_presentation() const noexcept {
  return pipeline_.waiting();
}

bool combat::faulted() const noexcept {
  return pipeline_.faulted() || timeout_reported_ ||
         resolution_.damage_frontier.status == resolve::frontier_status::faulted;
}

combat::snapshot combat::save() const {
  return snapshot{state_, pipeline_.save(), resolution_, pending_intent_, next_presentation_task_,
                  next_instance_, next_root_, next_execution_, next_effect_call_};
}

void combat::load(const snapshot& value) {
  state_ = value.state;
  resolution_ = value.resolution;
  pending_intent_ = value.pending_intent;
  next_presentation_task_ = value.next_presentation_task;
  next_instance_ = value.next_instance;
  next_root_ = value.next_root;
  next_execution_ = value.next_execution;
  next_effect_call_ = value.next_effect_call;
  presentation_outbox_.clear();
  active_beat_tasks_.clear();
  timeout_reported_ = false;
  pipeline_.load(value.pipeline);
}

bool combat::advances_countdown(const card_kind card) noexcept {
  return card != card_kind::quick_strike;
}

instance_id combat::allocate_instance() {
  if (next_instance_ == resolve::invalid_instance ||
      next_instance_ == std::numeric_limits<instance_id>::max()) {
    throw std::overflow_error("cardgame instance id space exhausted");
  }
  return next_instance_++;
}

instance_id combat::allocate_root() {
  if (next_root_ == resolve::invalid_instance ||
      next_root_ == std::numeric_limits<instance_id>::max()) {
    throw std::overflow_error("cardgame root token space exhausted");
  }
  return next_root_++;
}

instance_id combat::allocate_effect_call() {
  if (next_effect_call_ == resolve::invalid_instance ||
      next_effect_call_ == std::numeric_limits<instance_id>::max()) {
    throw std::overflow_error("cardgame authored effect call id space exhausted");
  }
  return next_effect_call_++;
}

instance_id combat::allocate_execution() {
  if (next_execution_ == resolve::invalid_instance ||
      next_execution_ == std::numeric_limits<instance_id>::max()) {
    throw std::overflow_error("cardgame execution id space exhausted");
  }
  return next_execution_++;
}

combatant_state* combat::find_combatant(const entity_id id) noexcept {
  if (id == state_.player.id) return &state_.player;
  if (id == state_.enemy.id) return &state_.enemy;
  return nullptr;
}

const combatant_state* combat::find_combatant(const entity_id id) const noexcept {
  if (id == state_.player.id) return &state_.player;
  if (id == state_.enemy.id) return &state_.enemy;
  return nullptr;
}

void combat::begin_card_resolution(combat_cursor& cursor) {
  resolution_ = {};
  cursor.resolution = {};
  cursor.resolution.stage = resolution_stage::select_beat;

  resolution_.report = execution_report{
    allocate_execution(), player_entity, enemy_entity, true, {}};

  const auto attack = [this](const element type, const int32_t damage,
                             const uint16_t hit_count, const bool applies_element) {
    const size_t index = resolution_.attack_effects.size();
    resolution_.attack_effects.push_back(
      attack_effect{player_entity, type, damage, hit_count, applies_element});
    return authored_effect{
      effect_ref{effect_store_kind::attack, index},
      targeter{targeter_kind::target, independent_target_binding}};
  };
  const auto burning = [this]() {
    const size_t index = resolution_.status_effects.size();
    resolution_.status_effects.push_back(
      status_effect{player_entity, effect_kind::burning, 1, 2});
    return authored_effect{
      effect_ref{effect_store_kind::effect, index},
      targeter{targeter_kind::target, independent_target_binding}};
  };
  const auto healing = [this](const int32_t amount) {
    const size_t index = resolution_.healing_effects.size();
    resolution_.healing_effects.push_back(healing_effect{player_entity, amount});
    return authored_effect{
      effect_ref{effect_store_kind::healing, index},
      targeter{targeter_kind::target, independent_target_binding},
      authored_effect::target_domain::self};
  };
  const auto attribute_damage = [this](const attribute_kind attribute,
                                       const int32_t amount) {
    const size_t index = resolution_.attribute_damage_effects.size();
    resolution_.attribute_damage_effects.push_back(
      attribute_damage_effect{player_entity, attribute, amount});
    return authored_effect{
      effect_ref{effect_store_kind::attribute_damage, index},
      targeter{targeter_kind::target, independent_target_binding}};
  };

  switch (cursor.active_card) {
    case card_kind::strike:
      resolution_.program.beats.push_back(
        effect_beat{{attack(element::none, 3, 1, false)}});
      break;
    case card_kind::quick_strike:
      resolution_.program.beats.push_back(
        effect_beat{{attack(element::none, 1, 1, false)}});
      break;
    case card_kind::fire_strike:
      // Both authored effects share one presentation beat but keep independent target snapshots.
      resolution_.program.beats.push_back(
        effect_beat{{attack(element::fire, 4, 1, true), burning()}});
      break;
    case card_kind::double_strike:
      // One script call/animation deliberately emits two typed attack instances.
      resolution_.program.beats.push_back(
        effect_beat{{attack(element::none, 2, 2, false)}});
      break;
    case card_kind::combo_strike:
      resolution_.program.beats.push_back(
        effect_beat{{attack(element::none, 2, 1, false)}});
      resolution_.program.beats.push_back(
        effect_beat{{attack(element::none, 2, 1, false)}});
      break;
    case card_kind::inverse_strike:
      resolution_.program.beats.push_back(
        effect_beat{{attack(element::none, -5, 1, false)}});
      break;
    case card_kind::mend:
      resolution_.program.beats.push_back(effect_beat{{healing(6)}});
      break;
    case card_kind::cursed_mend:
      resolution_.program.beats.push_back(effect_beat{{healing(-7)}});
      break;
    case card_kind::cripple:
      resolution_.program.beats.push_back(
        effect_beat{{attribute_damage(attribute_kind::agility, 4)}});
      break;
  }
}

void combat::begin_enemy_resolution(combat_cursor& cursor) {
  resolution_ = {};
  cursor.resolution = {};
  cursor.resolution.stage = resolution_stage::select_beat;
  resolution_.report = execution_report{
    allocate_execution(), enemy_entity, player_entity, true, {}};
  resolution_.attack_effects.push_back(
    attack_effect{enemy_entity, element::none, enemy_attack_damage, 1, false});
  resolution_.program.beats.push_back(effect_beat{{authored_effect{
    effect_ref{effect_store_kind::attack, 0},
    targeter{targeter_kind::target, independent_target_binding}}}});
}

void combat::begin_attack_frontier(const attack_instance& attack) {
  damage_instance root;
  root.header.root = attack.root;
  root.header.source = attack.source;
  root.header.target = attack.target;
  root.header.cause = resolve::cause_kind::primary;
  root.payload = damage_payload{
    attack.type,
    attack.base_damage,
    damage_channel::primary,
    attack.applies_element ? applies_element_tag : 0};

  if (!resolve::begin(
        resolution_.damage_frontier,
        std::span<const damage_instance>{&root, 1},
        combat_resolution_limits,
        next_instance_)) {
    return;
  }
  next_instance_ = resolution_.damage_frontier.next_instance;
  resolution_.next_frontier.clear();
  resolution_.retaliation_requests.clear();
  resolution_.responses.clear();
}

std::vector<entity_id> combat::eligible_targets(
  const entity_id source,
  const authored_effect::target_domain domain) const {
  std::vector<entity_id> result;
  const combatant_state* candidate = nullptr;
  if (domain == authored_effect::target_domain::self) {
    candidate = find_combatant(source);
  } else {
    candidate = source == player_entity ? &state_.enemy : &state_.player;
  }
  if (candidate == nullptr) return result;
  if (candidate->hp > 0) result.push_back(candidate->id);
  return result;
}

void combat::materialize_beat(resolution_cursor& cursor) {
  const effect_beat& beat = resolution_.program.beats[cursor.beat_index];
  std::vector<target_query> queries;
  queries.reserve(beat.effects.size());

  for (const authored_effect& effect : beat.effects) {
    entity_id source = invalid_entity;
    switch (effect.body.kind) {
      case effect_store_kind::attack:
        if (effect.body.index >= resolution_.attack_effects.size()) {
          throw std::out_of_range("cardgame authored attack recipe index is invalid");
        }
        source = resolution_.attack_effects[effect.body.index].source;
        break;
      case effect_store_kind::healing:
        if (effect.body.index >= resolution_.healing_effects.size()) {
          throw std::out_of_range("cardgame authored healing recipe index is invalid");
        }
        source = resolution_.healing_effects[effect.body.index].source;
        break;
      case effect_store_kind::attribute_damage:
        if (effect.body.index >= resolution_.attribute_damage_effects.size()) {
          throw std::out_of_range(
            "cardgame authored attribute damage recipe index is invalid");
        }
        source = resolution_.attribute_damage_effects[effect.body.index].source;
        break;
      case effect_store_kind::effect:
        if (effect.body.index >= resolution_.status_effects.size()) {
          throw std::out_of_range("cardgame authored status recipe index is invalid");
        }
        source = resolution_.status_effects[effect.body.index].source;
        break;
    }
    const entity_id fixed_target =
      effect.domain == authored_effect::target_domain::self
        ? source
        : resolution_.report.selected_target;
    queries.push_back(target_query{
      effect.targets, fixed_target, eligible_targets(source, effect.domain)});
  }

  const uint64_t entropy =
    static_cast<uint64_t>(resolution_.report.execution) ^
    (static_cast<uint64_t>(cursor.beat_index) << 32u);
  const auto snapshots = materialize_target_sets(queries, entropy);
  cursor.beat_report_begin = resolution_.report.effects.size();
  cursor.authored_effect_index = 0;
  for (size_t i = 0; i < beat.effects.size(); ++i) {
    resolution_.report.effects.push_back(authored_effect_report{
      allocate_effect_call(), cursor.beat_index, i, beat.effects[i].body, snapshots[i]});
  }
}

void combat::invoke_authored_effect(authored_effect_report& call) {
  call.invoked = true;
  call.plan_begin = resolution_.plan.size();
  call.damage_outcome_begin = resolution_.damage_trace.size();
  call.effect_outcome_begin = resolution_.effect_trace.size();
  call.healing_outcome_begin = resolution_.healing_trace.size();
  call.attribute_outcome_begin = resolution_.attribute_damage_trace.size();
  call.outcome_begin = resolution_.outcomes.size();

  switch (call.body.kind) {
    case effect_store_kind::attack: {
      const attack_effect& effect = resolution_.attack_effects.at(call.body.index);
      if (effect.hit_count == 0) {
        throw std::invalid_argument("cardgame attack effect must emit at least one hit");
      }
      for (const entity_id target : call.target_set.targets) {
        for (uint16_t hit = 0; hit < effect.hit_count; ++hit) {
          const size_t index = resolution_.attacks.size();
          resolution_.attacks.push_back(attack_instance{
            allocate_root(), resolution_.report.execution, effect.source, target,
            effect.type, effect.base_damage, effect.applies_element});
          resolution_.plan.push_back(
            effect_instance_ref{effect_store_kind::attack, index});
        }
      }
      if (resolution_.plan.size() != call.plan_begin) {
        add_category(resolution_.report.categories, execution_category::attack);
      }
      break;
    }
    case effect_store_kind::healing: {
      const healing_effect& effect = resolution_.healing_effects.at(call.body.index);
      for (const entity_id target : call.target_set.targets) {
        const size_t index = resolution_.healings.size();
        resolution_.healings.push_back(healing_instance{
          0, resolution_.report.execution, effect.source, target, effect.amount});
        resolution_.plan.push_back(
          effect_instance_ref{effect_store_kind::healing, index});
      }
      if (resolution_.plan.size() != call.plan_begin) {
        add_category(resolution_.report.categories, execution_category::healing);
      }
      break;
    }
    case effect_store_kind::attribute_damage: {
      const attribute_damage_effect& effect =
        resolution_.attribute_damage_effects.at(call.body.index);
      for (const entity_id target : call.target_set.targets) {
        const size_t index = resolution_.attribute_damages.size();
        resolution_.attribute_damages.push_back(attribute_damage_instance{
          0,
          resolution_.report.execution,
          effect.source,
          target,
          effect.attribute,
          effect.amount});
        resolution_.plan.push_back(
          effect_instance_ref{effect_store_kind::attribute_damage, index});
      }
      if (resolution_.plan.size() != call.plan_begin) {
        add_category(resolution_.report.categories, execution_category::attribute_change);
      }
      break;
    }
    case effect_store_kind::effect: {
      const status_effect& effect = resolution_.status_effects.at(call.body.index);
      for (const entity_id target : call.target_set.targets) {
        const size_t index = resolution_.effects.size();
        resolution_.effects.push_back(effect_request{
          0, resolution_.report.execution, effect.source, target,
          effect.kind, effect.stacks, effect.remaining_pulses});
        resolution_.plan.push_back(
          effect_instance_ref{effect_store_kind::effect, index});
      }
      if (resolution_.plan.size() != call.plan_begin) {
        add_category(resolution_.report.categories, execution_category::status);
      }
      break;
    }
  }
  call.plan_count = resolution_.plan.size() - call.plan_begin;
}

void combat::finalize_authored_effect(authored_effect_report& call) {
  call.damage_outcome_count = resolution_.damage_trace.size() - call.damage_outcome_begin;
  call.effect_outcome_count = resolution_.effect_trace.size() - call.effect_outcome_begin;
  call.healing_outcome_count = resolution_.healing_trace.size() - call.healing_outcome_begin;
  call.attribute_outcome_count =
    resolution_.attribute_damage_trace.size() - call.attribute_outcome_begin;
  call.outcome_count = resolution_.outcomes.size() - call.outcome_begin;
}

presentation_subject combat::subject_for(const authored_effect_report& call) const {
  switch (call.body.kind) {
    case effect_store_kind::attack: {
      const attack_effect& effect = resolution_.attack_effects.at(call.body.index);
      return effect.source == player_entity ? presentation_subject::player_attack
                                            : presentation_subject::enemy_attack;
    }
    case effect_store_kind::healing:
      return presentation_subject::healing;
    case effect_store_kind::attribute_damage:
      return presentation_subject::attribute_damage;
    case effect_store_kind::effect:
      return presentation_subject::effect;
  }
  return presentation_subject::effect;
}

std::vector<presentation_command::result_value> combat::presentation_results(
  const authored_effect_report& call) const {
  std::vector<presentation_command::result_value> result;
  result.reserve(call.outcome_count);
  for (size_t i = 0; i < call.outcome_count; ++i) {
    const outcome_ref ref = resolution_.outcomes[call.outcome_begin + i];
    switch (ref.kind) {
      case outcome_store_kind::damage: {
        const damage_outcome& outcome = resolution_.damage_trace.at(ref.index);
        result.push_back(presentation_command::result_value{
          damage_subject(outcome.damage), outcome.damage.header.id,
          static_cast<entity_id>(outcome.damage.header.target),
          outcome.route.hp_before - outcome.route.committed_hp_after, 0});
        break;
      }
      case outcome_store_kind::healing: {
        const healing_outcome& outcome = resolution_.healing_trace.at(ref.index);
        result.push_back(presentation_command::result_value{
          presentation_subject::healing,
          outcome.healing.id,
          outcome.healing.target,
          outcome.route.committed_after - outcome.route.before,
          0});
        break;
      }
      case outcome_store_kind::attribute_damage: {
        const attribute_damage_outcome& outcome =
          resolution_.attribute_damage_trace.at(ref.index);
        result.push_back(presentation_command::result_value{
          presentation_subject::attribute_damage,
          outcome.damage.id,
          outcome.damage.target,
          outcome.route.before - outcome.route.committed_after,
          static_cast<uint32_t>(outcome.damage.attribute)});
        break;
      }
      case outcome_store_kind::effect: {
        const effect_outcome& outcome = resolution_.effect_trace.at(ref.index);
        result.push_back(presentation_command::result_value{
          presentation_subject::effect,
          outcome.request.id,
          outcome.request.target,
          outcome.resulting_stacks,
          static_cast<uint32_t>(outcome.result)});
        break;
      }
    }
  }
  return result;
}

std::vector<damage_modifier> combat::collect_resistance_modifiers(
  const combatant_state& target,
  const damage_instance& damage) const {
  std::vector<damage_modifier> modifiers;
  const size_t index = static_cast<size_t>(damage.payload.kind);
  if (index < target.resistance_basis_points.size()) {
    const int32_t resistance = std::clamp(
      target.resistance_basis_points[index], -basis_points, basis_points);
    if (resistance != 0) {
      // This native row is the first stand-in for the project ds resistance script. The script
      // will record the same operations rather than mutate HP or damage directly.
      modifiers.push_back(damage_modifier{
        0, damage_modifier_kind::multiply_basis_points, basis_points - resistance});
    }
  }

  std::sort(modifiers.begin(), modifiers.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.source != rhs.source) return lhs.source < rhs.source;
    return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);
  });
  return modifiers;
}

damage_outcome combat::resolve_damage(const damage_instance& damage) {
  damage_outcome result;
  result.damage = damage;
  result.route.requested = damage.payload.amount;
  combatant_state* target = find_combatant(static_cast<entity_id>(damage.header.target));
  // Liveness gates the next beat, not work already materialized for the current beat. A later hit
  // in the same authored batch therefore still produces an authoritative zero-floor outcome.
  if (target == nullptr) {
    return result;
  }

  result.target_valid = true;
  result.modifiers = collect_resistance_modifiers(*target, damage);
  int64_t current = damage.payload.amount;
  for (const auto& modifier : result.modifiers) {
    switch (modifier.kind) {
      case damage_modifier_kind::add:
        current += modifier.value;
        break;
      case damage_modifier_kind::multiply_basis_points:
        current = current * modifier.value / basis_points;
        break;
      case damage_modifier_kind::clamp_max:
        current = std::min<int64_t>(current, modifier.value);
        break;
      case damage_modifier_kind::block:
        current = 0;
        break;
    }
    current = std::clamp<int64_t>(
      current,
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::max());
  }

  result.route.modified = static_cast<int32_t>(current);
  result.route.shield_absorbed = result.route.modified > 0
                                   ? std::min(target->shield, result.route.modified)
                                   : 0;
  result.route.hp_before = target->hp;
  const int64_t proposed = static_cast<int64_t>(target->hp) -
                           (result.route.modified - result.route.shield_absorbed);
  result.route.proposed_hp_after = static_cast<int32_t>(std::clamp<int64_t>(
    proposed,
    std::numeric_limits<int32_t>::min(),
    std::numeric_limits<int32_t>::max()));
  // Zero-flooring is cardgame's current death policy. Commit guards may adjust
  // committed_hp_after between route construction and this authoritative write.
  const int32_t upper = std::max(target->max_hp, target->hp);
  result.route.committed_hp_after =
    target->hp <= 0 && result.route.proposed_hp_after > target->hp
      ? target->hp
      : std::clamp(result.route.proposed_hp_after, 0, upper);
  target->shield -= result.route.shield_absorbed;
  target->hp = result.route.committed_hp_after;
  result.committed = true;
  return result;
}

healing_outcome combat::resolve_healing(const healing_instance& healing) {
  healing_outcome result;
  result.healing = healing;
  result.route.requested = healing.amount;
  combatant_state* target = find_combatant(healing.target);
  if (target == nullptr) return result;

  result.target_valid = true;
  result.route.before = target->hp;
  result.effectiveness_basis_points = std::clamp(
    target->healing_effectiveness_basis_points, 0, basis_points * 2);
  const int64_t modified =
    static_cast<int64_t>(healing.amount) * result.effectiveness_basis_points /
    basis_points;
  result.route.modified = static_cast<int32_t>(std::clamp<int64_t>(
    modified,
    std::numeric_limits<int32_t>::min(),
    std::numeric_limits<int32_t>::max()));

  // Healing never resurrects a target after the death predicate latched it. Already materialized
  // negative healing still produces a zero-delta outcome for the current beat.
  if (target->hp <= 0) {
    result.route.proposed_after = target->hp;
    result.route.committed_after = target->hp;
    return result;
  }

  const int64_t proposed =
    static_cast<int64_t>(target->hp) + result.route.modified;
  result.route.proposed_after = static_cast<int32_t>(std::clamp<int64_t>(
    proposed,
    std::numeric_limits<int32_t>::min(),
    std::numeric_limits<int32_t>::max()));
  const int32_t upper = std::max(target->max_hp, target->hp);
  result.route.committed_after = std::clamp(result.route.proposed_after, 0, upper);
  result.route.clamped = result.route.proposed_after - result.route.committed_after;
  target->hp = result.route.committed_after;
  result.committed = true;
  return result;
}

attribute_damage_outcome combat::resolve_attribute_damage(
  const attribute_damage_instance& damage) {
  attribute_damage_outcome result;
  result.damage = damage;
  result.route.requested = damage.amount;
  combatant_state* target = find_combatant(damage.target);
  if (target == nullptr) return result;

  int32_t* attribute = nullptr;
  switch (damage.attribute) {
    case attribute_kind::agility:
      attribute = &target->agility;
      break;
    case attribute_kind::count:
      throw std::invalid_argument("cardgame attribute damage kind is invalid");
  }

  result.target_valid = true;
  result.route.before = *attribute;
  const size_t resistance_index = static_cast<size_t>(damage.attribute);
  result.resistance_basis_points = std::clamp(
    target->attribute_resistance_basis_points.at(resistance_index),
    -basis_points,
    basis_points);
  const int64_t modified =
    static_cast<int64_t>(damage.amount) *
    (basis_points - result.resistance_basis_points) / basis_points;
  result.route.modified = static_cast<int32_t>(std::clamp<int64_t>(
    modified,
    std::numeric_limits<int32_t>::min(),
    std::numeric_limits<int32_t>::max()));

  if (target->hp <= 0) {
    result.route.proposed_after = *attribute;
    result.route.committed_after = *attribute;
    return result;
  }

  const int64_t proposed =
    static_cast<int64_t>(*attribute) - result.route.modified;
  result.route.proposed_after = static_cast<int32_t>(std::clamp<int64_t>(
    proposed,
    std::numeric_limits<int32_t>::min(),
    std::numeric_limits<int32_t>::max()));
  result.route.committed_after = std::max(result.route.proposed_after, 0);
  result.route.clamped = result.route.proposed_after - result.route.committed_after;
  *attribute = result.route.committed_after;
  result.committed = true;
  return result;
}

void combat::resolve_damage_work(const damage_instance& damage) {
  damage_outcome outcome = resolve_damage(damage);
  resolution_.damage_trace.push_back(outcome);
  resolution_.outcomes.push_back(outcome_ref{
    outcome_store_kind::damage, resolution_.damage_trace.size() - 1});
  add_category(resolution_.report.categories, execution_category::damage);
  add_category(resolution_.report.categories, execution_category::stat_change);
  if (damage.payload.channel == damage_channel::reaction) {
    add_category(resolution_.report.categories, execution_category::reaction);
    add_category(resolution_.report.categories, execution_category::elemental_reaction);
  } else if (damage.payload.channel == damage_channel::retaliation) {
    add_category(resolution_.report.categories, execution_category::retaliation);
  }
  record_death_check(outcome_store_kind::damage, resolution_.damage_trace.size() - 1,
                     static_cast<entity_id>(damage.header.target));

  combatant_state* target = find_combatant(static_cast<entity_id>(damage.header.target));
  if (target == nullptr || !outcome.target_valid) {
    return;
  }

  // An elemental collision is detected once per attack instance. Its gameplay is a later resolver
  // stage even if presentation eventually chooses to overlap both animations.
  if ((damage.payload.tags & applies_element_tag) != 0 &&
      damage.payload.kind != element::none) {
    if (target->elemental_mark == element::none) {
      target->elemental_mark = damage.payload.kind;
    } else if (target->elemental_mark != damage.payload.kind) {
      target->elemental_mark = element::none;
      damage_instance reaction;
      reaction.header = resolve::make_child_header(
        damage.header,
        resolve::cause_kind::reaction,
        reaction_lane,
        0,
        damage.header.source,
        damage.header.target);
      reaction.payload = damage_payload{
        damage.payload.kind, 2, damage_channel::reaction, 0};
      resolution_.next_frontier.push_back(reaction);
    }
  }

  // `thorns` listens to committed primary damage. Requests are materialized now, then passed
  // through resolve::retaliation_journal after the reaction subtree so duplicate discovery and
  // retaliation-lineage suppression are engine invariants rather than cardgame conventions.
  const int32_t hp_loss = outcome.route.hp_before - outcome.route.committed_hp_after;
  if (damage.payload.channel == damage_channel::primary && hp_loss > 0) {
    for (size_t i = 0; i < target->effects.size(); ++i) {
      const auto& effect = target->effects[i];
      if (effect.kind != effect_kind::thorns || effect.stacks <= 0) continue;
      if (i > std::numeric_limits<uint16_t>::max()) {
        throw std::length_error("cardgame retaliation local ordinal exceeded");
      }
      resolution_.retaliation_requests.push_back(retaliation_request{
        damage.header,
        effect.id,
        static_cast<entity_id>(damage.header.target),
        static_cast<entity_id>(damage.header.source),
        effect.stacks,
        static_cast<uint16_t>(i)});
    }
  }
}

void combat::prepare_retaliations() {
  resolve::retaliation_journal<damage_payload> journal;
  journal.begin_record(combat_resolution_limits.max_jobs_per_frontier);
  for (const auto& request : resolution_.retaliation_requests) {
    const auto result = journal.emit(
      request.trigger,
      request.rule,
      retaliation_lane,
      request.local_ordinal,
      request.source,
      request.target,
      damage_payload{element::none, request.amount, damage_channel::retaliation, 0});
    if (result == resolve::retaliation_emit_result::overflow) {
      throw std::length_error("cardgame retaliation journal capacity exceeded");
    }
    if (result == resolve::retaliation_emit_result::invalid_trigger) {
      throw std::logic_error("cardgame retaliation trigger was not sealed");
    }
  }

  journal.seal_ordered(combat_resolution_limits);
  resolution_.responses.clear();
  resolution_.responses.reserve(journal.records().size());
  for (const auto& record : journal.records()) {
    resolution_.responses.push_back(damage_instance{record.header, record.payload});
  }
}

bool combat::advance_to_retaliations() {
  prepare_retaliations();
  if (!resolve::advance(
        resolution_.damage_frontier,
        std::span<const damage_instance>{resolution_.responses},
        combat_resolution_limits)) {
    return false;
  }
  next_instance_ = resolution_.damage_frontier.next_instance;
  if (resolution_.damage_frontier.active()) {
    resolution_.responses.assign(
      resolution_.damage_frontier.current.begin(),
      resolution_.damage_frontier.current.end());
  } else {
    resolution_.responses.clear();
  }
  return true;
}

effect_apply_result combat::can_apply_effect(
  const combatant_state* target,
  const effect_request& request) const noexcept {
  if (target == nullptr || target->hp <= 0) return effect_apply_result::invalid_target;
  if (request.stacks <= 0) return effect_apply_result::rejected;
  if ((target->effect_immunity_mask & effect_bit(request.kind)) != 0) {
    return effect_apply_result::immune;
  }
  return effect_apply_result::added;
}

effect_outcome combat::resolve_effect(const effect_request& request) {
  effect_outcome outcome;
  outcome.request = request;
  combatant_state* target = find_combatant(request.target);
  outcome.result = can_apply_effect(target, request);
  if (outcome.result != effect_apply_result::added) {
    return outcome;
  }

  const auto it = std::find_if(target->effects.begin(), target->effects.end(), [&](const auto& value) {
    return value.kind == request.kind;
  });
  if (it == target->effects.end()) {
    target->effects.push_back(effect_state{
      request.id, request.kind, request.source, request.stacks, request.remaining_pulses});
    outcome.resulting_stacks = request.stacks;
    return outcome;
  }

  outcome.result = effect_apply_result::updated;
  outcome.previous_stacks = it->stacks;
  it->stacks += request.stacks;
  it->remaining_pulses = std::max(it->remaining_pulses, request.remaining_pulses);
  outcome.resulting_stacks = it->stacks;
  return outcome;
}

void combat::record_death_check(const outcome_store_kind kind,
                                const size_t outcome_index,
                                const entity_id target) {
  const combatant_state* combatant = find_combatant(target);
  resolution_.death_trace.push_back(
    death_check{kind, outcome_index, target, combatant == nullptr || combatant->hp <= 0});
}

simul::step_control combat::run_resolution_step(combat_cursor& combat_cursor,
                                                pipeline_type& pipe) {
  resolution_cursor& cursor = combat_cursor.resolution;
  switch (cursor.stage) {
    case resolution_stage::idle:
      cursor.stage = resolution_stage::select_beat;
      return simul::step_control::advance;

    case resolution_stage::select_beat: {
      if (cursor.beat_index >= resolution_.program.beats.size()) {
        cursor.stage = resolution_stage::done;
        return simul::step_control::advance;
      }
      const combatant_state* actor = find_combatant(resolution_.report.actor);
      if (actor == nullptr || actor->hp <= 0) {
        cursor.stage = resolution_stage::done;
        return simul::step_control::advance;
      }
      materialize_beat(cursor);
      cursor.stage = resolution_stage::beat_cue;
      return simul::step_control::advance;
    }

    case resolution_stage::beat_cue: {
      const effect_beat& beat = resolution_.program.beats[cursor.beat_index];
      active_beat_tasks_.assign(beat.effects.size(), 0);
      bool waiting = false;
      if (mode_ == run_mode::animated) {
        for (size_t i = 0; i < beat.effects.size(); ++i) {
          const authored_effect_report& call =
            resolution_.report.effects[cursor.beat_report_begin + i];
          if (call.target_set.targets.empty()) continue;

          const auto task = next_presentation_task_++;
          active_beat_tasks_[i] = task;
          // Arm the complete beat before publishing any cue: presentation may answer immediately.
          pipe.expect_presentation(task, simul::presentation_event_kind::gameplay);
          presentation_command command;
          command.kind = presentation_command_kind::start;
          command.task = task;
          command.subject = subject_for(call);
          command.instance = call.id;
          command.targets = call.target_set.targets;
          command.target = command.targets.front();
          presentation_outbox_.push_back(std::move(command));
          waiting = true;
        }
      }
      cursor.stage = resolution_stage::select_effect;
      return waiting ? simul::step_control::wait : simul::step_control::advance;
    }

    case resolution_stage::select_effect: {
      const effect_beat& beat = resolution_.program.beats[cursor.beat_index];
      if (cursor.authored_effect_index >= beat.effects.size()) {
        cursor.stage = resolution_stage::beat_results;
        return simul::step_control::advance;
      }

      authored_effect_report& call = resolution_.report.effects[cursor.beat_report_begin + cursor.authored_effect_index];
      if (call.target_set.targets.empty()) {
        ++cursor.authored_effect_index;
        return simul::step_control::advance;
      }
      invoke_authored_effect(call);
      cursor.item_index = call.plan_begin;
      cursor.item_end = call.plan_begin + call.plan_count;
      cursor.stage = resolution_stage::select_item;
      return simul::step_control::advance;
    }

    case resolution_stage::select_item:
      if (cursor.item_index >= cursor.item_end) {
        authored_effect_report& call = resolution_.report.effects[cursor.beat_report_begin + cursor.authored_effect_index];
        finalize_authored_effect(call);
        ++cursor.authored_effect_index;
        cursor.stage = resolution_stage::select_effect;
        return simul::step_control::advance;
      }
      switch (resolution_.plan[cursor.item_index].kind) {
        case effect_store_kind::attack:
          cursor.attack_index = resolution_.plan[cursor.item_index].index;
          begin_attack_frontier(resolution_.attacks[cursor.attack_index]);
          if (!resolution_.damage_frontier.active()) {
            return simul::step_control::halt;
          }
          cursor.stage = resolution_stage::attack_commit;
          break;
        case effect_store_kind::healing: {
          cursor.healing_index = resolution_.plan[cursor.item_index].index;
          auto& healing = resolution_.healings.at(cursor.healing_index);
          if (healing.id == resolve::invalid_instance) healing.id = allocate_instance();
          cursor.stage = resolution_stage::healing_commit;
          break;
        }
        case effect_store_kind::attribute_damage: {
          cursor.attribute_damage_index = resolution_.plan[cursor.item_index].index;
          auto& damage = resolution_.attribute_damages.at(cursor.attribute_damage_index);
          if (damage.id == resolve::invalid_instance) damage.id = allocate_instance();
          cursor.stage = resolution_stage::attribute_damage_commit;
          break;
        }
        case effect_store_kind::effect: {
          cursor.effect_index = resolution_.plan[cursor.item_index].index;
          auto& request = resolution_.effects.at(cursor.effect_index);
          if (request.id == resolve::invalid_instance) request.id = allocate_instance();
          cursor.stage = resolution_stage::effect_commit;
          break;
        }
      }
      return simul::step_control::advance;

    case resolution_stage::attack_commit: {
      const auto damage = resolution_.damage_frontier.current.front();
      resolve_damage_work(damage);
      cursor.stage = resolution_stage::attack_after;
      return simul::step_control::advance;
    }

    case resolution_stage::attack_after: {
      cursor.reaction_index = 0;
      if (!resolution_.next_frontier.empty()) {
        if (!resolve::advance(
              resolution_.damage_frontier,
              std::span<const damage_instance>{resolution_.next_frontier},
              combat_resolution_limits)) {
          return simul::step_control::halt;
        }
        next_instance_ = resolution_.damage_frontier.next_instance;
        resolution_.next_frontier.clear();
        cursor.stage = resolution_stage::select_reaction;
      } else {
        if (!advance_to_retaliations()) return simul::step_control::halt;
        cursor.response_index = 0;
        cursor.stage = resolution_.damage_frontier.active()
                         ? resolution_stage::select_response
                         : resolution_stage::post_attack;
      }
      return simul::step_control::advance;
    }

    case resolution_stage::select_reaction:
      if (cursor.reaction_index < resolution_.damage_frontier.current.size()) {
        cursor.stage = resolution_stage::reaction_commit;
        return simul::step_control::advance;
      }
      cursor.reaction_index = 0;
      if (!resolution_.next_frontier.empty()) {
        if (!resolve::advance(
              resolution_.damage_frontier,
              std::span<const damage_instance>{resolution_.next_frontier},
              combat_resolution_limits)) {
          return simul::step_control::halt;
        }
        next_instance_ = resolution_.damage_frontier.next_instance;
        resolution_.next_frontier.clear();
        cursor.stage = resolution_stage::select_reaction;
      } else {
        if (!advance_to_retaliations()) return simul::step_control::halt;
        cursor.response_index = 0;
        cursor.stage = resolution_.damage_frontier.active()
                         ? resolution_stage::select_response
                         : resolution_stage::post_attack;
      }
      return simul::step_control::advance;

    case resolution_stage::reaction_commit: {
      const auto reaction = resolution_.damage_frontier.current[cursor.reaction_index];
      resolve_damage_work(reaction);
      cursor.stage = resolution_stage::reaction_after;
      return simul::step_control::advance;
    }

    case resolution_stage::reaction_after:
      ++cursor.reaction_index;
      cursor.stage = resolution_stage::select_reaction;
      return simul::step_control::advance;

    case resolution_stage::select_response:
      if (cursor.response_index < resolution_.responses.size()) {
        cursor.stage = resolution_stage::response_commit;
        return simul::step_control::advance;
      }
      if (resolution_.damage_frontier.active()) {
        if (!resolve::advance(
              resolution_.damage_frontier,
              std::span<const damage_instance>{},
              combat_resolution_limits)) {
          return simul::step_control::halt;
        }
        next_instance_ = resolution_.damage_frontier.next_instance;
      }
      cursor.stage = resolution_stage::post_attack;
      return simul::step_control::advance;

    case resolution_stage::response_commit: {
      const auto response = resolution_.responses[cursor.response_index];
      resolve_damage_work(response);
      cursor.stage = resolution_stage::response_after;
      return simul::step_control::advance;
    }

    case resolution_stage::response_after:
      ++cursor.response_index;
      cursor.stage = resolution_stage::select_response;
      return simul::step_control::advance;

    case resolution_stage::post_attack:
      // HP reaches zero immediately, while destructive entity cleanup/death effects will be a
      // later explicit project stage. Reactions and responses collected by this hit already ran.
      ++cursor.item_index;
      cursor.stage = resolution_stage::select_item;
      return simul::step_control::advance;

    case resolution_stage::healing_commit: {
      const auto healing = resolution_.healings.at(cursor.healing_index);
      resolution_.healing_trace.push_back(resolve_healing(healing));
      const size_t outcome_index = resolution_.healing_trace.size() - 1;
      resolution_.outcomes.push_back(
        outcome_ref{outcome_store_kind::healing, outcome_index});
      add_category(resolution_.report.categories, execution_category::stat_change);
      record_death_check(outcome_store_kind::healing, outcome_index, healing.target);
      cursor.stage = resolution_stage::healing_after;
      return simul::step_control::advance;
    }

    case resolution_stage::healing_after:
      ++cursor.item_index;
      cursor.stage = resolution_stage::select_item;
      return simul::step_control::advance;

    case resolution_stage::attribute_damage_commit: {
      const auto damage = resolution_.attribute_damages.at(cursor.attribute_damage_index);
      resolution_.attribute_damage_trace.push_back(resolve_attribute_damage(damage));
      const size_t outcome_index = resolution_.attribute_damage_trace.size() - 1;
      resolution_.outcomes.push_back(
        outcome_ref{outcome_store_kind::attribute_damage, outcome_index});
      add_category(resolution_.report.categories, execution_category::stat_change);
      record_death_check(
        outcome_store_kind::attribute_damage, outcome_index, damage.target);
      cursor.stage = resolution_stage::attribute_damage_after;
      return simul::step_control::advance;
    }

    case resolution_stage::attribute_damage_after:
      ++cursor.item_index;
      cursor.stage = resolution_stage::select_item;
      return simul::step_control::advance;

    case resolution_stage::effect_commit: {
      const auto request = resolution_.effects[cursor.effect_index];
      resolution_.effect_trace.push_back(resolve_effect(request));
      const size_t outcome_index = resolution_.effect_trace.size() - 1;
      resolution_.outcomes.push_back(
        outcome_ref{outcome_store_kind::effect, outcome_index});
      record_death_check(outcome_store_kind::effect, outcome_index, request.target);
      cursor.stage = resolution_stage::effect_after;
      return simul::step_control::advance;
    }

    case resolution_stage::effect_after:
      ++cursor.item_index;
      cursor.stage = resolution_stage::select_item;
      return simul::step_control::advance;

    case resolution_stage::beat_results: {
      bool waiting = false;
      if (mode_ == run_mode::animated) {
        const effect_beat& beat = resolution_.program.beats[cursor.beat_index];
        if (!active_beat_tasks_.empty() && active_beat_tasks_.size() != beat.effects.size()) {
          throw std::logic_error("cardgame presentation task batch shape changed");
        }
        for (size_t i = 0; i < active_beat_tasks_.size(); ++i) {
          const auto task = active_beat_tasks_[i];
          if (task == 0) continue;
          const authored_effect_report& call =
            resolution_.report.effects[cursor.beat_report_begin + i];
          pipe.expect_presentation(task, simul::presentation_event_kind::finished);

          presentation_command command;
          command.kind = presentation_command_kind::result;
          command.task = task;
          command.subject = subject_for(call);
          command.instance = call.id;
          command.targets = call.target_set.targets;
          command.target = command.targets.front();
          command.results = presentation_results(call);
          if (!command.results.empty()) {
            command.value = command.results.front().value;
            command.outcome = command.results.front().outcome;
          }
          presentation_outbox_.push_back(std::move(command));
          waiting = true;
        }
      }
      cursor.stage = resolution_stage::beat_after;
      return waiting ? simul::step_control::wait : simul::step_control::advance;
    }

    case resolution_stage::beat_after:
      active_beat_tasks_.clear();
      ++cursor.beat_index;
      cursor.stage = resolution_stage::select_beat;
      return simul::step_control::advance;

    case resolution_stage::done:
      return simul::step_control::advance;
  }
  return simul::step_control::halt;
}

void combat::countdown_pulse() {
  ++state_.countdown_pulse_index;
  if (state_.enemy_intent_active && state_.enemy_countdown > 0) {
    --state_.enemy_countdown;
  }
}

void combat::enter_group(combat_cursor& cursor, const combat_group group) {
  cursor.group = group;
  cursor.step = combat_step::enter;
  cursor.action.party = {};
}

void combat::trace(const combat_trace_kind kind,
                   const combat_group group,
                   const combat_cursor& cursor,
                   const entity_id actor,
                   const instance_id execution) {
  entity_id source_actor = invalid_entity;
  switch (group) {
    case combat_group::action_begin:
    case combat_group::card_effects:
    case combat_group::card_player_party_follow_ups:
    case combat_group::card_enemy_party_follow_ups:
    case combat_group::player_actor_state_tick:
    case combat_group::action_countdown:
    case combat_group::action_end:
      source_actor = cursor.action.player_actor;
      break;
    case combat_group::enemy_execution:
    case combat_group::enemy_action_enemy_party_follow_ups:
    case combat_group::enemy_action_player_party_follow_ups:
    case combat_group::enemy_actor_state_tick:
      source_actor = enemy_entity;
      break;
    case combat_group::turn_begin:
    case combat_group::awaiting_action:
    case combat_group::turn_end:
      break;
  }
  state_.trace.push_back(combat_trace_event{
    kind, group, cursor.action.token, execution, source_actor, actor});
}

std::vector<entity_id> combat::party_members(const combat_side side) const {
  const combatant_state& member =
    side == combat_side::player ? state_.player : state_.enemy;
  if (member.hp <= 0) return {};
  return {member.id};
}

bool combat::run_party_follow_ups(combat_cursor& cursor, const combat_side side) {
  switch (cursor.step) {
    case combat_step::enter:
      trace(combat_trace_kind::group_enter, cursor.group, cursor);
      if (!cursor.action.trigger_report.executed) return true;
      cursor.action.party.order = materialize_follow_up_order(
        party_members(side),
        follow_up_order_context{state_.combat_seed, cursor.action.token, side});
      cursor.action.party.actor_index = 0;
      cursor.step = combat_step::visit_party;
      return false;

    case combat_step::visit_party: {
      if (cursor.action.party.actor_index >= cursor.action.party.order.size()) {
        return true;
      }

      const entity_id actor =
        cursor.action.party.order[cursor.action.party.actor_index++];
      const combatant_state* participant = find_combatant(actor);
      if (participant == nullptr || participant->hp <= 0) return false;

      // Temporary native fixture for the future rule registry: one attack subscription per
      // participant, invoked once with the whole frozen report. Emitting zero new instances is a
      // valid script result and, importantly, does not open a recursive follow-up window.
      constexpr follow_up_enabler attack_listener{
        category_bit(execution_category::attack), 0};
      if (attack_listener.matches(cursor.action.trigger_report.categories)) {
        trace(combat_trace_kind::follow_up_rule,
              cursor.group,
              cursor,
              actor,
              cursor.action.trigger_report.execution);
      }
      return false;
    }

    default:
      throw std::logic_error("cardgame party follow-up cursor entered an invalid step");
  }
}

void combat::actor_state_tick(combat_cursor& cursor, const entity_id actor) {
  const combatant_state* participant = find_combatant(actor);
  if (participant == nullptr || participant->hp <= 0) return;

  // The group boundary is live now. The ordered DoT -> negative -> positive effect programs are
  // the next typed-instance slice; this fixture records exactly one tick for the source actor.
  trace(combat_trace_kind::actor_state_tick,
        cursor.group,
        cursor,
        actor,
        cursor.action.trigger_report.execution);
}

simul::step_control combat::run_step(combat_cursor& cursor, pipeline_type& pipe) {
  switch (cursor.phase) {
    case combat_phase::turn_begin:
      trace(combat_trace_kind::group_enter, combat_group::turn_begin, cursor);
      ++state_.turn_index;
      state_.enemy_countdown = enemy_start_countdown;
      state_.enemy_intent_active = true;
      cursor.phase = combat_phase::awaiting_action;
      enter_group(cursor, combat_group::awaiting_action);
      return simul::step_control::advance;

    case combat_phase::awaiting_action: {
      if (cursor.step == combat_step::enter) {
        trace(combat_trace_kind::group_enter, combat_group::awaiting_action, cursor);
        cursor.step = combat_step::wait_input;
      }
      if (!pending_intent_.has_value()) {
        return simul::step_control::halt;
      }
      const player_intent intent = *pending_intent_;
      pending_intent_.reset();
      if (intent.kind == player_intent_kind::end_turn) {
        cursor.phase = combat_phase::end_turn;
        cursor.action = {};
        enter_group(cursor, combat_group::turn_end);
      } else {
        ++state_.player_action_index;
        ++state_.action_cycle_index;
        cursor.active_card = intent.card;
        cursor.action = {};
        cursor.action.token = state_.action_cycle_index;
        cursor.action.player_actor = player_entity;
        cursor.phase = combat_phase::action_cycle;
        enter_group(cursor, combat_group::action_begin);
      }
      return simul::step_control::advance;
    }

    case combat_phase::action_cycle:
      switch (cursor.group) {
        case combat_group::action_begin:
          trace(combat_trace_kind::group_enter, cursor.group, cursor);
          if (state_.intercept_next_card) {
            state_.intercept_next_card = false;
            ++state_.stolen_card_count;
            cursor.action.card_stolen = true;
            resolution_ = {};
            cursor.resolution = {};
            resolution_.report = execution_report{
              allocate_execution(), player_entity, enemy_entity, false, {}};
            cursor.action.trigger_report = resolution_.report;
            trace(combat_trace_kind::card_stolen,
                  cursor.group,
                  cursor,
                  player_entity,
                  resolution_.report.execution);
            enter_group(cursor, combat_group::player_actor_state_tick);
          } else {
            enter_group(cursor, combat_group::card_effects);
          }
          return simul::step_control::advance;

        case combat_group::card_effects:
          if (cursor.step == combat_step::enter) {
            trace(combat_trace_kind::group_enter, cursor.group, cursor);
            begin_card_resolution(cursor);
            cursor.step = combat_step::resolve_execution;
            return simul::step_control::advance;
          }
          if (cursor.step != combat_step::resolve_execution) {
            throw std::logic_error("cardgame card execution entered an invalid step");
          }
          if (cursor.resolution.stage != resolution_stage::done) {
            return run_resolution_step(cursor, pipe);
          }
          cursor.action.trigger_report = resolution_.report;
          cursor.resolution = {};
          enter_group(cursor, combat_group::card_player_party_follow_ups);
          return simul::step_control::advance;

        case combat_group::card_player_party_follow_ups:
          if (run_party_follow_ups(cursor, combat_side::player)) {
            enter_group(cursor, combat_group::card_enemy_party_follow_ups);
          }
          return simul::step_control::advance;

        case combat_group::card_enemy_party_follow_ups:
          if (run_party_follow_ups(cursor, combat_side::enemy)) {
            enter_group(cursor, combat_group::player_actor_state_tick);
          }
          return simul::step_control::advance;

        case combat_group::player_actor_state_tick:
          trace(combat_trace_kind::group_enter, cursor.group, cursor);
          actor_state_tick(cursor, cursor.action.player_actor);
          enter_group(cursor, combat_group::action_countdown);
          return simul::step_control::advance;

        case combat_group::action_countdown:
          trace(combat_trace_kind::group_enter, cursor.group, cursor);
          if (cursor.action.card_stolen || advances_countdown(cursor.active_card)) {
            countdown_pulse();
            trace(combat_trace_kind::countdown_pulse, cursor.group, cursor);
          }
          enter_group(cursor,
                      state_.enemy_intent_active && state_.enemy_countdown == 0
                        ? combat_group::enemy_execution
                        : combat_group::action_end);
          return simul::step_control::advance;

        case combat_group::enemy_execution:
          if (cursor.step == combat_step::enter) {
            trace(combat_trace_kind::group_enter, cursor.group, cursor);
            begin_enemy_resolution(cursor);
            cursor.step = combat_step::resolve_execution;
            return simul::step_control::advance;
          }
          if (cursor.step != combat_step::resolve_execution) {
            throw std::logic_error("cardgame enemy execution entered an invalid step");
          }
          if (cursor.resolution.stage != resolution_stage::done) {
            return run_resolution_step(cursor, pipe);
          }
          cursor.action.trigger_report = resolution_.report;
          cursor.resolution = {};
          enter_group(cursor, combat_group::enemy_action_enemy_party_follow_ups);
          return simul::step_control::advance;

        case combat_group::enemy_action_enemy_party_follow_ups:
          if (run_party_follow_ups(cursor, combat_side::enemy)) {
            enter_group(cursor, combat_group::enemy_action_player_party_follow_ups);
          }
          return simul::step_control::advance;

        case combat_group::enemy_action_player_party_follow_ups:
          if (run_party_follow_ups(cursor, combat_side::player)) {
            enter_group(cursor, combat_group::enemy_actor_state_tick);
          }
          return simul::step_control::advance;

        case combat_group::enemy_actor_state_tick:
          trace(combat_trace_kind::group_enter, cursor.group, cursor);
          actor_state_tick(cursor, enemy_entity);
          state_.enemy_intent_active = false;
          if (cursor.action.forced_enemy_cycle) {
            cursor.phase = combat_phase::end_turn;
            cursor.step = combat_step::forced_pulse;
          } else {
            enter_group(cursor, combat_group::action_end);
          }
          return simul::step_control::advance;

        case combat_group::action_end:
          trace(combat_trace_kind::group_enter, cursor.group, cursor);
          cursor.action = {};
          cursor.phase = combat_phase::awaiting_action;
          enter_group(cursor, combat_group::awaiting_action);
          return simul::step_control::advance;

        case combat_group::turn_begin:
        case combat_group::awaiting_action:
        case combat_group::turn_end:
          throw std::logic_error("cardgame action cycle entered a non-action group");
      }
      throw std::logic_error("cardgame action cycle group is invalid");

    case combat_phase::end_turn:
      switch (cursor.step) {
        case combat_step::enter:
          trace(combat_trace_kind::group_enter, combat_group::turn_end, cursor);
          cursor.step = combat_step::forced_pulse;
          return simul::step_control::advance;
        case combat_step::forced_pulse:
          if (!state_.enemy_intent_active) {
            cursor.step = combat_step::turn_done;
            return simul::step_control::advance;
          }
          if (state_.enemy_countdown == 0) {
            ++state_.action_cycle_index;
            cursor.action = {};
            cursor.action.token = state_.action_cycle_index;
            cursor.action.forced_enemy_cycle = true;
            cursor.phase = combat_phase::action_cycle;
            enter_group(cursor, combat_group::enemy_execution);
            return simul::step_control::advance;
          }
          countdown_pulse();
          trace(combat_trace_kind::countdown_pulse, combat_group::turn_end, cursor);
          return simul::step_control::advance;
        case combat_step::turn_done:
          cursor.action = {};
          cursor.phase = combat_phase::turn_begin;
          enter_group(cursor, combat_group::turn_begin);
          return simul::step_control::advance;
        default:
          throw std::logic_error("cardgame end-turn cursor entered an invalid step");
      }

    case combat_phase::battle_over:
      return simul::step_control::halt;
  }
  return simul::step_control::halt;
}

uint64_t combat::barrier_budget() const noexcept {
  return 600;
}

void combat::on_barrier_timeout(
  const combat_cursor&,
  const simul::presentation_barrier&) noexcept {
  timeout_reported_ = true;
}

} // namespace core
} // namespace cardgame
