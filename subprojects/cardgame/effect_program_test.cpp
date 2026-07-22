#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "cardgame/effect_program.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "cardgame_effect_program_test: %s\n", message);
  std::exit(EXIT_FAILURE);
}

void check(const bool value, const char* message) {
  if (!value) fail(message);
}

} // namespace

int main() {
  // This is a project-defined stable order, deliberately not numeric entity-id order.
  const std::vector<cg::entity_id> candidates{30, 10, 40, 20};
  const std::vector queries{
    cg::target_query{{cg::targeter_kind::random_target, 0}, 0, candidates},
    cg::target_query{{cg::targeter_kind::random_target, 77}, 0, candidates},
    cg::target_query{{cg::targeter_kind::random_target, 77}, 0, candidates},
    cg::target_query{{cg::targeter_kind::all_targets, 0}, 0, candidates},
    cg::target_query{{cg::targeter_kind::target, 0}, 30, candidates},
  };

  const auto first = cg::materialize_target_sets(queries, 1234);
  const auto second = cg::materialize_target_sets(queries, 1234);
  check(first == second, "same execution entropy changed target snapshots");
  check(first[1] == first[2], "explicit target binding did not reuse one random choice");
  check(first[3].targets == candidates, "AoE did not preserve the stable eligible snapshot");
  check(first[4].targets == std::vector<cg::entity_id>{30},
        "fixed targeter did not preserve the selected target");

  bool independent_can_differ = false;
  for (uint64_t entropy = 1; entropy < 256 && !independent_can_differ; ++entropy) {
    const auto snapshots = cg::materialize_target_sets(queries, entropy);
    independent_can_differ = snapshots[0] != snapshots[1];
  }
  check(independent_can_differ,
        "independent random targeters were accidentally coupled to a shared selection");

  auto mismatched = queries;
  mismatched[2].selector.kind = cg::targeter_kind::all_targets;
  bool mismatch_failed = false;
  try {
    (void)cg::materialize_target_sets(mismatched, 1);
  } catch (const std::invalid_argument&) {
    mismatch_failed = true;
  }
  check(mismatch_failed, "mismatched queries silently reused one target binding");

  auto duplicates = queries;
  duplicates[0].eligible_targets = {20, 10, 20};
  bool duplicates_failed = false;
  try {
    (void)cg::materialize_target_sets(duplicates, 1);
  } catch (const std::invalid_argument&) {
    duplicates_failed = true;
  }
  check(duplicates_failed, "duplicate candidates were accepted");

  std::puts("cardgame effect program targeting: independent/shared/random/AoE snapshots OK");
  return EXIT_SUCCESS;
}
