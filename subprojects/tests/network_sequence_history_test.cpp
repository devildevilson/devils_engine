#include <concepts>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace network = devils_engine::network;

TEST_CASE("network sequence window accepts gaps and unseen late values") {
  network::sequence_window<uint16_t, 8> window;
  CHECK_FALSE(window.initialized());
  CHECK(window.classify(10) == network::sequence_classification::new_value);
  CHECK_FALSE(window.initialized());

  CHECK(window.observe(10) == network::sequence_classification::new_value);
  CHECK(window.newest() == 10);
  CHECK(window.observe(12) == network::sequence_classification::new_value);
  CHECK(window.newest() == 12);
  CHECK(window.observe(11) == network::sequence_classification::new_value);
  CHECK(window.newest() == 12);
  CHECK(window.observe(11) == network::sequence_classification::duplicate);
  CHECK(window.observe(12) == network::sequence_classification::duplicate);
}

TEST_CASE("network sequence window distinguishes stale and too-far-ahead values") {
  network::sequence_window<uint8_t, 8> window;
  REQUIRE(window.observe(10) == network::sequence_classification::new_value);

  CHECK(window.observe(2) == network::sequence_classification::stale);
  CHECK(window.observe(19) == network::sequence_classification::too_far_ahead);
  CHECK(window.observe(138) == network::sequence_classification::too_far_ahead);
  CHECK(window.newest() == uint8_t{10});

  CHECK(window.observe(18) == network::sequence_classification::new_value);
  CHECK(window.newest() == uint8_t{18});
  CHECK(window.observe(10) == network::sequence_classification::stale);
}

TEST_CASE("network sequence window crosses the unsigned maximum") {
  network::sequence_window<uint8_t, 8> window;
  for (const uint8_t value : {uint8_t{254}, uint8_t{255}, uint8_t{0}, uint8_t{1}}) {
    CHECK(window.observe(value) == network::sequence_classification::new_value);
  }
  CHECK(window.newest() == uint8_t{1});
  CHECK(window.observe(255) == network::sequence_classification::duplicate);
  CHECK(window.observe(250) == network::sequence_classification::new_value);
  CHECK(window.observe(250) == network::sequence_classification::duplicate);
  CHECK(window.observe(249) == network::sequence_classification::stale);
  CHECK(window.observe(10) == network::sequence_classification::too_far_ahead);
  CHECK(window.newest() == uint8_t{1});
}

TEST_CASE("network sequence window reset establishes a new acceptance epoch") {
  network::sequence_window<uint32_t, 64> window;
  window.reset(4'000'000'000u);
  CHECK(window.newest() == 4'000'000'000u);
  CHECK(window.observe(4'000'000'000u) == network::sequence_classification::duplicate);
  window.reset();
  CHECK_FALSE(window.initialized());
  CHECK(window.observe(7) == network::sequence_classification::new_value);
  CHECK(window.newest() == 7u);
}

namespace {

struct bundle {
  std::string name;
  std::vector<uint8_t> bytes;
  bool operator==(const bundle&) const = default;
};

using history = network::bounded_history<uint32_t, bundle>;

static_assert(std::same_as<decltype(std::declval<const history&>().find(0)),
                           const bundle*>);
static_assert(std::same_as<decltype(std::declval<history&>().find(0)),
                           const bundle*>);
static_assert(std::same_as<std::ranges::range_reference_t<decltype(std::declval<const history&>().entries())>,
                          const history::entry&>);
static_assert(std::ranges::random_access_range<decltype(std::declval<history&>().entries())>);

bundle make_bundle(const char* name, const std::initializer_list<uint8_t> bytes) {
  return {name, bytes};
}

} // namespace

TEST_CASE("network bounded history evicts oldest bundles by count") {
  history values(3, 100);
  REQUIRE(values.try_store(10, make_bundle("ten", {1}), 1).stored());
  REQUIRE(values.try_store(11, make_bundle("eleven", {2, 3}), 2).stored());
  REQUIRE(values.try_store(12, make_bundle("twelve", {4, 5, 6}), 3).stored());

  const auto result = values.try_store(13, make_bundle("thirteen", {7}), 1);
  CHECK(result.status == network::history_store_status::stored);
  CHECK(result.evicted_count == 1);
  CHECK(result.evicted_bytes == 1);
  CHECK(values.retained_count() == 3);
  CHECK(values.retained_bytes() == 6);
  CHECK(values.oldest_tick() == 11u);
  CHECK(values.newest_tick() == 13u);
  CHECK(values.find(10) == nullptr);
  REQUIRE(values.find(12) != nullptr);
  CHECK(values.find(12)->name == "twelve");
}

TEST_CASE("network bounded history evicts enough bundles for its byte budget") {
  history values(8, 6);
  REQUIRE(values.try_store(1, make_bundle("a", {1, 2}), 2).stored());
  REQUIRE(values.try_store(2, make_bundle("b", {3, 4, 5}), 3).stored());

  const auto result = values.try_store(3, make_bundle("c", {6, 7, 8, 9}), 4);
  CHECK(result.status == network::history_store_status::stored);
  CHECK(result.evicted_count == 2);
  CHECK(result.evicted_bytes == 5);
  CHECK(values.retained_count() == 1);
  CHECK(values.retained_bytes() == 4);
  CHECK(values.oldest_tick() == 3u);
}

TEST_CASE("network bounded history rejects duplicate out-of-order and oversized bundles transactionally") {
  history values(3, 5);
  REQUIRE(values.try_store(10, make_bundle("ten", {1, 2}), 2).stored());
  REQUIRE(values.try_store(12, make_bundle("twelve", {3}), 1).stored());

  const auto before_count = values.retained_count();
  const auto before_bytes = values.retained_bytes();
  CHECK(values.try_store(12, make_bundle("duplicate", {9}), 1).status ==
        network::history_store_status::duplicate_tick);
  CHECK(values.try_store(11, make_bundle("late", {9}), 1).status ==
        network::history_store_status::out_of_order);
  CHECK(values.try_store(13, make_bundle("large", {1, 2, 3, 4, 5, 6}), 6).status ==
        network::history_store_status::budget_exceeded);
  CHECK(values.retained_count() == before_count);
  CHECK(values.retained_bytes() == before_bytes);
  REQUIRE(values.find(10) != nullptr);
  CHECK(values.find(10)->name == "ten");
  REQUIRE(values.find(12) != nullptr);
  CHECK(values.find(12)->name == "twelve");
}

TEST_CASE("network bounded history retains explicit empty bundles") {
  network::bounded_history<uint64_t, std::vector<uint8_t>> values(2, 0);
  REQUIRE(values.try_store(40, {}, 0).stored());
  REQUIRE(values.try_store(41, {}, 0).stored());
  CHECK(values.retained_count() == 2);
  CHECK(values.retained_bytes() == 0);
  REQUIRE(values.find(40) != nullptr);
  CHECK(values.find(40)->empty());

  const auto result = values.try_store(42, {}, 0);
  CHECK(result.stored());
  CHECK(result.evicted_count == 1);
  CHECK(values.find(40) == nullptr);
  CHECK(values.oldest_tick() == uint64_t{41});
}

TEST_CASE("network bounded history with zero count budget refuses even empty bundles") {
  network::bounded_history<uint32_t, std::vector<uint8_t>> values(0, 100);
  const auto result = values.try_store(1, {}, 0);
  CHECK(result.status == network::history_store_status::budget_exceeded);
  CHECK(values.empty());
}
