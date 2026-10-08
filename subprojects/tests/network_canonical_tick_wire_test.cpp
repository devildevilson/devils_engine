#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <devils_engine/network/canonical_tick_wire.h>
#include <doctest/doctest.h>

namespace net = devils_engine::network;

namespace {

std::vector<std::byte> prepared() {
  std::vector<std::byte> result;
  result.reserve(net::canonical_tick_max_bytes);
  return result;
}

} // namespace

TEST_CASE("network canonical tick closes an empty tick with or without a root") {
  auto bytes = prepared();
  REQUIRE(net::try_encode_canonical_tick(17, std::nullopt, {}, bytes) ==
          net::canonical_tick_wire_status::ok);
  CHECK(bytes.size() == net::canonical_tick_header_bytes);
  std::array<net::canonical_tick_record_view, 2> storage{};
  net::canonical_tick_view view;
  REQUIRE(net::try_decode_canonical_tick(bytes, storage, view) ==
          net::canonical_tick_wire_status::ok);
  CHECK(view.tick == 17);
  CHECK_FALSE(view.state_root.has_value());
  CHECK(view.records.empty());

  REQUIRE(net::try_encode_canonical_tick(18, UINT64_C(0x0123456789abcdef), {}, bytes) ==
          net::canonical_tick_wire_status::ok);
  CHECK(bytes.size() == net::canonical_tick_header_bytes + net::canonical_tick_root_bytes);
  REQUIRE(net::try_decode_canonical_tick(bytes, storage, view) ==
          net::canonical_tick_wire_status::ok);
  CHECK(view.tick == 18);
  CHECK(view.state_root == UINT64_C(0x0123456789abcdef));
}

TEST_CASE("network canonical tick keeps origin and full sequence for each accepted input") {
  auto bytes = prepared();
  const std::array<std::byte, 2> first_payload{std::byte{3}, std::byte{4}};
  const std::array<std::byte, 1> second_payload{std::byte{5}};
  const std::array records{
    net::canonical_tick_record_view{7, 65536, first_payload},
    net::canonical_tick_record_view{8, 65536, second_payload}};
  REQUIRE(net::try_encode_canonical_tick(42, 123456, records, bytes) ==
          net::canonical_tick_wire_status::ok);
  CHECK(bytes.size() == net::canonical_tick_header_bytes + net::canonical_tick_root_bytes +
                        2 * net::canonical_tick_record_header_bytes +
                        first_payload.size() + second_payload.size());
  std::array<net::canonical_tick_record_view, 2> storage{};
  net::canonical_tick_view view;
  REQUIRE(net::try_decode_canonical_tick(bytes, storage, view) ==
          net::canonical_tick_wire_status::ok);
  CHECK(view.tick == 42);
  CHECK(view.state_root == 123456);
  REQUIRE(view.records.size() == 2);
  CHECK(view.records[0].origin_peer == 7);
  CHECK(view.records[0].input_sequence == 65536);
  CHECK(std::equal(view.records[0].payload.begin(), view.records[0].payload.end(),
                   first_payload.begin(), first_payload.end()));
  CHECK(view.records[1].origin_peer == 8);
  CHECK(view.records[1].input_sequence == 65536);
  CHECK(std::equal(view.records[1].payload.begin(), view.records[1].payload.end(),
                   second_payload.begin(), second_payload.end()));
}

TEST_CASE("network canonical tick refuses ambiguity before publication") {
  auto bytes = prepared();
  const std::array<std::byte, 1> payload{std::byte{9}};
  const std::array duplicate{
    net::canonical_tick_record_view{7, 3, payload},
    net::canonical_tick_record_view{7, 3, payload}};
  CHECK(net::try_encode_canonical_tick(1, std::nullopt, duplicate, bytes) ==
        net::canonical_tick_wire_status::duplicate_input);
  CHECK(bytes.empty());
  CHECK(net::try_encode_canonical_tick(0, std::nullopt, {}, bytes) ==
        net::canonical_tick_wire_status::invalid_header);

  const std::array one{duplicate[0]};
  REQUIRE(net::try_encode_canonical_tick(1, std::nullopt, one, bytes) ==
          net::canonical_tick_wire_status::ok);
  std::array<net::canonical_tick_record_view, 1> storage{};
  net::canonical_tick_view view;
  REQUIRE(net::try_decode_canonical_tick(bytes, storage, view) ==
          net::canonical_tick_wire_status::ok);
  const auto accepted = view;
  const auto record = storage[0];

  SUBCASE("old message class is not reinterpreted") {
    auto forged = bytes;
    forged[0] = std::byte{1};
    CHECK(net::try_decode_canonical_tick(forged, storage, view) ==
          net::canonical_tick_wire_status::wrong_message_type);
  }
  SUBCASE("truncation refuses every partial record") {
    for (size_t length = 11; length < bytes.size(); ++length) {
      CHECK(net::try_decode_canonical_tick(std::span(bytes).first(length), storage, view) ==
            net::canonical_tick_wire_status::truncated);
    }
  }
  SUBCASE("a duplicate pair in a forged message is refused") {
    REQUIRE(net::try_encode_canonical_tick(1, std::nullopt,
              std::array{net::canonical_tick_record_view{7, 3, payload},
                         net::canonical_tick_record_view{7, 4, payload}}, bytes) ==
            net::canonical_tick_wire_status::ok);
    const size_t second_sequence = net::canonical_tick_header_bytes +
      net::canonical_tick_record_header_bytes + payload.size() + 8;
    bytes[second_sequence] = std::byte{3};
    std::array<net::canonical_tick_record_view, 2> forged_storage{};
    CHECK(net::try_decode_canonical_tick(bytes, forged_storage, view) ==
          net::canonical_tick_wire_status::duplicate_input);
  }
  SUBCASE("a trailing byte is refused") {
    bytes.push_back(std::byte{0});
    CHECK(net::try_decode_canonical_tick(bytes, storage, view) ==
          net::canonical_tick_wire_status::trailing_bytes);
  }
  SUBCASE("unprepared record storage is refused") {
    CHECK(net::try_decode_canonical_tick(bytes, {}, view) ==
          net::canonical_tick_wire_status::capacity_exceeded);
  }
  SUBCASE("declared byte and record budgets are enforced before publication") {
    const std::vector<std::byte> oversized(net::canonical_tick_max_bytes + 1, std::byte{0});
    CHECK(net::try_decode_canonical_tick(oversized, storage, view) ==
          net::canonical_tick_wire_status::too_large);
    auto forged = bytes;
    forged[10] = std::byte{uint8_t(net::canonical_tick_max_records + 1)};
    CHECK(net::try_decode_canonical_tick(forged, storage, view) ==
          net::canonical_tick_wire_status::too_many_records);
    const std::array<std::byte, net::canonical_tick_max_payload_bytes + 1> large_payload{};
    const std::array invalid{net::canonical_tick_record_view{7, 3, large_payload}};
    CHECK(net::try_encode_canonical_tick(1, std::nullopt, invalid, bytes) ==
          net::canonical_tick_wire_status::invalid_record);
  }
  CHECK(view.tick == accepted.tick);
  CHECK(view.state_root == accepted.state_root);
  CHECK(view.records.data() == accepted.records.data());
  CHECK(storage[0].origin_peer == record.origin_peer);
  CHECK(storage[0].input_sequence == record.input_sequence);
}
