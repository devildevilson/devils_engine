#ifndef DEVILS_ENGINE_CARDGAME_COMBAT_H
#define DEVILS_ENGINE_CARDGAME_COMBAT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <devils_engine/resolve/resolve.h>
#include <devils_engine/simul/turn_pipeline.h>
#include <devils_engine/utils/string_id.h>

#include "cardgame/follow_up.h"

namespace cardgame {
namespace core {

namespace resolve = devils_engine::resolve;
namespace simul = devils_engine::simul;

using instance_id = resolve::instance_id;

class combat_effect_script_provider;

inline constexpr entity_id player_entity = 1;
inline constexpr entity_id enemy_entity = 2;

enum class run_mode : uint8_t {
  headless,
  animated
};

enum class card_kind : uint8_t {
  strike,         // physical damage 3, advances countdown
  quick_strike,   // physical damage 1, does not advance countdown
  fire_strike,    // fire damage 4 + one burning effect request
  double_strike,  // one authored effect emits two physical attack instances
  combo_strike,   // two sequential beats with physical damage 2 each
  inverse_strike, // physical attack -5; semantic kind remains attack/damage
  mend,           // self healing +6
  cursed_mend,    // self healing -7; semantic kind remains healing
  cripple,        // agility damage 4
  scripted_strike // resource-backed DS effect: physical damage 3
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

using damage_channel = resolve::damage_channel;

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
  instance_id root = 0;
  instance_id parent_execution = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  element type = element::none;
  int32_t base_damage = 0;
  bool applies_element = false;
  constexpr bool operator==(const attack_instance&) const noexcept = default;
};

enum class damage_destination : uint8_t {
  unrouted,
  shield,
  health
};

// `unrouted` is the magnitude/modifier root produced by an attack. It never mutates a stat or
// becomes an outcome. Routing emits bounded shield/health leaves carrying the already-modified
// amount, so resistances cannot be applied twice.
struct damage_payload {
  element kind = element::none;
  int32_t amount = 0;
  damage_channel channel = damage_channel::primary;
  uint64_t tags = 0;
  damage_destination destination = damage_destination::unrouted;
  constexpr bool operator==(const damage_payload&) const noexcept = default;
};

using damage_instance = resolve::work_item<damage_payload>;
static_assert(resolve::work_record<damage_instance>);

struct stat_change_route {
  int32_t requested = 0;
  int32_t modified = 0;
  int32_t before = 0;
  int32_t proposed_after = 0;
  int32_t committed_after = 0;
  int32_t clamped = 0;
  constexpr bool operator==(const stat_change_route&) const noexcept = default;
};

struct damage_preparation {
  damage_instance damage{};
  std::vector<damage_modifier> modifiers;
  resolve::damage_route<int32_t> route{};
  bool target_valid = false;
  bool operator==(const damage_preparation&) const noexcept = default;
};

struct damage_outcome {
  damage_instance damage{};
  stat_change_route route{};
  bool target_valid = false;
  bool committed = false;
  bool operator==(const damage_outcome&) const noexcept = default;
};

struct healing_effect {
  entity_id source = invalid_entity;
  int32_t amount = 0;
  constexpr bool operator==(const healing_effect&) const noexcept = default;
};

struct healing_instance {
  instance_id id = 0;
  instance_id parent_execution = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  int32_t amount = 0;
  constexpr bool operator==(const healing_instance&) const noexcept = default;
};

struct healing_outcome {
  healing_instance healing{};
  stat_change_route route{};
  int32_t effectiveness_basis_points = 10000;
  bool target_valid = false;
  bool committed = false;
  constexpr bool operator==(const healing_outcome&) const noexcept = default;
};

enum class attribute_kind : uint8_t {
  agility,
  count
};

inline constexpr size_t attribute_count = static_cast<size_t>(attribute_kind::count);

struct attribute_damage_effect {
  entity_id source = invalid_entity;
  attribute_kind attribute = attribute_kind::agility;
  int32_t amount = 0;
  constexpr bool operator==(const attribute_damage_effect&) const noexcept = default;
};

struct attribute_damage_instance {
  instance_id id = 0;
  instance_id parent_execution = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  attribute_kind attribute = attribute_kind::agility;
  int32_t amount = 0;
  constexpr bool operator==(const attribute_damage_instance&) const noexcept = default;
};

struct attribute_damage_outcome {
  attribute_damage_instance damage{};
  stat_change_route route{};
  int32_t resistance_basis_points = 0;
  bool target_valid = false;
  bool committed = false;
  constexpr bool operator==(const attribute_damage_outcome&) const noexcept = default;
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

enum class outcome_store_kind : uint8_t {
  damage,
  healing,
  attribute_damage,
  effect
};

struct outcome_ref {
  outcome_store_kind kind = outcome_store_kind::damage;
  size_t index = 0;
  constexpr bool operator==(const outcome_ref&) const noexcept = default;
};

struct death_check {
  outcome_store_kind kind = outcome_store_kind::damage;
  size_t outcome_index = 0;
  entity_id target = invalid_entity;
  bool dead = false;
  constexpr bool operator==(const death_check&) const noexcept = default;
};

struct combatant_state {
  entity_id id = invalid_entity;
  int32_t hp = 0;
  int32_t max_hp = 0;
  int32_t shield = 0;
  int32_t agility = 0;
  // 10000 means ordinary healing, 5000 halves both positive and negative healing instances.
  int32_t healing_effectiveness_basis_points = 10000;
  // Positive values reduce damage; -2500 means 25% vulnerability, 10000 means immunity.
  std::array<int32_t, element_count> resistance_basis_points{};
  std::array<int32_t, attribute_count> attribute_resistance_basis_points{};
  element elemental_mark = element::none;
  uint64_t effect_immunity_mask = 0;
  std::vector<effect_state> effects;
  bool operator==(const combatant_state&) const noexcept = default;
};

struct retaliation_request {
  resolve::work_header trigger{};
  resolve::rule_id rule = 0;
  entity_id source = invalid_entity;
  entity_id target = invalid_entity;
  int32_t amount = 0;
  uint16_t local_ordinal = 0;
  constexpr bool operator==(const retaliation_request&) const noexcept = default;
};

struct attack_effect {
  entity_id source = invalid_entity;
  element type = element::none;
  int32_t base_damage = 0;
  uint16_t hit_count = 1;
  bool applies_element = false;
  constexpr bool operator==(const attack_effect&) const noexcept = default;
};

struct status_effect {
  entity_id source = invalid_entity;
  effect_kind kind = effect_kind::burning;
  int32_t stacks = 1;
  int32_t remaining_pulses = 0;
  constexpr bool operator==(const status_effect&) const noexcept = default;
};

// Pointer-free authored reference to an externally owned compiled DS resource. The provider is
// runtime infrastructure and is deliberately absent from resolution_work snapshots.
struct script_effect {
  entity_id source = invalid_entity;
  devils_engine::utils::id script = devils_engine::utils::invalid_id;
  constexpr bool operator==(const script_effect&) const noexcept = default;
};

enum class authored_effect_store_kind : uint8_t {
  attack,
  healing,
  attribute_damage,
  effect,
  script
};

enum class effect_store_kind : uint8_t {
  attack,
  healing,
  attribute_damage,
  effect
};

// Erased pointer-free ref into an authored typed recipe store.
struct effect_ref {
  authored_effect_store_kind kind = authored_effect_store_kind::attack;
  size_t index = 0;
  constexpr bool operator==(const effect_ref&) const noexcept = default;
};

// The same envelope shape, but for concrete instances emitted by a script. Keeping the type
// distinct prevents authored recipes and runtime work from being mixed accidentally.
struct effect_instance_ref {
  effect_store_kind kind = effect_store_kind::attack;
  size_t index = 0;
  constexpr bool operator==(const effect_instance_ref&) const noexcept = default;
};

// Pointer-free authored program. The ref selects a typed recipe; the targeter is orthogonal and is
// materialized for every effect in a beat before any cue or gameplay call starts.
struct authored_effect {
  effect_ref body{};
  targeter targets{};
  enum class target_domain : uint8_t {
    opponent,
    self
  } domain = target_domain::opponent;
  constexpr bool operator==(const authored_effect&) const noexcept = default;
};

struct effect_beat {
  std::vector<authored_effect> effects;
  bool operator==(const effect_beat&) const noexcept = default;
};

struct effect_program {
  std::vector<effect_beat> beats;
  bool operator==(const effect_program&) const noexcept = default;
};

struct authored_effect_report {
  instance_id id = 0;
  size_t beat_index = 0;
  size_t effect_index = 0;
  effect_ref body{};
  target_snapshot target_set{};
  size_t plan_begin = 0;
  size_t plan_count = 0;
  size_t damage_outcome_begin = 0;
  size_t damage_outcome_count = 0;
  size_t effect_outcome_begin = 0;
  size_t effect_outcome_count = 0;
  size_t healing_outcome_begin = 0;
  size_t healing_outcome_count = 0;
  size_t attribute_outcome_begin = 0;
  size_t attribute_outcome_count = 0;
  size_t outcome_begin = 0;
  size_t outcome_count = 0;
  bool invoked = false;
  constexpr bool operator==(const authored_effect_report&) const noexcept = default;
};

struct execution_report {
  instance_id execution = 0;
  entity_id actor = invalid_entity;
  entity_id selected_target = invalid_entity;
  bool executed = false;
  std::vector<authored_effect_report> effects;
  execution_category_mask categories = 0;
  bool operator==(const execution_report&) const noexcept = default;
};

struct resolution_work {
  effect_program program;
  std::vector<attack_effect> attack_effects;
  std::vector<healing_effect> healing_effects;
  std::vector<attribute_damage_effect> attribute_damage_effects;
  std::vector<status_effect> status_effects;
  std::vector<script_effect> script_effects;
  execution_report report;

  // Typed instances emitted by authored-effect scripts. `plan` preserves their semantic order.
  std::vector<attack_instance> attacks;
  std::vector<healing_instance> healings;
  std::vector<attribute_damage_instance> attribute_damages;
  std::vector<effect_request> effects;
  std::vector<effect_instance_ref> plan;
  resolve::frontier_state<damage_instance> damage_frontier;
  std::vector<damage_instance> next_frontier;
  std::vector<retaliation_request> retaliation_requests;
  std::vector<damage_instance> responses;
  std::vector<damage_preparation> damage_preparations;
  std::vector<damage_outcome> damage_trace;
  std::vector<healing_outcome> healing_trace;
  std::vector<attribute_damage_outcome> attribute_damage_trace;
  std::vector<effect_outcome> effect_trace;
  std::vector<outcome_ref> outcomes;
  std::vector<death_check> death_trace;
  bool operator==(const resolution_work&) const noexcept = default;
};

enum class resolution_stage : uint8_t {
  idle,
  select_beat,
  beat_cue,
  select_effect,
  select_item,
  attack_commit,
  attack_after,
  select_reaction,
  reaction_commit,
  reaction_after,
  select_response,
  response_commit,
  response_after,
  post_attack,
  healing_commit,
  healing_after,
  attribute_damage_commit,
  attribute_damage_after,
  effect_commit,
  effect_after,
  beat_results,
  beat_after,
  done
};

struct resolution_cursor {
  resolution_stage stage = resolution_stage::idle;
  size_t beat_index = 0;
  size_t beat_report_begin = 0;
  size_t authored_effect_index = 0;
  size_t item_index = 0;
  size_t item_end = 0;
  size_t attack_index = 0;
  size_t reaction_index = 0;
  size_t response_index = 0;
  size_t healing_index = 0;
  size_t attribute_damage_index = 0;
  size_t effect_index = 0;
  constexpr bool operator==(const resolution_cursor&) const noexcept = default;
};

enum class combat_phase : uint8_t {
  turn_begin,
  awaiting_action,
  action_cycle,
  end_turn,
  battle_over
};

// Project-owned groups of the settled combat pipeline. `action_countdown` is the explicit boundary
// between numbered groups 7 and 8 rather than a fourteenth gameplay group.
enum class combat_group : uint8_t {
  turn_begin,
  awaiting_action,
  action_begin,
  card_effects,
  card_player_party_follow_ups,
  card_enemy_party_follow_ups,
  player_actor_state_tick,
  action_countdown,
  enemy_execution,
  enemy_action_enemy_party_follow_ups,
  enemy_action_player_party_follow_ups,
  enemy_actor_state_tick,
  action_end,
  turn_end
};

enum class combat_step : uint8_t {
  enter,
  wait_input,
  resolve_execution,
  visit_party,
  forced_pulse,
  turn_done
};

enum class combat_trace_kind : uint8_t {
  group_enter,
  card_stolen,
  follow_up_rule,
  actor_state_tick,
  countdown_pulse
};

// Authoritative diagnostic/event log for the headless kernel. It also makes group order observable
// without deriving gameplay from presentation timing. A bounded production log policy comes later.
struct combat_trace_event {
  combat_trace_kind kind = combat_trace_kind::group_enter;
  combat_group group = combat_group::turn_begin;
  uint64_t action_token = 0;
  instance_id execution = 0;
  entity_id source_actor = invalid_entity;
  entity_id actor = invalid_entity;
  constexpr bool operator==(const combat_trace_event&) const noexcept = default;
};

struct party_follow_up_cursor {
  std::vector<entity_id> order;
  size_t actor_index = 0;
  bool operator==(const party_follow_up_cursor&) const noexcept = default;
};

struct action_cycle_cursor {
  uint64_t token = 0;
  entity_id player_actor = player_entity;
  execution_report trigger_report{};
  party_follow_up_cursor party{};
  bool card_stolen = false;
  bool forced_enemy_cycle = false;
  bool operator==(const action_cycle_cursor&) const noexcept = default;
};

enum class presentation_subject : uint8_t {
  player_attack,
  enemy_attack,
  elemental_reaction,
  returned_damage,
  shield_damage,
  healing,
  attribute_damage,
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
  std::vector<entity_id> targets;
  struct result_value {
    presentation_subject subject = presentation_subject::player_attack;
    instance_id instance = 0;
    entity_id target = invalid_entity;
    int32_t value = 0;
    uint32_t outcome = 0;
    constexpr bool operator==(const result_value&) const noexcept = default;
  };
  std::vector<result_value> results;
  bool operator==(const presentation_command&) const noexcept = default;
};

// Entire authoritative state of the first combat slice. Presentation state is absent.
struct combat_state {
  combatant_state player{
    .id = player_entity, .hp = 30, .max_hp = 30, .agility = 10, .effects = {}};
  combatant_state enemy{
    .id = enemy_entity, .hp = 100, .max_hp = 100, .agility = 10, .effects = {}};
  int32_t enemy_countdown = 0;
  uint64_t combat_seed = 0x4341524447414d45ull;
  uint64_t turn_index = 0;
  uint64_t player_action_index = 0;
  uint64_t action_cycle_index = 0;
  uint64_t countdown_pulse_index = 0;
  uint64_t stolen_card_count = 0;
  bool enemy_intent_active = false;
  bool intercept_next_card = false;
  std::vector<combat_trace_event> trace;
  bool operator==(const combat_state&) const noexcept = default;
};

// Serializable project cursor. Presentation task ids are deliberately absent.
struct combat_cursor {
  combat_phase phase = combat_phase::turn_begin;
  combat_group group = combat_group::turn_begin;
  combat_step step = combat_step::enter;
  card_kind active_card = card_kind::strike;
  action_cycle_cursor action{};
  resolution_cursor resolution{};
  bool operator==(const combat_cursor&) const noexcept = default;
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
    instance_id next_root = 1;
    instance_id next_execution = 1;
    instance_id next_effect_call = 1;
  };

  explicit combat(run_mode mode = run_mode::headless,
                  const combat_effect_script_provider* scripts = nullptr) noexcept;

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
  void begin_attack_frontier(const attack_instance& attack);
  void materialize_beat(resolution_cursor& cursor);
  void invoke_authored_effect(authored_effect_report& call);
  void finalize_authored_effect(authored_effect_report& call);
  std::vector<entity_id> eligible_targets(
    entity_id source, authored_effect::target_domain domain) const;
  presentation_subject subject_for(const authored_effect_report& call) const;
  std::vector<presentation_command::result_value> presentation_results(
    const authored_effect_report& call) const;
  void prepare_retaliations();
  void run_retaliation_rules(const damage_outcome& trigger);
  bool advance_to_retaliations();
  simul::step_control run_resolution_step(combat_cursor& cursor, pipeline_type& pipe);

  damage_preparation prepare_damage(const damage_instance& damage) const;
  damage_outcome commit_damage(const damage_instance& damage);
  void resolve_damage_work(const damage_instance& damage);
  healing_outcome resolve_healing(const healing_instance& healing);
  attribute_damage_outcome resolve_attribute_damage(
    const attribute_damage_instance& damage);
  effect_outcome resolve_effect(const effect_request& request);
  void record_death_check(outcome_store_kind kind, size_t outcome_index, entity_id target);
  bool run_party_follow_ups(combat_cursor& cursor, combat_side side);
  std::vector<entity_id> party_members(combat_side side) const;
  void enter_group(combat_cursor& cursor, combat_group group);
  void trace(combat_trace_kind kind, combat_group group, const combat_cursor& cursor,
             entity_id actor = invalid_entity, instance_id execution = 0);
  void actor_state_tick(combat_cursor& cursor, entity_id actor);
  std::vector<damage_modifier> collect_resistance_modifiers(
    const combatant_state& target, const damage_instance& damage) const;
  effect_apply_result can_apply_effect(const combatant_state* target,
                                       const effect_request& request) const noexcept;

  combatant_state* find_combatant(entity_id id) noexcept;
  const combatant_state* find_combatant(entity_id id) const noexcept;
  instance_id allocate_instance();
  instance_id allocate_root();
  instance_id allocate_execution();
  instance_id allocate_effect_call();
  void countdown_pulse();

  pipeline_type pipeline_;
  combat_state state_;
  resolution_work resolution_;
  std::optional<player_intent> pending_intent_;
  std::vector<presentation_command> presentation_outbox_;
  // Presentation task ids are derived and intentionally absent from snapshots.
  std::vector<simul::presentation_task_id> active_beat_tasks_;
  simul::presentation_task_id active_response_task_ = 0;
  uint64_t next_presentation_task_ = 1;
  instance_id next_instance_ = 1;
  instance_id next_root_ = 1;
  instance_id next_execution_ = 1;
  instance_id next_effect_call_ = 1;
  run_mode mode_ = run_mode::headless;
  const combat_effect_script_provider* scripts_ = nullptr;
  bool timeout_reported_ = false;
};

} // namespace core
} // namespace cardgame

#endif
