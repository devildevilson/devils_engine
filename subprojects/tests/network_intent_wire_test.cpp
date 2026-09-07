#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace net = devils_engine::network;

namespace {

// Cell of 64 world units, quantum of 1/1024. The two exactly fill a 16-bit
// code, which is what fixed_axis::valid() demands.
constexpr net::fixed_axis<uint16_t> axis{6, 10};

// Project-owned kind codes. The library never learns what they mean.
constexpr uint8_t kind_move = 1;
constexpr uint8_t kind_turn = 2;
constexpr uint8_t kind_call = 3;
constexpr uint8_t kind_event = 4;

net::intent_layout_table make_layouts() {
  const std::array<net::intent_kind_layout, 4> declared{
    net::intent_kind_layout{kind_move, {2, false, false, false}},
    net::intent_kind_layout{kind_turn, {0, false, false, true}},
    net::intent_kind_layout{kind_call, {0, true, true, false}},
    net::intent_kind_layout{kind_event, {0, true, false, false}}};
  net::intent_layout_table table;
  REQUIRE(table.assign(declared) == net::intent_wire_status::ok);
  return table;
}

net::id_index_table make_registry() {
  const std::array<uint64_t, 5> ids{0xdead'beefull, 7, 0xffff'ffff'ffff'ffffull, 42, 1};
  net::id_index_table table;
  REQUIRE(net::id_index_table::try_build(ids, table) ==
          net::id_index_table::build_status::built);
  return table;
}

std::vector<std::byte> prepared(const size_t bytes = net::intent_batch_max_bytes) {
  std::vector<std::byte> buffer;
  buffer.reserve(bytes);
  return buffer;
}

std::vector<net::intent> prepared_intents(const size_t count) {
  std::vector<net::intent> out;
  out.reserve(count);
  return out;
}

} // namespace

TEST_CASE("network fixed axis is exact on its own lattice") {
  REQUIRE(axis.valid());
  CHECK(axis.quantum() == 1.0 / 1024.0);
  CHECK(axis.cell_size() == 64.0);
  CHECK_FALSE((net::fixed_axis<uint16_t>{6, 9}).valid());
  CHECK_FALSE((net::fixed_axis<uint16_t>{7, 10}).valid());

  // Re-encoding a decoded value returns the same code. This is the property
  // that a decimal quantum would only satisfy approximately.
  for (const int32_t key : {-1000000, -65536, -1, 0, 1, 4242, 1000000}) {
    for (const uint32_t code : {0u, 1u, 2u, 32768u, 65534u, 65535u}) {
      CAPTURE(key);
      CAPTURE(code);
      const double world = net::join_axis(axis, key, code);
      const auto split = net::split_axis(axis, world);
      CHECK(split == net::axis_split{key, code, false});
    }
  }
}

TEST_CASE("network fixed axis keeps resolution at distance and rounds away from zero") {
  struct sample {
    double world;
    int32_t key;
    uint32_t code;
  };
  // Golden values. A different platform which disagrees with any row has a
  // different quantizer, not a rounding preference.
  const std::array<sample, 9> golden{
    sample{0.0, 0, 0},
    sample{1.0, 0, 1024},
    sample{1.0 / 1024.0, 0, 1},
    sample{64.0, 1, 0},
    sample{-1.0, -1, 64512},
    sample{-1.0 / 1024.0, -1, 65535},
    sample{100000.5, 1562, 33280},
    // One quantum past a cell boundary a million units from the origin: the
    // code is still exactly one, which is the whole point of carrying a key.
    sample{1000000.0 + 1.0 / 1024.0, 15625, 1},
    sample{-64.0, -1, 0}};

  for (const auto& row : golden) {
    CAPTURE(row.world);
    const auto split = net::split_axis(axis, row.world);
    CHECK(split.key == row.key);
    CHECK(split.code == row.code);
    CHECK_FALSE(split.clamped);
    CHECK(net::join_axis(axis, split.key, split.code) == row.world);
  }

  SUBCASE("exactly half a quantum rounds away from zero on both signs") {
    const double half = 1.0 / 2048.0;
    CHECK(net::split_axis(axis, half) == net::axis_split{0, 1, false});
    CHECK(net::split_axis(axis, -half) == net::axis_split{-1, 65535, false});
  }

  SUBCASE("off-lattice error never exceeds half a quantum, near or far") {
    const double near_origin = 3.14159265358979;
    const double far_away = 1000000.0 + 3.14159265358979;
    for (const double world : {near_origin, far_away}) {
      CAPTURE(world);
      const auto split = net::split_axis(axis, world);
      REQUIRE_FALSE(split.clamped);
      const double restored = net::join_axis(axis, split.key, split.code);
      const double error = restored > world ? restored - world : world - restored;
      CHECK(error <= axis.quantum() / 2.0);
    }
  }

  SUBCASE("out-of-range and non-finite inputs are reported, not folded") {
    CHECK(net::split_axis(axis, 1e300).clamped);
    CHECK(net::split_axis(axis, -1e300).clamped);
    CHECK(net::split_axis(axis, std::numeric_limits<double>::quiet_NaN()).clamped);
    CHECK(net::split_axis(axis, std::numeric_limits<double>::infinity()).clamped);
  }
}

TEST_CASE("network turn quantization wraps instead of having a boundary") {
  using code_t = uint16_t;
  CHECK(net::split_turn<code_t>(0.0) == net::turn_split{0, false});
  // A full turn is the same direction as none, and says so by wrapping.
  CHECK(net::split_turn<code_t>(1.0) == net::turn_split{0, false});
  CHECK(net::split_turn<code_t>(-1.0) == net::turn_split{0, false});
  CHECK(net::split_turn<code_t>(0.25) == net::turn_split{16384, false});
  CHECK(net::split_turn<code_t>(0.5) == net::turn_split{32768, false});
  CHECK(net::split_turn<code_t>(-0.25) == net::turn_split{49152, false});
  CHECK(net::split_turn<code_t>(1.25) == net::turn_split{16384, false});

  for (const code_t code : {code_t(0), code_t(1), code_t(16384), code_t(65535)}) {
    CAPTURE(code);
    CHECK(net::split_turn<code_t>(net::join_turn(code)) == net::turn_split{code, false});
  }
  CHECK(net::split_turn<code_t>(std::numeric_limits<double>::quiet_NaN()).clamped);
}

TEST_CASE("network registry index is the sorted position and needs no agreement") {
  const auto registry = make_registry();
  REQUIRE(registry.size() == 5);
  CHECK(registry.index_of(1) == std::optional<uint16_t>(0));
  CHECK(registry.index_of(7) == std::optional<uint16_t>(1));
  CHECK(registry.index_of(42) == std::optional<uint16_t>(2));
  CHECK(registry.index_of(0xdead'beefull) == std::optional<uint16_t>(3));
  CHECK(registry.index_of(0xffff'ffff'ffff'ffffull) == std::optional<uint16_t>(4));
  CHECK_FALSE(registry.index_of(2).has_value());
  CHECK(registry.id_at(4) == std::optional<uint64_t>(0xffff'ffff'ffff'ffffull));
  CHECK_FALSE(registry.id_at(5).has_value());

  SUBCASE("registration order does not change the fingerprint") {
    const std::array<uint64_t, 5> shuffled{1, 42, 0xffff'ffff'ffff'ffffull, 0xdead'beefull, 7};
    net::id_index_table other;
    REQUIRE(net::id_index_table::try_build(shuffled, other) ==
            net::id_index_table::build_status::built);
    CHECK(other.fingerprint() == registry.fingerprint());
  }

  SUBCASE("one added identifier changes the fingerprint and shifts later indices") {
    const std::array<uint64_t, 6> grown{1, 7, 8, 42, 0xdead'beefull,
                                             0xffff'ffff'ffff'ffffull};
    net::id_index_table other;
    REQUIRE(net::id_index_table::try_build(grown, other) ==
            net::id_index_table::build_status::built);
    CHECK(other.fingerprint() != registry.fingerprint());
    // Shifting is not a defect: the handshake refuses a peer whose fingerprint
    // differs, so no session ever reads one index as two different things.
    CHECK(other.index_of(42) == std::optional<uint16_t>(3));
  }

  SUBCASE("empty and duplicated registries are refused") {
    net::id_index_table other;
    CHECK(net::id_index_table::try_build({}, other) == net::id_index_table::build_status::empty);
    const std::array<uint64_t, 3> repeated{5, 9, 5};
    CHECK(net::id_index_table::try_build(repeated, other) ==
          net::id_index_table::build_status::duplicate);
  }
}

TEST_CASE("network intent layout declarations are resolved once and refused when ambiguous") {
  net::intent_layout_table table;
  SUBCASE("a duplicate kind would give one wire code two readings") {
    const std::array<net::intent_kind_layout, 2> declared{
      net::intent_kind_layout{kind_move, {2, false, false, false}},
      net::intent_kind_layout{kind_move, {0, true, false, false}}};
    CHECK(table.assign(declared) == net::intent_wire_status::duplicate_kind);
  }
  SUBCASE("a kind outside the packed field is refused") {
    const std::array<net::intent_kind_layout, 1> declared{
      net::intent_kind_layout{net::intent_kind_limit + uint8_t(1), {0, true, false, false}}};
    CHECK(table.assign(declared) == net::intent_wire_status::invalid_layout);
  }
  SUBCASE("more axes than the record holds is refused") {
    const std::array<net::intent_kind_layout, 1> declared{
      net::intent_kind_layout{kind_move, {4, false, false, false}}};
    CHECK(table.assign(declared) == net::intent_wire_status::invalid_layout);
  }
  SUBCASE("declared shapes give the sizes the budget was computed from") {
    const auto layouts = make_layouts();
    CHECK(layouts.fields(kind_move).wire_bytes() == 7);  // kind + two axes
    CHECK(layouts.fields(kind_turn).wire_bytes() == 3);  // kind + direction
    CHECK(layouts.fields(kind_call).wire_bytes() == 7);  // kind + index + target
    CHECK(layouts.fields(kind_event).wire_bytes() == 3); // kind + index
  }
}

TEST_CASE("network intent batch round-trips mixed kinds and ticks") {
  const auto layouts = make_layouts();
  const auto registry = make_registry();
  auto wire = prepared();
  auto decoded = prepared_intents(8);

  net::intent move;
  move.kind = kind_move;
  move.tick_delta = 0;
  const auto x = net::split_axis(axis, 12.5), y = net::split_axis(axis, -3.75);
  const auto dx = net::relative_cell(0, x.key), dy = net::relative_cell(0, y.key);
  REQUIRE_FALSE(dx.out_of_range);
  REQUIRE_FALSE(dy.out_of_range);
  move.cell_delta = {dx.value, dy.value, 0};
  move.code = {uint16_t(x.code), uint16_t(y.code), 0};

  net::intent turn;
  turn.kind = kind_turn;
  turn.tick_delta = 1;
  turn.turn = uint16_t(net::split_turn<uint16_t>(0.125).code);

  net::intent call;
  call.kind = kind_call;
  call.tick_delta = 7;
  call.reference = *registry.index_of(42);
  call.target = 0x0012'3456;

  const std::array<net::intent, 3> sent{move, turn, call};
  REQUIRE(net::try_encode_intent_batch(0x1'0000 + 300, sent, layouts, registry, wire) ==
          net::intent_wire_status::ok);

  net::intent_batch_view view;
  REQUIRE(net::try_decode_intent_batch(wire, layouts, registry, view, decoded) ==
          net::intent_wire_status::ok);
  CHECK(view.base_tick_low == 300);
  CHECK(view.count == 3);
  REQUIRE(decoded.size() == 3);
  CHECK(decoded[0] == move);
  CHECK(decoded[1] == turn);
  CHECK(decoded[2] == call);

  // The decoded point restores the world coordinate exactly, because the wire
  // moved integers and the only floating-point step is reversible.
  CHECK(net::join_axis(axis, net::absolute_cell(0, decoded[0].cell_delta[0]),
                       decoded[0].code[0]) == 12.5);
  CHECK(net::join_axis(axis, net::absolute_cell(0, decoded[0].cell_delta[1]),
                       decoded[0].code[1]) == -3.75);
}

TEST_CASE("network cell delta refuses a target too far from its base cell") {
  CHECK(net::relative_cell(0, 0) == net::cell_delta{0, false});
  CHECK(net::relative_cell(1000, 1127) == net::cell_delta{127, false});
  CHECK(net::relative_cell(1000, 872) == net::cell_delta{-128, false});
  // One cell further is refused rather than wrapping into a nearby cell.
  CHECK(net::relative_cell(1000, 1128) == net::cell_delta{0, true});
  CHECK(net::relative_cell(1000, 871) == net::cell_delta{0, true});
  CHECK(net::relative_cell(-2000000000, 2000000000).out_of_range);

  for (const int32_t base : {-1000000, 0, 1000000}) {
    for (const int8_t delta : {int8_t(-128), int8_t(-1), int8_t(0),
                                    int8_t(1), int8_t(127)}) {
      CAPTURE(base);
      CAPTURE(int(delta));
      const int32_t key = net::absolute_cell(base, delta);
      CHECK(net::relative_cell(base, key) == net::cell_delta{delta, false});
    }
  }
}

TEST_CASE("network intent batch byte budget is measured, not assumed") {
  const auto layouts = make_layouts();
  const auto registry = make_registry();
  auto wire = prepared();

  SUBCASE("one movement intent for one tick") {
    net::intent move;
    move.kind = kind_move;
    const std::array<net::intent, 1> sent{move};
    REQUIRE(net::try_encode_intent_batch(1, sent, layouts, registry, wire) ==
            net::intent_wire_status::ok);
    // 3 header + 1 packed kind/tick + 2 axes of (cell delta + code)
    CHECK(wire.size() == 10);
  }

  SUBCASE("three ticks of movement resent for redundancy") {
    std::array<net::intent, 3> sent{};
    for (uint8_t i = 0; i < sent.size(); ++i) {
      sent[i].kind = kind_move;
      sent[i].tick_delta = i;
    }
    REQUIRE(net::try_encode_intent_batch(90, sent, layouts, registry, wire) ==
            net::intent_wire_status::ok);
    CHECK(wire.size() == 3 + 3 * 7);
    // Against the 51 bytes of transport overhead per packet, resending two more
    // ticks costs 14 bytes and removes the need for any retransmit protocol.
    CHECK(wire.size() < 51);
  }

  SUBCASE("a full eight-tick window of movement still fits one packet") {
    std::array<net::intent, 8> sent{};
    for (uint8_t i = 0; i < sent.size(); ++i) {
      sent[i].kind = kind_move;
      sent[i].tick_delta = i;
    }
    REQUIRE(net::try_encode_intent_batch(1000, sent, layouts, registry, wire) ==
            net::intent_wire_status::ok);
    CHECK(wire.size() == 59);
  }
}

TEST_CASE("network intent batch refuses malformed and hostile input") {
  const auto layouts = make_layouts();
  const auto registry = make_registry();
  auto wire = prepared();
  auto decoded = prepared_intents(8);
  net::intent_batch_view view;

  net::intent move;
  move.kind = kind_move;
  const std::array<net::intent, 1> one_move{move};

  SUBCASE("an undeclared kind is refused on both sides") {
    net::intent unknown;
    unknown.kind = 9;
    const std::array<net::intent, 1> sent{unknown};
    CHECK(net::try_encode_intent_batch(1, sent, layouts, registry, wire) ==
          net::intent_wire_status::unknown_kind);

    REQUIRE(net::try_encode_intent_batch(1, one_move, layouts, registry, wire) ==
            net::intent_wire_status::ok);
    auto forged = wire;
    forged[net::intent_batch_header_bytes] = std::byte(9);
    CHECK(net::try_decode_intent_batch(forged, layouts, registry, view, decoded) ==
          net::intent_wire_status::unknown_kind);
  }

  SUBCASE("a tick delta outside the packed window is refused at encode") {
    net::intent late = move;
    late.tick_delta = net::intent_tick_delta_limit + 1;
    const std::array<net::intent, 1> sent{late};
    CHECK(net::try_encode_intent_batch(1, sent, layouts, registry, wire) ==
          net::intent_wire_status::tick_delta_out_of_range);
  }

  SUBCASE("a registry index beyond the frozen set is refused on both sides") {
    net::intent call;
    call.kind = kind_call;
    call.reference = uint16_t(registry.size());
    const std::array<net::intent, 1> sent{call};
    CHECK(net::try_encode_intent_batch(1, sent, layouts, registry, wire) ==
          net::intent_wire_status::reference_out_of_range);

    call.reference = 0;
    const std::array<net::intent, 1> legal{call};
    REQUIRE(net::try_encode_intent_batch(1, legal, layouts, registry, wire) ==
            net::intent_wire_status::ok);
    auto forged = wire;
    forged[net::intent_batch_header_bytes + 1] = std::byte(0xff);
    forged[net::intent_batch_header_bytes + 2] = std::byte(0xff);
    CHECK(net::try_decode_intent_batch(forged, layouts, registry, view, decoded) ==
          net::intent_wire_status::reference_out_of_range);
  }

  SUBCASE("every truncated prefix is refused") {
    REQUIRE(net::try_encode_intent_batch(1, one_move, layouts, registry, wire) ==
            net::intent_wire_status::ok);
    for (size_t size = 0; size < wire.size(); ++size) {
      CAPTURE(size);
      const auto status =
        net::try_decode_intent_batch(std::span(wire).first(size), layouts, registry, view, decoded);
      // A header-only buffer is a legal empty batch; anything shorter, or a
      // partial intent, is refused.
      if (size == net::intent_batch_header_bytes) {
        CHECK(status == net::intent_wire_status::ok);
        CHECK(decoded.empty());
      } else {
        CHECK(status == net::intent_wire_status::truncated);
      }
    }
  }

  SUBCASE("a zero-filled buffer is not a message") {
    const std::vector<std::byte> zeros(16, std::byte(0));
    CHECK(net::try_decode_intent_batch(zeros, layouts, registry, view, decoded) ==
          net::intent_wire_status::wrong_message_type);
  }

  SUBCASE("an oversized batch is refused before parsing") {
    const std::vector<std::byte> huge(net::intent_batch_max_bytes + 1, std::byte(1));
    CHECK(net::try_decode_intent_batch(huge, layouts, registry, view, decoded) ==
          net::intent_wire_status::too_large);
  }

  SUBCASE("more intents than prepared storage is refused, not grown") {
    std::array<net::intent, 4> sent{};
    for (auto& value : sent) value.kind = kind_event;
    REQUIRE(net::try_encode_intent_batch(1, sent, layouts, registry, wire) ==
            net::intent_wire_status::ok);
    auto small = prepared_intents(2);
    CHECK(net::try_decode_intent_batch(wire, layouts, registry, view, small) ==
          net::intent_wire_status::capacity_exceeded);
    CHECK(small.capacity() == 2);
  }

  SUBCASE("an unprepared output buffer is refused instead of growing") {
    std::vector<std::byte> unprepared;
    CHECK(net::try_encode_intent_batch(1, one_move, layouts, registry, unprepared) ==
          net::intent_wire_status::buffer_too_small);
    CHECK(unprepared.capacity() == 0);
  }

  SUBCASE("a batch larger than one transport packet is refused at encode") {
    std::vector<net::intent> many(200);
    for (auto& value : many) value.kind = kind_move;
    auto big = prepared(4096);
    CHECK(net::try_encode_intent_batch(1, many, layouts, registry, big) ==
          net::intent_wire_status::too_large);
  }
}

TEST_CASE("network intent tick reconstruction survives the sixteen-bit wrap") {
  CHECK(net::intent_absolute_tick(300, 300, 0) == std::optional<uint64_t>(300));
  CHECK(net::intent_absolute_tick(300, 300, 7) == std::optional<uint64_t>(293));

  SUBCASE("low bits from the previous window resolve backwards") {
    const uint64_t receiver = 0x1'0005;
    CHECK(net::intent_absolute_tick(receiver, 0xffff, 0) ==
          std::optional<uint64_t>(0xffff));
    CHECK(net::intent_absolute_tick(receiver, 0xfff0, 2) ==
          std::optional<uint64_t>(0xfff0 - 2));
  }

  SUBCASE("low bits from the next window resolve forwards") {
    const uint64_t receiver = 0x1'fff0;
    CHECK(net::intent_absolute_tick(receiver, 3, 0) == std::optional<uint64_t>(0x2'0003));
  }

  SUBCASE("a delta reaching before the first tick has no answer") {
    CHECK_FALSE(net::intent_absolute_tick(2, 2, 5).has_value());
  }
}
