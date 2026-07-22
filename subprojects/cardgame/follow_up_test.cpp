#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "cardgame/follow_up.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "cardgame_follow_up_test: %s\n", message);
  std::exit(EXIT_FAILURE);
}

void check(const bool value, const char* message) {
  if (!value) fail(message);
}

std::vector<cg::entity_id> retain(const std::vector<cg::entity_id>& order,
                                  const std::vector<cg::entity_id>& subset) {
  std::vector<cg::entity_id> result;
  for (const cg::entity_id id : order) {
    if (std::find(subset.begin(), subset.end(), id) != subset.end()) {
      result.push_back(id);
    }
  }
  return result;
}

} // namespace

int main() {
  const std::vector<cg::entity_id> participants{40, 10, 30, 20};
  const cg::follow_up_order_context context{1234, 42, cg::combat_side::player};
  const auto first = cg::materialize_follow_up_order(participants, context);

  auto shuffled = participants;
  std::reverse(shuffled.begin(), shuffled.end());
  check(cg::materialize_follow_up_order(shuffled, context) == first,
        "party order depends on source container order");
  check(cg::materialize_follow_up_order(participants, context) == first,
        "same action context changed party order");

  const std::vector<cg::entity_id> subset{10, 30, 40};
  check(cg::materialize_follow_up_order(subset, context) == retain(first, subset),
        "eligibility changes reordered the remaining party members");

  bool action_can_change_order = false;
  bool side_can_change_order = false;
  for (uint64_t token = 43; token < 256; ++token) {
    const cg::follow_up_order_context next{1234, token, cg::combat_side::player};
    action_can_change_order |= cg::materialize_follow_up_order(participants, next) != first;
    const cg::follow_up_order_context enemy{1234, token, cg::combat_side::enemy};
    side_can_change_order |= cg::materialize_follow_up_order(participants, enemy) !=
                             cg::materialize_follow_up_order(participants, next);
  }
  check(action_can_change_order, "action token does not affect party order");
  check(side_can_change_order, "player and enemy party domains are coupled");

  bool duplicate_failed = false;
  try {
    const std::vector<cg::entity_id> duplicate{10, 20, 10};
    (void)cg::materialize_follow_up_order(duplicate, context);
  } catch (const std::invalid_argument&) {
    duplicate_failed = true;
  }
  check(duplicate_failed, "duplicate follow-up participant was accepted");

  bool invalid_failed = false;
  try {
    const std::vector<cg::entity_id> invalid{10, cg::invalid_entity, 20};
    (void)cg::materialize_follow_up_order(invalid, context);
  } catch (const std::invalid_argument&) {
    invalid_failed = true;
  }
  check(invalid_failed, "invalid follow-up participant was accepted");

  constexpr cg::execution_category_mask attack =
    cg::category_bit(cg::execution_category::attack);
  constexpr cg::execution_category_mask reaction =
    cg::category_bit(cg::execution_category::reaction);
  constexpr cg::execution_category_mask damage =
    cg::category_bit(cg::execution_category::damage);
  static_assert(cg::follow_up_enabler{attack, 0}.matches(attack));
  static_assert(cg::follow_up_enabler{attack | reaction, damage}.matches(attack | damage));
  static_assert(!cg::follow_up_enabler{attack | reaction, damage}.matches(attack));
  static_assert(!cg::follow_up_enabler{}.matches(attack | reaction | damage));

  std::puts("cardgame follow-up order: deterministic party materialization and enablers OK");
  return EXIT_SUCCESS;
}
