#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace {

namespace network = devils_engine::network;

struct actor_value {
  float x = 0.0f;
  float y = 0.0f;
  std::uint32_t health = 0;
  bool operator==(const actor_value&) const = default;
};

using snapshot = network::keyed_snapshot<std::uint32_t, actor_value, std::uint32_t>;
using delta = network::keyed_delta<std::uint32_t, actor_value, std::uint32_t>;

struct snapshot_wire_size {
  std::size_t operator()(const snapshot& value) const noexcept {
    constexpr std::size_t entry_bytes = 4 + 4 + 4 + 4 + 4;
    return 4 + value.size() * entry_bytes;
  }
};

using store = network::baseline_store<std::uint64_t, snapshot, snapshot_wire_size>;

snapshot initial_state() {
  return {
    {10, 1, {1.0f, 2.0f, 100}},
    {20, 4, {5.0f, 8.0f, 80}},
  };
}

snapshot changed_state() {
  return {
    {10, 2, {1.5f, 2.0f, 95}},
    {30, 1, {-4.0f, 3.0f, 50}},
  };
}

std::optional<snapshot> apply_delta(const snapshot& base, const delta& changes) {
  auto result = network::apply_keyed_delta(base, changes);
  return std::move(result.snapshot);
}

} // namespace

TEST_CASE("network state frame window accepts only advancing compatible frames") {
  network::state_frame_window<std::uint8_t, 8> frames(3);
  CHECK(frames.classify(3, 250) == network::state_frame_acceptance::accepted);
  CHECK_FALSE(frames.initialized());
  CHECK(frames.commit(3, 250) == network::state_frame_acceptance::accepted);
  CHECK(frames.newest() == std::uint8_t{250});
  CHECK(frames.commit(3, 250) == network::state_frame_acceptance::duplicate);
  CHECK(frames.commit(3, 249) == network::state_frame_acceptance::stale);
  CHECK(frames.commit(3, 2) == network::state_frame_acceptance::accepted);
  CHECK(frames.newest() == std::uint8_t{2});
  CHECK(frames.commit(3, 11) == network::state_frame_acceptance::too_far_ahead);
  CHECK(frames.commit(4, 3) ==
        network::state_frame_acceptance::format_version_mismatch);
  CHECK(frames.newest() == std::uint8_t{2});

  CHECK(frames.reset(4, 100) ==
        network::state_frame_acceptance::format_version_mismatch);
  CHECK(frames.reset(3, 100) == network::state_frame_acceptance::accepted);
  CHECK(frames.newest() == std::uint8_t{100});
}

TEST_CASE("network keyed delta describes create update and delete") {
  const snapshot base = initial_state();
  const snapshot current = changed_state();
  const auto built = network::make_keyed_delta(base, current);
  REQUIRE(built.status == network::keyed_delta_status::success);
  REQUIRE(built.delta.size() == 3);

  CHECK(built.delta[0].key == 10);
  CHECK(built.delta[0].expected_version == 1u);
  REQUIRE(built.delta[0].result.has_value());
  CHECK(built.delta[0].result->version == 2u);

  CHECK(built.delta[1].key == 20);
  CHECK(built.delta[1].expected_version == 4u);
  CHECK_FALSE(built.delta[1].result.has_value());

  CHECK(built.delta[2].key == 30);
  CHECK_FALSE(built.delta[2].expected_version.has_value());
  REQUIRE(built.delta[2].result.has_value());
  CHECK(built.delta[2].result->version == 1u);

  const auto applied = network::apply_keyed_delta(base, built.delta);
  REQUIRE(applied.status == network::keyed_delta_status::success);
  REQUIRE(applied.snapshot.has_value());
  CHECK(*applied.snapshot == current);
}

TEST_CASE("network keyed delta rejects noncanonical and unversioned changes") {
  const snapshot base = initial_state();
  snapshot duplicate_keys = base;
  duplicate_keys.push_back(base.back());
  CHECK(network::make_keyed_delta(base, duplicate_keys).status ==
        network::keyed_delta_status::current_not_canonical);

  snapshot unversioned = base;
  unversioned.front().value.health = 1;
  CHECK(network::make_keyed_delta(base, unversioned).status ==
        network::keyed_delta_status::version_not_advanced);

  delta duplicate_change = {
    {30, std::nullopt, network::versioned_value<actor_value, std::uint32_t>{1, {0, 0, 1}}},
    {30, std::nullopt, network::versioned_value<actor_value, std::uint32_t>{1, {0, 0, 1}}},
  };
  const auto duplicate_result = network::apply_keyed_delta(base, duplicate_change);
  CHECK(duplicate_result.status == network::keyed_delta_status::delta_not_canonical);
  CHECK_FALSE(duplicate_result.snapshot.has_value());
}

TEST_CASE("network keyed lifecycle preconditions make repeated create and delete explicit") {
  const snapshot base = initial_state();
  const delta create = {{
    30,
    std::nullopt,
    network::versioned_value<actor_value, std::uint32_t>{1, {3, 4, 60}},
  }};
  const auto created = network::apply_keyed_delta(base, create);
  REQUIRE(created.snapshot.has_value());
  const auto duplicate_create = network::apply_keyed_delta(*created.snapshot, create);
  CHECK(duplicate_create.status == network::keyed_delta_status::precondition_failed);
  CHECK_FALSE(duplicate_create.snapshot.has_value());

  const delta erase = {{20, 4u, std::nullopt}};
  const auto erased = network::apply_keyed_delta(base, erase);
  REQUIRE(erased.snapshot.has_value());
  const auto duplicate_erase = network::apply_keyed_delta(*erased.snapshot, erase);
  CHECK(duplicate_erase.status == network::keyed_delta_status::precondition_failed);
  CHECK_FALSE(duplicate_erase.snapshot.has_value());

  const delta meaningless = {{40, std::nullopt, std::nullopt}};
  CHECK(network::apply_keyed_delta(base, meaningless).status ==
        network::keyed_delta_status::invalid_change);
}

TEST_CASE("network baseline store applies deltas only from their exact retained base") {
  const snapshot base = initial_state();
  const snapshot current = changed_state();
  const auto built = network::make_keyed_delta(base, current);
  REQUIRE(built.succeeded());

  store baselines(3, 1024);
  REQUIRE(baselines.try_store(100, base).stored());
  const auto applied = network::try_materialize_delta(
    baselines, std::uint64_t{100}, std::uint64_t{101}, built.delta, apply_delta);
  CHECK(applied.status == network::delta_materialize_status::materialized);
  REQUIRE(baselines.find(101) != nullptr);
  CHECK(*baselines.find(101) == current);

  const auto missing = network::try_materialize_delta(
    baselines, std::uint64_t{99}, std::uint64_t{102}, built.delta, apply_delta);
  CHECK(missing.status == network::delta_materialize_status::missing_baseline);
  CHECK(baselines.newest_id() == 101u);

  const auto duplicate = network::try_materialize_delta(
    baselines, std::uint64_t{100}, std::uint64_t{101}, built.delta, apply_delta);
  CHECK(duplicate.status == network::delta_materialize_status::duplicate_result);
  CHECK(baselines.retained_count() == 2);
}

TEST_CASE("network baseline store enforces serialized byte and count budgets") {
  store baselines(2, 84);
  const snapshot base = initial_state(); // 44 logical wire bytes
  const snapshot current = changed_state();
  REQUIRE(baselines.try_store(7, base).stored());

  const auto second = baselines.try_store(8, current);
  CHECK(second.status == network::baseline_store_status::stored);
  CHECK(second.evicted_count == 1);
  CHECK(second.evicted_bytes == 44);
  CHECK(baselines.find(7) == nullptr);
  CHECK(baselines.find(8) != nullptr);

  snapshot oversized = current;
  oversized.push_back({40, 1, {0, 0, 1}});
  oversized.push_back({50, 1, {0, 0, 1}});
  oversized.push_back({60, 1, {0, 0, 1}});
  const auto rejected = baselines.try_store(9, oversized);
  CHECK(rejected.status == network::baseline_store_status::budget_exceeded);
  CHECK(baselines.newest_id() == 8u);
  CHECK(baselines.retained_count() == 1);
}

TEST_CASE("network rejected project delta does not publish a result baseline") {
  store baselines(4, 1024);
  REQUIRE(baselines.try_store(1, initial_state()).stored());
  const delta bad_version = {{
    10,
    99u,
    network::versioned_value<actor_value, std::uint32_t>{100, {1, 1, 1}},
  }};

  const auto result = network::try_materialize_delta(
    baselines, std::uint64_t{1}, std::uint64_t{2}, bad_version, apply_delta);
  CHECK(result.status == network::delta_materialize_status::delta_rejected);
  CHECK(baselines.find(2) == nullptr);
  CHECK(baselines.retained_count() == 1);
}
