#ifndef DEVILS_ENGINE_CARDGAME_FOLLOW_UP_H
#define DEVILS_ENGINE_CARDGAME_FOLLOW_UP_H

#include <cstdint>
#include <span>
#include <vector>

#include "cardgame/effect_program.h"

namespace cardgame {
namespace core {

enum class combat_side : uint8_t {
  player,
  enemy
};

// Categories describe work that actually occurred during one execution. They are intentionally
// orthogonal to signs: an attack or healing instance remains that category even when its authored
// amount is negative. `stat_change` is the common subscription for all resolved stat changes.
enum class execution_category : uint8_t {
  attack,
  damage,
  healing,
  shield,
  attribute_change,
  status,
  stat_change,
  reaction,
  elemental_reaction,
  retaliation,
  count
};

using execution_category_mask = uint64_t;
static_assert(static_cast<uint8_t>(execution_category::count) <= 64);

constexpr execution_category_mask category_bit(const execution_category category) noexcept {
  return execution_category_mask{1} << static_cast<uint8_t>(category);
}

constexpr bool has_category(const execution_category_mask categories,
                            const execution_category category) noexcept {
  return (categories & category_bit(category)) != 0;
}

constexpr void add_category(execution_category_mask& categories,
                            const execution_category category) noexcept {
  categories |= category_bit(category);
}

// One enabler gates one script invocation. The script receives the complete execution report and
// decides how many follow-up effect programs to emit. An empty subscription never matches.
struct follow_up_enabler {
  execution_category_mask any_of = 0;
  execution_category_mask all_of = 0;

  constexpr bool matches(const execution_category_mask categories) const noexcept {
    if ((any_of | all_of) == 0) return false;
    const bool any_matches = any_of == 0 || (categories & any_of) != 0;
    const bool all_match = (categories & all_of) == all_of;
    return any_matches && all_match;
  }

  constexpr bool operator==(const follow_up_enabler&) const noexcept = default;
};

struct follow_up_order_context {
  uint64_t combat_seed = 0;
  uint64_t action_token = 0;
  combat_side side = combat_side::player;
  constexpr bool operator==(const follow_up_order_context&) const noexcept = default;
};

// Freezes one party pass. Ordering is independent of the input container and of which other
// participants are eligible: every actor receives its own deterministic pseudo-random key from
// (combat seed, action token, side domain, stable entity id), then ties use entity id. Callers
// recheck liveness immediately before invoking each actor. Rules inside one actor remain in normal
// project definition order and are deliberately outside this materializer.
std::vector<entity_id> materialize_follow_up_order(
  std::span<const entity_id> eligible_participants,
  follow_up_order_context context);

} // namespace core
} // namespace cardgame

#endif
