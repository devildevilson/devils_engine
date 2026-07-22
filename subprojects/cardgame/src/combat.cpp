#include "cardgame/combat.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace cardgame {
namespace core {
namespace {

constexpr int32_t enemy_attack_damage = 2;
constexpr int32_t enemy_start_countdown = 2;
constexpr int32_t basis_points = 10000;

constexpr uint64_t effect_bit(const effect_kind kind) noexcept {
  return uint64_t{1} << static_cast<uint8_t>(kind);
}

presentation_subject attack_subject(const attack_instance& attack) noexcept {
  return attack.source == player_entity ? presentation_subject::player_attack
                                        : presentation_subject::enemy_attack;
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
  return pipeline_.faulted() || timeout_reported_;
}

combat::snapshot combat::save() const {
  auto pipeline = pipeline_.save();
  pipeline.cursor.resolution.active_task = 0;
  return snapshot{state_, pipeline, resolution_, pending_intent_, next_presentation_task_,
                  next_instance_};
}

void combat::load(const snapshot& value) {
  state_ = value.state;
  resolution_ = value.resolution;
  pending_intent_ = value.pending_intent;
  next_presentation_task_ = value.next_presentation_task;
  next_instance_ = value.next_instance;
  presentation_outbox_.clear();
  timeout_reported_ = false;
  pipeline_.load(value.pipeline);
}

bool combat::advances_countdown(const card_kind card) noexcept {
  return card != card_kind::quick_strike;
}

instance_id combat::allocate_instance() noexcept {
  return next_instance_++;
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
  cursor.resolution.stage = resolution_stage::select_item;

  const auto attack = [this](const instance_id parent, const element type,
                             const int32_t damage, const bool applies_element) {
    const size_t index = resolution_.attacks.size();
    resolution_.attacks.push_back(
      attack_instance{allocate_instance(), parent, player_entity, enemy_entity,
                      type, damage, applies_element});
    resolution_.plan.push_back(resolution_item{resolution_item_kind::attack, index});
  };

  const instance_id execution = state_.player_action_index;
  switch (cursor.active_card) {
    case card_kind::strike:
      attack(execution, element::none, 3, false);
      break;
    case card_kind::quick_strike:
      attack(execution, element::none, 1, false);
      break;
    case card_kind::fire_strike:
      attack(execution, element::fire, 4, true);
      resolution_.plan.push_back(
        resolution_item{resolution_item_kind::effect, resolution_.effects.size()});
      resolution_.effects.push_back(
        effect_request{allocate_instance(), execution, player_entity, enemy_entity,
                       effect_kind::burning, 1, 2});
      break;
    case card_kind::double_strike:
      attack(execution, element::none, 2, false);
      attack(execution, element::none, 2, false);
      break;
  }
}

void combat::begin_enemy_resolution(combat_cursor& cursor) {
  resolution_ = {};
  cursor.resolution = {};
  cursor.resolution.stage = resolution_stage::select_item;
  resolution_.attacks.push_back(
    attack_instance{allocate_instance(), state_.turn_index, enemy_entity, player_entity,
                    element::none, enemy_attack_damage, false});
  resolution_.plan.push_back(resolution_item{resolution_item_kind::attack, 0});
}

simul::step_control combat::begin_presentation(
  resolution_cursor& cursor,
  pipeline_type& pipe,
  const presentation_subject subject,
  const instance_id instance,
  const entity_id target,
  const resolution_stage commit_stage) {
  cursor.active_task = 0;
  if (mode_ == run_mode::animated) {
    cursor.active_task = next_presentation_task_++;
    // Arm before publishing: a real render worker may answer immediately.
    pipe.expect_presentation(cursor.active_task, simul::presentation_event_kind::gameplay);
    presentation_outbox_.push_back(
      presentation_command{presentation_command_kind::start, cursor.active_task, subject,
                           instance, target, 0, 0});
  }
  cursor.stage = commit_stage;
  return mode_ == run_mode::animated ? simul::step_control::wait
                                     : simul::step_control::advance;
}

simul::step_control combat::publish_result(
  resolution_cursor& cursor,
  pipeline_type& pipe,
  const presentation_subject subject,
  const instance_id instance,
  const entity_id target,
  const int32_t value,
  const uint32_t outcome,
  const resolution_stage after_stage) {
  const bool has_live_presentation = mode_ == run_mode::animated && cursor.active_task != 0;
  if (has_live_presentation) {
    pipe.expect_presentation(cursor.active_task, simul::presentation_event_kind::finished);
    presentation_outbox_.push_back(
      presentation_command{presentation_command_kind::result, cursor.active_task, subject,
                           instance, target, value, outcome});
  }
  cursor.stage = after_stage;
  return has_live_presentation ? simul::step_control::wait
                               : simul::step_control::advance;
}

std::vector<damage_modifier> combat::collect_resistance_modifiers(
  const combatant_state& target,
  const damage_instance& damage) const {
  std::vector<damage_modifier> modifiers;
  const size_t index = static_cast<size_t>(damage.type);
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
  combatant_state* target = find_combatant(damage.target);
  if (target == nullptr || target->hp <= 0) {
    return result;
  }

  result.target_valid = true;
  result.modifiers = collect_resistance_modifiers(*target, damage);
  int64_t current = std::max<int32_t>(damage.base_damage, 0);
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
    current = std::clamp<int64_t>(current, 0, std::numeric_limits<int32_t>::max());
  }

  result.modified_damage = static_cast<int32_t>(current);
  result.shield_absorbed = std::min(target->shield, result.modified_damage);
  target->shield -= result.shield_absorbed;
  const int32_t remaining = result.modified_damage - result.shield_absorbed;
  result.hp_loss = std::min(target->hp, remaining);
  target->hp -= result.hp_loss;
  return result;
}

void combat::resolve_attack(const attack_instance& attack) {
  damage_instance damage{allocate_instance(), attack.id, attack.source, attack.target,
                         attack.type, damage_channel::attack, attack.base_damage, true};
  damage_outcome outcome = resolve_damage(damage);
  resolution_.damage_trace.push_back(outcome);

  combatant_state* target = find_combatant(attack.target);
  if (target == nullptr || !outcome.target_valid) {
    return;
  }

  // An elemental collision is detected once per attack instance. Its gameplay is a later resolver
  // stage even if presentation eventually chooses to overlap both animations.
  if (attack.applies_element && attack.type != element::none) {
    if (target->elemental_mark == element::none) {
      target->elemental_mark = attack.type;
    } else if (target->elemental_mark != attack.type) {
      target->elemental_mark = element::none;
      resolution_.reactions.push_back(
        reaction_request{allocate_instance(), attack.id, attack.source, attack.target,
                         attack.type, 2});
    }
  }

  // `thorns` listens to attack damage, not to reaction/DoT/returned channels. Every eligible
  // attack appends its own response instance; it never recursively calls the resolver.
  if (damage.can_return && outcome.hp_loss > 0) {
    for (const auto& effect : target->effects) {
      if (effect.kind != effect_kind::thorns || effect.stacks <= 0) continue;
      resolution_.responses.push_back(
        damage_instance{allocate_instance(), attack.id, attack.target, attack.source,
                        element::none, damage_channel::returned, effect.stacks, false});
    }
  }
}

void combat::resolve_reaction(const reaction_request& reaction) {
  damage_instance damage{allocate_instance(), reaction.id, reaction.source, reaction.target,
                         reaction.type, damage_channel::reaction, reaction.base_damage, false};
  resolution_.damage_trace.push_back(resolve_damage(damage));
}

void combat::resolve_response(const damage_instance& response) {
  resolution_.damage_trace.push_back(resolve_damage(response));
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

simul::step_control combat::run_resolution_step(combat_cursor& combat_cursor,
                                                pipeline_type& pipe) {
  resolution_cursor& cursor = combat_cursor.resolution;
  switch (cursor.stage) {
    case resolution_stage::idle:
      cursor.stage = resolution_stage::select_item;
      return simul::step_control::advance;

    case resolution_stage::select_item:
      if (cursor.item_index >= resolution_.plan.size()) {
        cursor.stage = resolution_stage::done;
        return simul::step_control::advance;
      }
      if (resolution_.plan[cursor.item_index].kind == resolution_item_kind::attack) {
        cursor.attack_index = resolution_.plan[cursor.item_index].index;
        cursor.stage = resolution_stage::attack_cue;
      } else {
        cursor.effect_index = resolution_.plan[cursor.item_index].index;
        cursor.stage = resolution_stage::effect_cue;
      }
      return simul::step_control::advance;

    case resolution_stage::attack_cue: {
      const auto& attack = resolution_.attacks[cursor.attack_index];
      return begin_presentation(cursor, pipe, attack_subject(attack), attack.id, attack.target,
                                resolution_stage::attack_commit);
    }

    case resolution_stage::attack_commit: {
      const auto attack = resolution_.attacks[cursor.attack_index];
      resolve_attack(attack);
      const auto& outcome = resolution_.damage_trace.back();
      return publish_result(cursor, pipe, attack_subject(attack), attack.id, attack.target,
                            outcome.hp_loss, 0, resolution_stage::attack_after);
    }

    case resolution_stage::attack_after:
      cursor.active_task = 0;
      cursor.stage = resolution_stage::select_reaction;
      return simul::step_control::advance;

    case resolution_stage::select_reaction:
      cursor.stage = cursor.reaction_index < resolution_.reactions.size()
                       ? resolution_stage::reaction_cue
                       : resolution_stage::select_response;
      return simul::step_control::advance;

    case resolution_stage::reaction_cue: {
      const auto& reaction = resolution_.reactions[cursor.reaction_index];
      return begin_presentation(cursor, pipe, presentation_subject::elemental_reaction,
                                reaction.id, reaction.target, resolution_stage::reaction_commit);
    }

    case resolution_stage::reaction_commit: {
      const auto reaction = resolution_.reactions[cursor.reaction_index];
      resolve_reaction(reaction);
      const auto& outcome = resolution_.damage_trace.back();
      return publish_result(cursor, pipe, presentation_subject::elemental_reaction,
                            reaction.id, reaction.target, outcome.hp_loss, 0,
                            resolution_stage::reaction_after);
    }

    case resolution_stage::reaction_after:
      cursor.active_task = 0;
      ++cursor.reaction_index;
      cursor.stage = resolution_stage::select_reaction;
      return simul::step_control::advance;

    case resolution_stage::select_response:
      cursor.stage = cursor.response_index < resolution_.responses.size()
                       ? resolution_stage::response_cue
                       : resolution_stage::post_attack;
      return simul::step_control::advance;

    case resolution_stage::response_cue: {
      const auto& response = resolution_.responses[cursor.response_index];
      return begin_presentation(cursor, pipe, presentation_subject::returned_damage,
                                response.id, response.target, resolution_stage::response_commit);
    }

    case resolution_stage::response_commit: {
      const auto response = resolution_.responses[cursor.response_index];
      resolve_response(response);
      const auto& outcome = resolution_.damage_trace.back();
      return publish_result(cursor, pipe, presentation_subject::returned_damage,
                            response.id, response.target, outcome.hp_loss, 0,
                            resolution_stage::response_after);
    }

    case resolution_stage::response_after:
      cursor.active_task = 0;
      ++cursor.response_index;
      cursor.stage = resolution_stage::select_response;
      return simul::step_control::advance;

    case resolution_stage::post_attack:
      // HP reaches zero immediately, while destructive entity cleanup/death effects will be a
      // later explicit project stage. Reactions and responses collected by this hit already ran.
      ++cursor.item_index;
      cursor.stage = resolution_stage::select_item;
      return simul::step_control::advance;

    case resolution_stage::effect_cue: {
      const auto& request = resolution_.effects[cursor.effect_index];
      return begin_presentation(cursor, pipe, presentation_subject::effect,
                                request.id, request.target, resolution_stage::effect_commit);
    }

    case resolution_stage::effect_commit: {
      const auto request = resolution_.effects[cursor.effect_index];
      resolution_.effect_trace.push_back(resolve_effect(request));
      const auto& outcome = resolution_.effect_trace.back();
      return publish_result(cursor, pipe, presentation_subject::effect,
                            request.id, request.target, outcome.resulting_stacks,
                            static_cast<uint32_t>(outcome.result),
                            resolution_stage::effect_after);
    }

    case resolution_stage::effect_after:
      cursor.active_task = 0;
      ++cursor.item_index;
      cursor.stage = resolution_stage::select_item;
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

simul::step_control combat::run_step(combat_cursor& cursor, pipeline_type& pipe) {
  switch (cursor.phase) {
    case combat_phase::turn_begin:
      ++state_.turn_index;
      state_.enemy_countdown = enemy_start_countdown;
      state_.enemy_intent_active = true;
      cursor.phase = combat_phase::awaiting_action;
      cursor.step = combat_step::enter;
      return simul::step_control::advance;

    case combat_phase::awaiting_action: {
      if (!pending_intent_.has_value()) {
        return simul::step_control::halt;
      }
      const player_intent intent = *pending_intent_;
      pending_intent_.reset();
      if (intent.kind == player_intent_kind::end_turn) {
        cursor.phase = combat_phase::end_turn;
        cursor.step = combat_step::forced_pulse;
      } else {
        ++state_.player_action_index;
        cursor.active_card = intent.card;
        cursor.phase = combat_phase::resolving_action;
        cursor.step = combat_step::build_player_resolution;
      }
      return simul::step_control::advance;
    }

    case combat_phase::resolving_action:
      switch (cursor.step) {
        case combat_step::build_player_resolution:
          begin_card_resolution(cursor);
          cursor.step = combat_step::resolve_player;
          return simul::step_control::advance;
        case combat_step::resolve_player:
          if (cursor.resolution.stage != resolution_stage::done) {
            return run_resolution_step(cursor, pipe);
          }
          cursor.resolution = {};
          cursor.step = combat_step::action_countdown;
          return simul::step_control::advance;
        case combat_step::action_countdown:
          if (advances_countdown(cursor.active_card)) {
            countdown_pulse();
          }
          cursor.step = state_.enemy_intent_active && state_.enemy_countdown == 0
                          ? combat_step::build_enemy_resolution
                          : combat_step::action_done;
          return simul::step_control::advance;
        case combat_step::build_enemy_resolution:
          begin_enemy_resolution(cursor);
          cursor.step = combat_step::resolve_enemy;
          return simul::step_control::advance;
        case combat_step::resolve_enemy:
          if (cursor.resolution.stage != resolution_stage::done) {
            return run_resolution_step(cursor, pipe);
          }
          cursor.resolution = {};
          state_.enemy_intent_active = false;
          cursor.step = combat_step::action_done;
          return simul::step_control::advance;
        case combat_step::action_done:
          cursor.phase = combat_phase::awaiting_action;
          cursor.step = combat_step::enter;
          return simul::step_control::advance;
        default:
          return simul::step_control::halt;
      }

    case combat_phase::end_turn:
      switch (cursor.step) {
        case combat_step::forced_pulse:
          if (!state_.enemy_intent_active) {
            cursor.step = combat_step::turn_done;
            return simul::step_control::advance;
          }
          if (state_.enemy_countdown == 0) {
            cursor.step = combat_step::build_enemy_resolution;
            return simul::step_control::advance;
          }
          countdown_pulse();
          return simul::step_control::advance;
        case combat_step::build_enemy_resolution:
          begin_enemy_resolution(cursor);
          cursor.step = combat_step::resolve_enemy;
          return simul::step_control::advance;
        case combat_step::resolve_enemy:
          if (cursor.resolution.stage != resolution_stage::done) {
            return run_resolution_step(cursor, pipe);
          }
          cursor.resolution = {};
          state_.enemy_intent_active = false;
          cursor.step = combat_step::forced_pulse;
          return simul::step_control::advance;
        case combat_step::turn_done:
          cursor.phase = combat_phase::turn_begin;
          cursor.step = combat_step::enter;
          return simul::step_control::advance;
        default:
          return simul::step_control::halt;
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
