#ifndef DEVILS_ENGINE_CARDGAME_COMBAT_H
#define DEVILS_ENGINE_CARDGAME_COMBAT_H

#include <cstdint>
#include <optional>
#include <vector>

#include <devils_engine/simul/turn_pipeline.h>

namespace cardgame {
namespace core {

namespace simul = devils_engine::simul;

enum class run_mode : uint8_t {
  headless,
  animated
};

enum class card_kind : uint8_t {
  strike,       // damage 3, advances countdown
  quick_strike // damage 1, remains a player action but does not advance countdown
};

enum class player_intent_kind : uint8_t {
  play_card,
  end_turn
};

// Project-defined input. This deliberately does not extend act::intent_kind: card instance,
// target list, optional payment and choices will grow here without changing the engine ABI.
struct player_intent {
  player_intent_kind kind = player_intent_kind::play_card;
  card_kind card = card_kind::strike;
  uint64_t sequence = 0;
};

enum class combat_phase : uint8_t {
  turn_begin,
  awaiting_action,
  resolving_action,
  end_turn,
  battle_over
};

enum class combat_step : uint8_t {
  enter,
  card_cue,
  card_commit,
  card_after,
  action_countdown,
  enemy_cue,
  enemy_commit,
  enemy_after,
  action_done,
  forced_pulse,
  turn_done
};

enum class presentation_subject : uint8_t {
  player_attack,
  enemy_attack
};

enum class presentation_command_kind : uint8_t {
  start,
  result
};

struct presentation_command {
  presentation_command_kind kind = presentation_command_kind::start;
  simul::presentation_task_id task = 0;
  presentation_subject subject = presentation_subject::player_attack;
  int32_t value = 0; // authoritative damage/result on `result`, zero on `start`
};

// Entire authoritative state of the first combat slice. Presentation state is absent.
struct combat_state {
  int32_t player_hp = 30;
  int32_t enemy_hp = 100;
  int32_t enemy_countdown = 0;
  uint64_t turn_index = 0;
  uint64_t player_action_index = 0;
  uint64_t countdown_pulse_index = 0;
  bool enemy_intent_active = false;
  constexpr bool operator==(const combat_state&) const noexcept = default;
};

// Serializable project cursor. `active_task` is sanitized to zero by save(): task ids and
// in-flight animations are derived runtime state, while phase/step are authoritative.
struct combat_cursor {
  combat_phase phase = combat_phase::turn_begin;
  combat_step step = combat_step::enter;
  card_kind active_card = card_kind::strike;
  simul::presentation_task_id active_task = 0;
  constexpr bool operator==(const combat_cursor&) const noexcept = default;
};

class combat {
public:
  using pipeline_type = simul::turn_pipeline<combat_cursor>;

  struct snapshot {
    combat_state state{};
    pipeline_type::snapshot pipeline{};
    std::optional<player_intent> pending_intent;
    uint64_t next_presentation_task = 1;
  };

  explicit combat(run_mode mode = run_mode::headless) noexcept;

  bool submit(player_intent intent);
  void update(uint64_t engine_tick);
  bool notify_presentation(simul::presentation_task_id task,
                           simul::presentation_event_kind kind) noexcept;
  std::vector<presentation_command> take_presentation_commands();

  const combat_state& state() const noexcept;
  const combat_cursor& cursor() const noexcept;
  bool awaiting_player() const noexcept;
  bool waiting_presentation() const noexcept;
  bool faulted() const noexcept;

  snapshot save() const;
  void load(const snapshot& value);

  // Project policy seam. The real data-driven version becomes a ds predicate over card data;
  // importantly it is not inferred from player_action_index.
  static bool advances_countdown(card_kind card) noexcept;

  // turn_pipeline host contract
  simul::step_control run_step(combat_cursor& cursor, pipeline_type& pipe);
  uint64_t barrier_budget() const noexcept;
  void on_barrier_timeout(const combat_cursor&, const simul::presentation_barrier&) noexcept;

private:
  simul::step_control begin_attack(combat_cursor& cursor, pipeline_type& pipe,
                                   presentation_subject subject, combat_step commit_step);
  simul::step_control commit_attack(combat_cursor& cursor, pipeline_type& pipe,
                                    presentation_subject subject, int32_t damage,
                                    combat_step after_step);
  void countdown_pulse();

  pipeline_type pipeline_;
  combat_state state_;
  std::optional<player_intent> pending_intent_;
  std::vector<presentation_command> presentation_outbox_;
  uint64_t next_presentation_task_ = 1;
  run_mode mode_ = run_mode::headless;
  bool timeout_reported_ = false;
};

} // namespace core
} // namespace cardgame

#endif
