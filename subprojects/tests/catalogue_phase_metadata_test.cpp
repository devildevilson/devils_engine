#include <array>

#include <devils_engine/catalogue/phase.h>
#include <doctest/doctest.h>

namespace catalogue = devils_engine::catalogue;
namespace mt = devils_engine::catalogue::mt;

namespace {

using parallel_strategy = mt::preset::parallel_collect<0>;
using structural_strategy = mt::preset::structural_elect<
  1, mt::conflict::target_not_source>;
struct parallel_domain_id {};
struct structural_domain_id {};
using parallel_domain = mt::domain<parallel_domain_id, parallel_strategy>;
using structural_domain = mt::domain<structural_domain_id, structural_strategy>;

inline constexpr std::array parallel_reads{
  catalogue::phase_resource<"position">(),
  catalogue::phase_resource<"perception">(),
};
inline constexpr std::array parallel_writes{
  catalogue::phase_resource<"velocity">(),
};
inline constexpr std::array parallel_budgets{
  catalogue::fixed_phase_budget<"effects_per_source", "calls">(16),
  catalogue::dynamic_phase_budget<"calls_per_tick", "calls">(),
};
inline constexpr auto parallel_phase = catalogue::make_phase_descriptor<parallel_domain>(
  "actor.local_effects", "tile_frontier", catalogue::phase_write_policy::disjoint_by_key,
  parallel_reads, parallel_writes, parallel_budgets);

inline constexpr std::array structural_reads{
  catalogue::phase_resource<"position">(),
};
inline constexpr std::array structural_writes{
  catalogue::phase_resource<"world.structure">(),
  catalogue::phase_resource<"actor_eating">(),
};
inline constexpr std::array structural_budgets{
  catalogue::fixed_phase_budget<"effects_per_source", "calls">(16),
};
inline constexpr auto structural_phase = catalogue::make_phase_descriptor<structural_domain>(
  "actor.eat_effects", "tile_frontier", catalogue::phase_write_policy::structural,
  structural_reads, structural_writes, structural_budgets);

} // namespace

TEST_CASE("catalogue phase metadata derives execution policy without touching executors") {
  static_assert(parallel_phase.execution.arbitration == mt::arbitration::collect);
  static_assert(parallel_phase.execution.commit == catalogue::phase_commit::parallel_groups);
  static_assert(parallel_phase.execution.conflict == catalogue::phase_conflict::none);
  static_assert(structural_phase.execution.arbitration == mt::arbitration::elect);
  static_assert(structural_phase.execution.commit == catalogue::phase_commit::serial_structural);
  static_assert(structural_phase.execution.conflict == catalogue::phase_conflict::target_not_source);

  CHECK(parallel_phase.name == "actor.local_effects");
  CHECK(parallel_phase.owner == "tile_frontier");
  REQUIRE(parallel_phase.reads.size() == 2);
  CHECK(parallel_phase.reads[0].name == "position");
  REQUIRE(parallel_phase.budgets.size() == 2);
  CHECK(parallel_phase.budgets[0].fixed_limit == 16);
  CHECK_FALSE(parallel_phase.budgets[0].dynamic);
  CHECK(parallel_phase.budgets[1].dynamic);
}

TEST_CASE("catalogue phase registry is explicit sorted and validates contracts") {
  catalogue::phase_registry registry;
  registry.add(structural_phase);
  registry.add(parallel_phase);
  REQUIRE(registry.descriptors().size() == 2);
  CHECK(registry.descriptors()[0].id < registry.descriptors()[1].id);
  REQUIRE(registry.find(parallel_phase.id) != nullptr);
  CHECK(registry.find(parallel_phase.id)->name == parallel_phase.name);
  CHECK_THROWS(registry.add(parallel_phase));

  auto invalid = parallel_phase;
  invalid.write_policy = catalogue::phase_write_policy::read_only;
  CHECK_THROWS(catalogue::validate_phase_descriptor(invalid));

  invalid = structural_phase;
  invalid.execution.commit = catalogue::phase_commit::serial;
  CHECK_THROWS(catalogue::validate_phase_descriptor(invalid));

  registry.clear();
  CHECK(registry.descriptors().empty());
}
