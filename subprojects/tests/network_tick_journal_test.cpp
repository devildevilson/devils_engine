#include <algorithm>
#include <bit>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

#include "network_test_types.h"

namespace network = devils_engine::network;
namespace fixture = devils_engine::network::test;

namespace {

struct intent_tick {
  constexpr uint16_t operator()(const fixture::move_intent& value) const noexcept {
    return value.tick;
  }
};

struct intent_less {
  constexpr bool operator()(const fixture::move_intent& left,
                            const fixture::move_intent& right) const noexcept {
    return std::tie(left.principal, left.sequence) < std::tie(right.principal, right.sequence);
  }
};

struct intent_equivalent {
  constexpr bool operator()(const fixture::move_intent& left,
                            const fixture::move_intent& right) const noexcept {
    return left.principal == right.principal && left.sequence == right.sequence;
  }
};

using intent_journal = network::tick_journal<
  fixture::move_intent,
  uint16_t,
  intent_tick,
  intent_less,
  intent_equivalent>;

struct frame_tick {
  constexpr uint32_t operator()(const fixture::transform_state_frame& value) const noexcept {
    return value.tick;
  }
};

struct frame_less {
  constexpr bool operator()(const fixture::transform_state_frame& left,
                            const fixture::transform_state_frame& right) const noexcept {
    return left.entity < right.entity;
  }
};

struct frame_equivalent {
  constexpr bool operator()(const fixture::transform_state_frame& left,
                            const fixture::transform_state_frame& right) const noexcept {
    return left.entity == right.entity;
  }
};

using frame_journal = network::tick_journal<
  fixture::transform_state_frame,
  uint32_t,
  frame_tick,
  frame_less,
  frame_equivalent>;

struct tiny_record {
  uint8_t tick = 0;
  uint8_t sequence = 0;
  int8_t value = 0;
};

struct tiny_tick {
  constexpr uint8_t operator()(const tiny_record& value) const noexcept {
    return value.tick;
  }
};

struct tiny_less {
  constexpr bool operator()(const tiny_record& left, const tiny_record& right) const noexcept {
    return left.sequence < right.sequence;
  }
};

struct tiny_equivalent {
  constexpr bool operator()(const tiny_record& left, const tiny_record& right) const noexcept {
    return left.sequence == right.sequence;
  }
};

void append_u16(std::vector<uint8_t>& bytes, const uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

std::vector<uint8_t> canonical_intent_bytes(const std::vector<fixture::move_intent>& input) {
  intent_journal journal;
  const auto tag = journal.begin(40, input.size());
  for (const auto& value : input) {
    REQUIRE(journal.try_record(value) == network::tick_record_result::recorded);
  }
  journal.seal();
  auto batch = journal.consume(tag);

  std::vector<uint8_t> bytes;
  bytes.reserve(batch.records().size() * 8);
  for (const auto& value : batch.records()) {
    append_u16(bytes, value.tick);
    append_u16(bytes, value.principal);
    append_u16(bytes, value.sequence);
    bytes.push_back(std::bit_cast<uint8_t>(value.x));
    bytes.push_back(std::bit_cast<uint8_t>(value.y));
  }
  return bytes;
}

} // namespace

TEST_CASE("network tick journal erases physical arrival order") {
  std::vector<fixture::move_intent> input{
    {40, 3, 2, 1, -1},
    {40, 1, 8, -1, 0},
    {40, 2, 1, 0, 1},
    {40, 1, 7, 1, 1}};
  auto shuffled = input;
  std::reverse(shuffled.begin(), shuffled.end());

  CHECK(canonical_intent_bytes(input) == canonical_intent_bytes(shuffled));
}

TEST_CASE("network tick journal rejects another tick without writing it") {
  intent_journal journal;
  const auto tag = journal.begin(40, 2);
  CHECK(journal.try_record(fixture::move_intent{41, 1, 1, 0, 0}) ==
        network::tick_record_result::wrong_tick);
  CHECK(journal.recorded_count() == 0);
  journal.seal();
  CHECK(journal.records(tag).empty());
}

TEST_CASE("network tick journal rejects duplicate semantic provenance") {
  intent_journal journal;
  const auto tag = journal.begin(40, 2);
  REQUIRE(journal.try_record({40, 7, 3, 1, 0}) == network::tick_record_result::recorded);
  REQUIRE(journal.try_record({40, 7, 3, -1, 0}) == network::tick_record_result::recorded);
  CHECK_THROWS_AS(journal.seal(), std::invalid_argument);
  CHECK(journal.phase() == network::tick_journal_phase::faulted);
  journal.retire(tag);
}

TEST_CASE("network tick journal latches capacity overflow") {
  intent_journal exact;
  const auto exact_tag = exact.begin(40, 1);
  REQUIRE(exact.try_record({40, 1, 1, 0, 0}) == network::tick_record_result::recorded);
  exact.seal();
  CHECK(exact.records(exact_tag).size() == 1);

  intent_journal overflow;
  const auto overflow_tag = overflow.begin(40, 1);
  REQUIRE(overflow.try_record({40, 1, 1, 0, 0}) == network::tick_record_result::recorded);
  CHECK(overflow.try_record({40, 2, 1, 0, 0}) ==
        network::tick_record_result::capacity_exceeded);
  CHECK(overflow.overflowed());
  CHECK(overflow.recorded_count() == 1);
  CHECK_THROWS_AS(overflow.seal(), std::length_error);
  CHECK(overflow.phase() == network::tick_journal_phase::faulted);
  overflow.retire(overflow_tag);
}

TEST_CASE("network tick journal exposes only a sealed tick") {
  intent_journal journal;
  const auto tag = journal.begin(40, 1);
  CHECK_THROWS_AS(journal.records(tag), std::logic_error);
  REQUIRE(journal.try_record({40, 1, 1, 0, 0}) == network::tick_record_result::recorded);
  journal.seal();
  CHECK_THROWS_AS(journal.try_record(fixture::move_intent{40, 2, 1, 0, 0}), std::logic_error);
  CHECK_THROWS_AS(journal.retire(tag), std::logic_error);
}

TEST_CASE("network tick journal consume transfers ownership once") {
  intent_journal journal;
  const auto tag = journal.begin(40, 1);
  REQUIRE(journal.try_record({40, 1, 1, 1, -1}) == network::tick_record_result::recorded);
  journal.seal();

  const auto batch = journal.consume(tag);
  REQUIRE(batch.records().size() == 1);
  CHECK(batch.tag() == tag);
  CHECK(batch.records().front() == fixture::move_intent{40, 1, 1, 1, -1});
  CHECK_THROWS_AS(journal.consume(tag), std::logic_error);
  journal.retire(tag);
  CHECK(journal.phase() == network::tick_journal_phase::idle);
}

TEST_CASE("network tick journal generation survives tick wrap and rejects stale tags") {
  using tiny_journal = network::tick_journal<
    tiny_record,
    uint8_t,
    tiny_tick,
    tiny_less,
    tiny_equivalent>;

  tiny_journal journal;
  const auto old_tag = journal.begin(uint8_t{0}, 1);
  REQUIRE(journal.try_record({0, 1, -1}) == network::tick_record_result::recorded);
  journal.seal();
  auto old_batch = journal.consume(old_tag);
  journal.retire(old_tag);

  for (uint16_t tick = 1; tick <= 255; ++tick) {
    const auto wrapped_tag = journal.begin(static_cast<uint8_t>(tick), 0);
    journal.seal();
    static_cast<void>(journal.consume(wrapped_tag));
    journal.retire(wrapped_tag);
  }

  const auto current_tag = journal.begin(uint8_t{0}, 1);
  REQUIRE(journal.try_record({0, 1, 1}) == network::tick_record_result::recorded);
  journal.seal();
  CHECK(old_tag.tick == uint8_t{0});
  CHECK(current_tag.tick == uint8_t{0});
  CHECK(old_tag.generation != current_tag.generation);
  CHECK_THROWS_AS(journal.records(old_tag), std::invalid_argument);
  CHECK(journal.records(current_tag).front().value == 1);
  CHECK(old_batch.records().front().value == -1);
}

TEST_CASE("network tick journal accepts unrelated project record shapes") {
  static_assert(!std::is_base_of_v<fixture::move_intent, fixture::transform_state_frame>);

  frame_journal journal;
  const auto tag = journal.begin(900, 2);
  REQUIRE(journal.try_record({900, 12, 4.0f, 5.0f, 1.0f}) ==
          network::tick_record_result::recorded);
  REQUIRE(journal.try_record({900, 3, 1.0f, 2.0f, 0.5f}) ==
          network::tick_record_result::recorded);
  journal.seal();
  const auto records = journal.records(tag);
  REQUIRE(records.size() == 2);
  CHECK(records[0].entity == 3);
  CHECK(records[1].entity == 12);
}
