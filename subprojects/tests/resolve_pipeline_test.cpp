#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <devils_engine/resolve/resolve.h>
#include <devils_engine/thread/atomic_pool.h>
#include <doctest/doctest.h>

namespace resolve = devils_engine::resolve;
namespace thread = devils_engine::thread;

namespace {

struct integer_delta {
  int32_t value = 0;
  constexpr bool operator==(const integer_delta&) const noexcept = default;
};

using test_item = resolve::work_item<integer_delta>;

test_item make_root(const resolve::instance_id root,
                    const resolve::entity_key target,
                    const int32_t value) {
  test_item item;
  item.header.root = root;
  item.header.source = 77;
  item.header.target = target;
  item.header.local_ordinal = static_cast<uint16_t>(root);
  item.payload.value = value;
  return item;
}

std::vector<test_item> seal_in_parallel(std::vector<test_item> input,
                                        const size_t worker_count) {
  resolve::journal<test_item> journal;
  journal.begin_record(input.size());

  thread::atomic_pool pool(worker_count);
  pool.distribute1(input.size(), [&journal, &input](const size_t start, const size_t count) {
    for (size_t i = start; i < start + count; ++i) {
      CHECK(journal.try_record(input[i]));
    }
  });
  pool.compute();
  pool.wait();

  resolve::instance_id next_instance = 1000;
  journal.seal(next_instance);
  return {journal.records().begin(), journal.records().end()};
}

} // namespace

TEST_CASE("resolve journal restores semantic order after MT recording") {
  std::vector<test_item> input{
    make_root(4, 2, 5),
    make_root(1, 1, 1),
    make_root(5, 1, 3),
    make_root(2, 2, 4),
    make_root(3, 1, 2)};
  auto reversed = input;
  std::reverse(reversed.begin(), reversed.end());

  const auto one_worker = seal_in_parallel(input, 1);
  const auto four_workers = seal_in_parallel(reversed, 4);
  REQUIRE(one_worker == four_workers);
  REQUIRE(one_worker.size() == 5);
  for (size_t i = 0; i < one_worker.size(); ++i) {
    CHECK(one_worker[i].header.root == i + 1);
    CHECK(one_worker[i].header.id == 1000 + i);
  }
}

TEST_CASE("resolve target groups are serial per target and parallel between targets") {
  std::vector<test_item> input{
    make_root(4, 2, 5),
    make_root(1, 1, 1),
    make_root(5, 1, 3),
    make_root(2, 2, 4),
    make_root(3, 1, 2)};
  resolve::journal<test_item> journal;
  journal.begin_record(input.size());
  for (const auto& item : input)
    REQUIRE(journal.try_record(item));
  resolve::instance_id next_instance = 1;
  journal.seal(next_instance);

  resolve::target_groups<test_item> groups;
  groups.build(journal.records());
  REQUIRE(groups.group_count() == 2);

  std::array<int32_t, 3> reference{};
  resolve::run_target_groups_serial(
    groups, journal.records(), [&reference](const resolve::entity_key target, const test_item& item) {
      reference[target] = reference[target] * 10 + item.payload.value;
    });

  std::array<int32_t, 3> parallel{};
  thread::atomic_pool pool(4);
  resolve::run_target_groups(
    pool, groups, journal.records(), [&parallel](const resolve::entity_key target, const test_item& item) {
      parallel[target] = parallel[target] * 10 + item.payload.value;
    });

  CHECK(reference == parallel);
  CHECK(parallel[1] == 123);
  CHECK(parallel[2] == 45);
}

TEST_CASE("resolve journal overflow is a deterministic fault") {
  resolve::journal<test_item> journal;
  journal.begin_record(1);
  CHECK(journal.try_record(make_root(1, 1, 1)));
  CHECK_FALSE(journal.try_record(make_root(2, 1, 2)));
  CHECK(journal.overflowed());

  resolve::instance_id next_instance = 10;
  CHECK_THROWS_AS(journal.seal(next_instance), std::length_error);
  CHECK(next_instance == 10);
}

TEST_CASE("resolve frontier progression is host-paced and budgeted") {
  std::array roots{make_root(2, 20, 2), make_root(1, 10, 1)};
  resolve::frontier_state<test_item> state;
  REQUIRE(resolve::begin(state, std::span<const test_item>{roots}, {}, 50));
  REQUIRE(state.active());
  REQUIRE(state.current.size() == 2);
  CHECK(state.current[0].header.root == 1);
  CHECK(state.current[0].header.id == 50);
  CHECK(state.current[1].header.root == 2);
  CHECK(state.current[1].header.id == 51);

  std::array<test_item, 2> children;
  children[0].header = resolve::make_child_header(
    state.current[1].header, resolve::cause_kind::reaction, 0, 1, 20, 30);
  children[0].payload.value = 4;
  children[1].header = resolve::make_child_header(
    state.current[0].header, resolve::cause_kind::reaction, 0, 0, 10, 30);
  children[1].payload.value = 3;

  REQUIRE(resolve::advance(state, std::span<const test_item>{children}));
  CHECK(state.frontier_index == 1);
  CHECK(state.total_jobs == 4);
  CHECK(state.current[0].header.root == 1);
  CHECK(state.current[0].header.id == 52);
  CHECK(state.current[1].header.root == 2);
  CHECK(state.current[1].header.id == 53);

  REQUIRE(resolve::advance(state, std::span<const test_item>{}));
  CHECK(state.complete());
  CHECK(state.frontier_index == 2);
}

TEST_CASE("resolve frontier rejects ambiguous provenance") {
  std::array roots{make_root(1, 10, 1), make_root(1, 10, 2)};
  resolve::frontier_state<test_item> state;
  CHECK_FALSE(resolve::begin(state, std::span<const test_item>{roots}));
  CHECK(state.status == resolve::frontier_status::faulted);
  CHECK(state.error.code == resolve::fault_code::invalid_provenance);
}

TEST_CASE("retaliation is once per hit and cannot recurse through its lineage") {
  auto first_hit = make_root(1, 20, 10).header;
  first_hit.id = 100;
  auto second_hit = make_root(2, 20, 20).header;
  second_hit.id = 101;

  resolve::retaliation_journal<integer_delta> retaliation;
  retaliation.begin_record(5);
  CHECK(retaliation.emit(first_hit, 11, 0, 0, 20, 10, {3}) ==
        resolve::retaliation_emit_result::recorded);
  CHECK(retaliation.emit(first_hit, 11, 0, 1, 20, 10, {3}) ==
        resolve::retaliation_emit_result::recorded);
  CHECK(retaliation.emit(first_hit, 12, 0, 2, 20, 10, {4}) ==
        resolve::retaliation_emit_result::recorded);
  CHECK(retaliation.emit(second_hit, 11, 0, 0, 20, 10, {5}) ==
        resolve::retaliation_emit_result::recorded);

  resolve::instance_id next_instance = 500;
  retaliation.seal(next_instance);
  REQUIRE(retaliation.records().size() == 3);
  CHECK(next_instance == 503); // duplicate discovery consumed no gameplay id
  CHECK(retaliation.records()[0].key.triggering_instance == 100);
  CHECK(retaliation.records()[1].key.triggering_instance == 100);
  CHECK(retaliation.records()[2].key.triggering_instance == 101);
  for (size_t i = 0; i < retaliation.records().size(); ++i) {
    const auto& header = retaliation.records()[i].header;
    CHECK(header.id == 500 + i);
    CHECK(header.cause == resolve::cause_kind::retaliation);
    CHECK(header.retaliation_lineage);
  }

  const auto retaliation_child = retaliation.records()[0].header;
  auto reaction_descendant = resolve::make_child_header(
    retaliation_child, resolve::cause_kind::reaction, 0, 0, 10, 30);
  reaction_descendant.id = 600; // model the descendant after its own semantic seal
  CHECK(reaction_descendant.retaliation_lineage);

  resolve::retaliation_journal<integer_delta> recursive;
  recursive.begin_record(2);
  CHECK(recursive.emit(retaliation_child, 13, 0, 0, 10, 20, {6}) ==
        resolve::retaliation_emit_result::suppressed_lineage);
  CHECK(recursive.emit(reaction_descendant, 13, 0, 1, 30, 20, {7}) ==
        resolve::retaliation_emit_result::suppressed_lineage);

  auto unsealed = resolve::make_child_header(
    first_hit, resolve::cause_kind::reaction, 0, 2, 10, 30);
  CHECK(recursive.emit(unsealed, 13, 0, 2, 30, 20, {8}) ==
        resolve::retaliation_emit_result::invalid_trigger);
}

TEST_CASE("retaliation has a deterministic per-trigger budget") {
  auto hit = make_root(1, 20, 10).header;
  hit.id = 100;

  resolve::retaliation_journal<integer_delta> retaliation;
  retaliation.begin_record(2);
  REQUIRE(retaliation.emit(hit, 11, 0, 0, 20, 10, {3}) ==
          resolve::retaliation_emit_result::recorded);
  REQUIRE(retaliation.emit(hit, 12, 0, 1, 20, 10, {4}) ==
          resolve::retaliation_emit_result::recorded);

  resolve::resolution_limits limits;
  limits.max_retaliations_per_trigger = 1;
  resolve::instance_id next_instance = 200;
  CHECK_THROWS_AS(retaliation.seal(next_instance, limits), std::length_error);
  CHECK(next_instance == 200);
}

TEST_CASE("ordered retaliation records can join a later project frontier") {
  const auto root = make_root(1, 20, 10);
  resolve::frontier_state<test_item> state;
  REQUIRE(resolve::begin(state, std::span<const test_item>{&root, 1}, {}, 100));

  resolve::retaliation_journal<integer_delta> retaliation;
  retaliation.begin_record(1);
  REQUIRE(retaliation.emit(state.current.front().header, 7, 1, 0, 20, 10, {3}) ==
          resolve::retaliation_emit_result::recorded);
  retaliation.seal_ordered();
  REQUIRE(retaliation.records().size() == 1);
  CHECK(retaliation.records().front().header.id == resolve::invalid_instance);

  const test_item child{
    retaliation.records().front().header,
    retaliation.records().front().payload};
  REQUIRE(resolve::advance(state, std::span<const test_item>{&child, 1}));
  REQUIRE(state.current.size() == 1);
  CHECK(state.current.front().header.id == 101);
  CHECK(state.current.front().header.cause == resolve::cause_kind::retaliation);
  CHECK(state.current.front().header.retaliation_lineage);
  CHECK(state.total_jobs == 2);
}

TEST_CASE("minimum HP guard prevents lethal damage without resurrecting") {
  resolve::damage_route<int32_t> living;
  living.hp_before = 5;
  living.proposed_hp_after = -3;
  living.committed_hp_after = -3;
  resolve::apply_minimum_hp_guard(living, 1);
  CHECK(living.committed_hp_after == 1);
  CHECK(living.lethal_prevented == 4);
  CHECK(living.survived_by_guard);

  resolve::damage_route<int32_t> already_dead;
  already_dead.hp_before = 0;
  already_dead.proposed_hp_after = -4;
  already_dead.committed_hp_after = -4;
  resolve::apply_minimum_hp_guard(already_dead, 1);
  CHECK(already_dead.committed_hp_after == 0);
  CHECK(already_dead.lethal_prevented == 4);
  CHECK_FALSE(already_dead.survived_by_guard);

  resolve::damage_route<int32_t> nonlethal;
  nonlethal.hp_before = 5;
  nonlethal.proposed_hp_after = 3;
  nonlethal.committed_hp_after = 3;
  resolve::apply_minimum_hp_guard(nonlethal, 1);
  CHECK(nonlethal.committed_hp_after == 3);
  CHECK(nonlethal.lethal_prevented == 0);
  CHECK_FALSE(nonlethal.survived_by_guard);
}
