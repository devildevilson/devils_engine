#include "cardgame/follow_up.h"

#include <algorithm>
#include <stdexcept>

namespace cardgame {
namespace core {
namespace {

constexpr uint64_t player_side_domain = 0x706c617965725f66ull;
constexpr uint64_t enemy_side_domain = 0x656e656d795f666full;

uint64_t mix64(uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

uint64_t mix_field(const uint64_t state, const uint64_t value) noexcept {
  return mix64(state ^ mix64(value));
}

uint64_t side_domain(const combat_side side) {
  switch (side) {
    case combat_side::player:
      return player_side_domain;
    case combat_side::enemy:
      return enemy_side_domain;
  }
  throw std::invalid_argument("cardgame follow-up side is invalid");
}

} // namespace

std::vector<entity_id> materialize_follow_up_order(
  const std::span<const entity_id> eligible_participants,
  const follow_up_order_context context) {
  std::vector<entity_id> validated{
    eligible_participants.begin(), eligible_participants.end()};
  std::sort(validated.begin(), validated.end());
  if (!validated.empty() && validated.front() == invalid_entity) {
    throw std::invalid_argument("cardgame follow-up participants contain invalid entity");
  }
  if (std::adjacent_find(validated.begin(), validated.end()) != validated.end()) {
    throw std::invalid_argument("cardgame follow-up participants contain duplicates");
  }

  struct ordered_participant {
    uint64_t key = 0;
    entity_id id = invalid_entity;
  };

  uint64_t execution_key = mix_field(context.combat_seed, context.action_token);
  execution_key = mix_field(execution_key, side_domain(context.side));

  std::vector<ordered_participant> ordered;
  ordered.reserve(validated.size());
  for (const entity_id id : validated) {
    ordered.push_back(ordered_participant{mix_field(execution_key, id), id});
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.key != rhs.key) return lhs.key < rhs.key;
    return lhs.id < rhs.id;
  });

  std::vector<entity_id> result;
  result.reserve(ordered.size());
  for (const ordered_participant& participant : ordered) {
    result.push_back(participant.id);
  }
  return result;
}

} // namespace core
} // namespace cardgame
