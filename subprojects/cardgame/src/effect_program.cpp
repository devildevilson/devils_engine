#include "cardgame/effect_program.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace cardgame {
namespace core {
namespace {

uint64_t mix64(uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

void validate_query(const target_query& query) {
  std::vector<entity_id> sorted = query.eligible_targets;
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    throw std::invalid_argument("cardgame target candidates contain duplicates");
  }
  if (std::find(query.eligible_targets.begin(), query.eligible_targets.end(), invalid_entity) !=
      query.eligible_targets.end()) {
    throw std::invalid_argument("cardgame target candidates contain invalid entity");
  }
}

target_snapshot materialize_one(const target_query& query, const uint64_t selector_salt) {
  target_snapshot snapshot;
  switch (query.selector.kind) {
    case targeter_kind::target:
      if (query.fixed_target != invalid_entity &&
          std::find(query.eligible_targets.begin(), query.eligible_targets.end(),
                    query.fixed_target) != query.eligible_targets.end()) {
        snapshot.targets.push_back(query.fixed_target);
      }
      break;
    case targeter_kind::random_target:
      if (!query.eligible_targets.empty()) {
        const size_t index = static_cast<size_t>(
          mix64(selector_salt) % static_cast<uint64_t>(query.eligible_targets.size()));
        snapshot.targets.push_back(query.eligible_targets[index]);
      }
      break;
    case targeter_kind::all_targets:
      snapshot.targets = query.eligible_targets;
      break;
  }
  return snapshot;
}

} // namespace

std::vector<target_snapshot> materialize_target_sets(
  const std::span<const target_query> queries,
  const uint64_t deterministic_entropy) {
  struct bound_selection {
    target_query query;
    target_snapshot snapshot;
  };

  std::unordered_map<target_binding_id, bound_selection> bindings;
  std::vector<target_snapshot> result;
  result.reserve(queries.size());

  for (size_t i = 0; i < queries.size(); ++i) {
    const target_query& query = queries[i];
    validate_query(query);

    if (query.selector.binding == independent_target_binding) {
      result.push_back(materialize_one(
        query, deterministic_entropy ^ mix64(static_cast<uint64_t>(i) + 1u)));
      continue;
    }

    const auto found = bindings.find(query.selector.binding);
    if (found != bindings.end()) {
      if (found->second.query != query) {
        throw std::invalid_argument(
          "cardgame target binding reused by non-identical target queries");
      }
      result.push_back(found->second.snapshot);
      continue;
    }

    target_snapshot snapshot = materialize_one(
      query, deterministic_entropy ^ mix64(query.selector.binding));
    bindings.emplace(query.selector.binding, bound_selection{query, snapshot});
    result.push_back(std::move(snapshot));
  }

  return result;
}

} // namespace core
} // namespace cardgame
