#include "cardgame/combat.h"

#include <utility>

namespace cardgame {
namespace core {
namespace {

constexpr int32_t enemy_attack_damage = 2;
constexpr int32_t enemy_start_countdown = 2;

int32_t card_damage(const card_kind card) noexcept {
  switch (card) {
    case card_kind::strike: return 3;
    case card_kind::quick_strike: return 1;
  }
  return 0;
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
  pipeline.cursor.active_task = 0; // in-flight render task is derived and cannot survive resume
  return snapshot{state_, pipeline, pending_intent_, next_presentation_task_};
}

void combat::load(const snapshot& value) {
  state_ = value.state;
  pending_intent_ = value.pending_intent;
  next_presentation_task_ = value.next_presentation_task;
  presentation_outbox_.clear();
  timeout_reported_ = false;
  pipeline_.load(value.pipeline);
}

bool combat::advances_countdown(const card_kind card) noexcept {
  return card != card_kind::quick_strike;
}

simul::step_control combat::begin_attack(
  combat_cursor& cursor,
  pipeline_type& pipe,
  const presentation_subject subject,
  const combat_step commit_step) {
  cursor.active_task = 0;
  if (mode_ == run_mode::animated) {
    cursor.active_task = next_presentation_task_++;
    // Arm the checkpoint before publishing work: a real render worker may answer immediately.
    pipe.expect_presentation(cursor.active_task, simul::presentation_event_kind::gameplay);
    presentation_outbox_.push_back(
      presentation_command{presentation_command_kind::start, cursor.active_task, subject, 0});
  }
  cursor.step = commit_step; // resume with the animation dropped commits exactly once
  return mode_ == run_mode::animated ? simul::step_control::wait : simul::step_control::advance;
}

simul::step_control combat::commit_attack(
  combat_cursor& cursor,
  pipeline_type& pipe,
  const presentation_subject subject,
  const int32_t damage,
  const combat_step after_step) {
  if (subject == presentation_subject::player_attack) {
    state_.enemy_hp -= damage;
  } else {
    state_.player_hp -= damage;
  }

  const bool has_live_presentation = mode_ == run_mode::animated && cursor.active_task != 0;
  if (has_live_presentation) {
    pipe.expect_presentation(cursor.active_task, simul::presentation_event_kind::finished);
    presentation_outbox_.push_back(
      presentation_command{presentation_command_kind::result, cursor.active_task, subject, damage});
  }
  cursor.step = after_step; // resume after commit skips the lost result animation
  return has_live_presentation ? simul::step_control::wait : simul::step_control::advance;
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
        cursor.step = combat_step::card_cue;
      }
      return simul::step_control::advance;
    }

    case combat_phase::resolving_action:
      switch (cursor.step) {
        case combat_step::card_cue:
          return begin_attack(cursor, pipe, presentation_subject::player_attack,
                              combat_step::card_commit);
        case combat_step::card_commit:
          return commit_attack(cursor, pipe, presentation_subject::player_attack,
                               card_damage(cursor.active_card), combat_step::card_after);
        case combat_step::card_after:
          cursor.active_task = 0;
          cursor.step = combat_step::action_countdown;
          return simul::step_control::advance;
        case combat_step::action_countdown:
          if (advances_countdown(cursor.active_card)) {
            countdown_pulse();
          }
          cursor.step = state_.enemy_intent_active && state_.enemy_countdown == 0
                          ? combat_step::enemy_cue
                          : combat_step::action_done;
          return simul::step_control::advance;
        case combat_step::enemy_cue:
          return begin_attack(cursor, pipe, presentation_subject::enemy_attack,
                              combat_step::enemy_commit);
        case combat_step::enemy_commit:
          return commit_attack(cursor, pipe, presentation_subject::enemy_attack,
                               enemy_attack_damage, combat_step::enemy_after);
        case combat_step::enemy_after:
          cursor.active_task = 0;
          state_.enemy_intent_active = false;
          cursor.step = combat_step::action_done;
          return simul::step_control::advance;
        case combat_step::action_done:
          cursor.phase = combat_phase::awaiting_action;
          cursor.step = combat_step::enter;
          return simul::step_control::advance;
        default: return simul::step_control::halt;
      }

    case combat_phase::end_turn:
      switch (cursor.step) {
        case combat_step::forced_pulse:
          if (!state_.enemy_intent_active || state_.enemy_countdown == 0) {
            cursor.step = state_.enemy_intent_active ? combat_step::enemy_cue
                                                     : combat_step::turn_done;
            return simul::step_control::advance;
          }
          countdown_pulse();
          cursor.step = state_.enemy_countdown == 0 ? combat_step::enemy_cue
                                                    : combat_step::forced_pulse;
          return simul::step_control::advance;
        case combat_step::enemy_cue:
          return begin_attack(cursor, pipe, presentation_subject::enemy_attack,
                              combat_step::enemy_commit);
        case combat_step::enemy_commit:
          return commit_attack(cursor, pipe, presentation_subject::enemy_attack,
                               enemy_attack_damage, combat_step::enemy_after);
        case combat_step::enemy_after:
          cursor.active_task = 0;
          state_.enemy_intent_active = false;
          cursor.step = combat_step::forced_pulse;
          return simul::step_control::advance;
        case combat_step::turn_done:
          cursor.phase = combat_phase::turn_begin;
          cursor.step = combat_step::enter;
          return simul::step_control::advance;
        default: return simul::step_control::halt;
      }

    case combat_phase::battle_over:
      return simul::step_control::halt;
  }
  return simul::step_control::halt;
}

uint64_t combat::barrier_budget() const noexcept {
  return 600; // engine ticks; project settings will own the real value
}

void combat::on_barrier_timeout(
  const combat_cursor&,
  const simul::presentation_barrier&) noexcept {
  timeout_reported_ = true;
}

} // namespace core
} // namespace cardgame
