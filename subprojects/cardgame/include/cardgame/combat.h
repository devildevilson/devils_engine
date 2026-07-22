#ifndef DEVILS_ENGINE_CARDGAME_COMBAT_H
#define DEVILS_ENGINE_CARDGAME_COMBAT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <devils_engine/simul/turn_pipeline.h>

namespace cardgame {
namespace core {

namespace simul = devils_engine::simul;

using entity_id = uint32_t;
using instance_id = uint64_t;

inline constexpr entity_id invalid_entity = 0;
inline constexpr entity_id player_entity = 1;
inline constexpr entity_id enemy_entity = 2;

enum class run_mode : uint8_t {
  headless,
  animated
};

enum class card_kind : uint8_t {
  strike,       // physical damage 3, advances countdown
  quick_strike, // physical damage 1, does not advance countdown
  fire_strike,  // fire damage 4 + one burning effect request
  double_strike // two physical attack instances with damage 2 each
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

enum class element : uint8_t {
  none,
  fire,
  water,
  ice,
  count
};

inline constexpr size_t element_count = static_cast<size_t>(element::count);

enum class damage_channel : uint8_t {
  attack,
  reaction,
  returned,
  dot,
  hp_cost
};

enum class damage_modifier_kind : uint8_t {
  add,
  multiply_basis_points,
  clamp_max,
  block
};

struct damage_modifier {
  instance_id source = 0;
  damage_modifier_kind kind = damage_modifier_kind::add;
  int32_t value = 0;
  constexpr bool operator==(const damage_modifier&) const noexcept = default;
};

struct attack_instance {
  instance_id id = 0;
  instance_id parent_execution = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  element type = element::none;
  int32_t base_damage = 0;
  bool applies_element = false;
  constexpr bool operator==(const attack_instance&) const noexcept = default;
};

struct damage_instance {
  instance_id id = 0;
  instance_id parent = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  element type = element::none;
  damage_channel channel = damage_channel::attack;
  int32_t base_damage = 0;
  // Returned/reaction damage is deliberately non-recursive by default.
  bool can_return = false;
  constexpr bool operator==(const damage_instance&) const noexcept = default;
};

struct damage_outcome {
  damage_instance damage{};
  std::vector<damage_modifier> modifiers;
  int32_t modified_damage = 0;
  int32_t shield_absorbed = 0;
  int32_t hp_loss = 0;
  bool target_valid = false;
  bool operator==(const damage_outcome&) const noexcept = default;
};

enum class effect_kind : uint8_t {
  burning,
  thorns,
  count
};

inline constexpr size_t effect_count = static_cast<size_t>(effect_kind::count);
static_assert(effect_count <= 64, "effect immunity mask needs a wider representation");

struct effect_state {
  instance_id id = 0;
  effect_kind kind = effect_kind::burning;
  entity_id source = invalid_entity;
  int32_t stacks = 0;
  int32_t remaining_pulses = 0;
  constexpr bool operator==(const effect_state&) const noexcept = default;
};

struct effect_request {
  instance_id id = 0;
  instance_id parent_execution = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  effect_kind kind = effect_kind::burning;
  int32_t stacks = 1;
  int32_t remaining_pulses = 0;
  constexpr bool operator==(const effect_request&) const noexcept = default;
};

enum class effect_apply_result : uint8_t {
  added,
  updated,
  immune,
  invalid_target,
  rejected
};

struct effect_outcome {
  effect_request request{};
  effect_apply_result result = effect_apply_result::rejected;
  int32_t previous_stacks = 0;
  int32_t resulting_stacks = 0;
  constexpr bool operator==(const effect_outcome&) const noexcept = default;
};

struct combatant_state {
  entity_id id = invalid_entity;
  int32_t hp = 0;
  int32_t shield = 0;
  // Positive values reduce damage; -2500 means 25% vulnerability, 10000 means immunity.
  std::array<int32_t, element_count> resistance_basis_points{};
  element elemental_mark = element::none;
  uint64_t effect_immunity_mask = 0;
  std::vector<effect_state> effects;
  bool operator==(const combatant_state&) const noexcept = default;
};

struct reaction_request {
  instance_id id = 0;
  instance_id parent_attack = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  element type = element::none;
  int32_t base_damage = 0;
  constexpr bool operator==(const reaction_request&) const noexcept = default;
};

enum class resolution_item_kind : uint8_t {
  attack,
  effect
};

struct resolution_item {
  resolution_item_kind kind = resolution_item_kind::attack;
  size_t index = 0;
  constexpr bool operator==(const resolution_item&) const noexcept = default;
};

struct resolution_work {
  std::vector<attack_instance> attacks;
  std::vector<effect_request> effects;
  // Materialized card-text order. Instance storage stays separated by type, while this plan keeps
  // `buff -> attack` observably different from `attack -> debuff`.
  std::vector<resolution_item> plan;
  std::vector<reaction_request> reactions;
  std::vector<damage_instance> responses;
  std::vector<damage_outcome> damage_trace;
  std::vector<effect_outcome> effect_trace;
  bool operator==(const resolution_work&) const noexcept = default;
};

enum class resolution_stage : uint8_t {
  idle,
  select_item,
  attack_cue,
  attack_commit,
  attack_after,
  select_reaction,
  reaction_cue,
  reaction_commit,
  reaction_after,
  select_response,
  response_cue,
  response_commit,
  response_after,
  post_attack,
  effect_cue,
  effect_commit,
  effect_after,
  done
};

struct resolution_cursor {
  resolution_stage stage = resolution_stage::idle;
  size_t item_index = 0;
  size_t attack_index = 0;
  size_t reaction_index = 0;
  size_t response_index = 0;
  size_t effect_index = 0;
  simul::presentation_task_id active_task = 0;
  constexpr bool operator==(const resolution_cursor&) const noexcept = default;
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
  build_player_resolution,
  resolve_player,
  action_countdown,
  build_enemy_resolution,
  resolve_enemy,
  action_done,
  forced_pulse,
  turn_done
};

enum class presentation_subject : uint8_t {
  player_attack,
  enemy_attack,
  elemental_reaction,
  returned_damage,
  effect
};

enum class presentation_command_kind : uint8_t {
  start,
  result
};

struct presentation_command {
  presentation_command_kind kind = presentation_command_kind::start;
  simul::presentation_task_id task = 0;
  presentation_subject subject = presentation_subject::player_attack;
  instance_id instance = 0;
  entity_id target = invalid_entity;
  int32_t value = 0;
  uint32_t outcome = 0;
};

// Entire authoritative state of the first combat slice. Presentation state is absent.
struct combat_state {
  combatant_state player{player_entity, 30, 0, {}, element::none, 0, {}};
  combatant_state enemy{enemy_entity, 100, 0, {}, element::none, 0, {}};
  int32_t enemy_countdown = 0;
  uint64_t turn_index = 0;
  uint64_t player_action_index = 0;
  uint64_t countdown_pulse_index = 0;
  bool enemy_intent_active = false;
  bool operator==(const combat_state&) const noexcept = default;
};

// Serializable project cursor. Presentation task ids are sanitized by save().
struct combat_cursor {
  combat_phase phase = combat_phase::turn_begin;
  combat_step step = combat_step::enter;
  card_kind active_card = card_kind::strike;
  resolution_cursor resolution{};
  constexpr bool operator==(const combat_cursor&) const noexcept = default;
};

class combat {
public:
  using pipeline_type = simul::turn_pipeline<combat_cursor>;

  struct snapshot {
    combat_state state{};
    pipeline_type::snapshot pipeline{};
    resolution_work resolution{};
    std::optional<player_intent> pending_intent;
    uint64_t next_presentation_task = 1;
    instance_id next_instance = 1;
  };

  explicit combat(run_mode mode = run_mode::headless) noexcept;

  bool submit(player_intent intent);
  void update(uint64_t engine_tick);
  bool notify_presentation(simul::presentation_task_id task,
                           simul::presentation_event_kind kind) noexcept;
  std::vector<presentation_command> take_presentation_commands();

  const combat_state& state() const noexcept;
  const combat_cursor& cursor() const noexcept;
  const resolution_work& last_resolution() const noexcept;
  bool awaiting_player() const noexcept;
  bool waiting_presentation() const noexcept;
  bool faulted() const noexcept;

  snapshot save() const;
  void load(const snapshot& value);

  // Project policy seams. Their data-driven forms will be ds predicates/scripts over card,
  // effect and target scopes; they deliberately do not belong to simul::turn_pipeline.
  static bool advances_countdown(card_kind card) noexcept;

  // turn_pipeline host contract
  simul::step_control run_step(combat_cursor& cursor, pipeline_type& pipe);
  uint64_t barrier_budget() const noexcept;
  void on_barrier_timeout(const combat_cursor&, const simul::presentation_barrier&) noexcept;

private:
  void begin_card_resolution(combat_cursor& cursor);
  void begin_enemy_resolution(combat_cursor& cursor);
  simul::step_control run_resolution_step(combat_cursor& cursor, pipeline_type& pipe);

  simul::step_control begin_presentation(resolution_cursor& cursor, pipeline_type& pipe,
                                         presentation_subject subject, instance_id instance,
                                         entity_id target, resolution_stage commit_stage);
  simul::step_control publish_result(resolution_cursor& cursor, pipeline_type& pipe,
                                     presentation_subject subject, instance_id instance,
                                     entity_id target, int32_t value, uint32_t outcome,
                                     resolution_stage after_stage);

  damage_outcome resolve_damage(const damage_instance& damage);
  void resolve_attack(const attack_instance& attack);
  void resolve_reaction(const reaction_request& reaction);
  void resolve_response(const damage_instance& response);
  effect_outcome resolve_effect(const effect_request& request);
  std::vector<damage_modifier> collect_resistance_modifiers(
    const combatant_state& target, const damage_instance& damage) const;
  effect_apply_result can_apply_effect(const combatant_state* target,
                                       const effect_request& request) const noexcept;

  combatant_state* find_combatant(entity_id id) noexcept;
  const combatant_state* find_combatant(entity_id id) const noexcept;
  instance_id allocate_instance() noexcept;
  void countdown_pulse();

  pipeline_type pipeline_;
  combat_state state_;
  resolution_work resolution_;
  std::optional<player_intent> pending_intent_;
  std::vector<presentation_command> presentation_outbox_;
  uint64_t next_presentation_task_ = 1;
  instance_id next_instance_ = 1;
  run_mode mode_ = run_mode::headless;
  bool timeout_reported_ = false;
};

} // namespace core
} // namespace cardgame

#endif
