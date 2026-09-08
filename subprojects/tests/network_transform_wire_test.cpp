#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace net = devils_engine::network;

namespace {

// Cell of 64 world units, quantum of 1/1024 — the same lattice HOT-01's tests
// and NET-LAB-01's stand declare, so a figure measured here is comparable with
// the intent budget measured there.
constexpr net::fixed_axis<uint16_t> axis{6, 10};

// Project-owned class codes. The library never learns what they mean.
constexpr uint8_t class_owned = 1;  // the client's own predicted transform
constexpr uint8_t class_remote = 2; // everyone else's, interpolated
constexpr uint8_t class_refresh = 3; // the periodic full pass the age bound needs

constexpr net::transform_field_layout point_and_turn{3, true};

net::transform_layout_table make_layouts() {
  // Cadence is a FREQUENCY, and the engine ticks at 60 Hz (TIME-02), so the
  // interval in ticks is that frequency's reciprocal rather than the number
  // printed in NETWORKING.md's 20 Hz table. The phases differ deliberately:
  // NET-LAB-01 had to learn that two classes sharing a phase turn a staggered
  // schedule back into a synchronous one.
  const std::array<net::transform_class_layout, 3> declared{
    net::transform_class_layout{point_and_turn, {0, 3, 0, 6, 2,
                                                 net::transform_delivery::unreliable_sequenced},
                                class_owned},
    net::transform_class_layout{point_and_turn, {0, 6, 1, 18, 1,
                                                 net::transform_delivery::unreliable_sequenced},
                                class_remote},
    net::transform_class_layout{point_and_turn, {0, 60, 7, 0, 0,
                                                 net::transform_delivery::reliable_ordered},
                                class_refresh}};
  net::transform_layout_table table;
  REQUIRE(table.assign(declared) == net::transform_wire_status::ok);
  return table;
}

std::vector<std::byte> prepared(const size_t bytes) {
  std::vector<std::byte> buffer;
  buffer.reserve(bytes);
  return buffer;
}

std::vector<net::transform_sample> prepared_samples(const size_t count) {
  std::vector<net::transform_sample> out;
  out.reserve(count);
  return out;
}

std::vector<net::slot_change> prepared_changes(const size_t count) {
  std::vector<net::slot_change> out;
  out.reserve(count);
  return out;
}

net::transform_sample sample_at(const uint16_t slot, const int32_t key, const uint16_t code) {
  net::transform_sample value;
  value.slot = slot;
  value.key = {key, key, key};
  value.code = {code, code, code};
  value.turn = code;
  return value;
}

// One unbudgeted send opportunity carrying the whole set. How a sender paces
// its frames across the tick stays its own policy; the cost is the library's
// arithmetic, which is why the ladder below can be trusted to match the wire.
size_t pass_bytes(const net::transform_field_layout fields, const net::transform_frame_mode mode,
                  const size_t records) {
  const auto plan = net::plan_transform_send(fields, mode, records, net::cadence_policy{});
  REQUIRE(plan.complete);
  return plan.bytes;
}

} // namespace

TEST_CASE("network relevant set keeps slots dense and publishes a diff") {
  net::relevant_set set(8);
  CHECK(set.capacity() == 8);
  CHECK(set.generation() == 0);

  // Handle zero is refused: a free slot is represented by zero, so admitting it
  // would make an occupied slot indistinguishable from an empty one.
  CHECK(set.enter(0).status == net::slot_admission::invalid_handle);

  for (uint64_t handle = 10; handle < 14; ++handle) {
    const auto result = set.enter(handle);
    CHECK(result.status == net::slot_admission::assigned);
    CHECK(result.slot == uint16_t(handle - 10));
  }
  CHECK(set.enter(11).status == net::slot_admission::already_present);
  CHECK(set.enter(11).slot == 1);
  CHECK(set.size() == 4);

  auto changes = prepared_changes(16);
  CHECK(set.publish(changes) == 1);
  REQUIRE(changes.size() == 4);
  CHECK(changes[0] == net::slot_change{10, 0, net::slot_operation::enter});
  CHECK(changes[3] == net::slot_change{13, 3, net::slot_operation::enter});

  // Nothing changed, so nothing is published and the generation stands: a
  // generation which advanced for free would invalidate every frame in flight.
  CHECK(set.publish(changes) == 1);
  CHECK(changes.empty());

  // Entered and left between two publications: no wire traffic at all, which is
  // the property a change log could not offer.
  CHECK(set.enter(99).status == net::slot_admission::assigned);
  CHECK(set.leave(99));
  CHECK(set.publish(changes) == 1);
  CHECK(changes.empty());

  // The lowest free slot is reused, and one slot changing occupant travels as
  // one enter rather than a leave/enter pair whose order could matter.
  CHECK(set.leave(11));
  CHECK(set.enter(21).slot == 1);
  CHECK(set.publish(changes) == 2);
  REQUIRE(changes.size() == 1);
  CHECK(changes[0] == net::slot_change{21, 1, net::slot_operation::enter});

  CHECK(set.leave(12));
  CHECK_FALSE(set.leave(12));
  CHECK(set.publish(changes) == 3);
  REQUIRE(changes.size() == 1);
  CHECK(changes[0] == net::slot_change{0, 2, net::slot_operation::leave});
}

TEST_CASE("network relevant set reports an exhausted slot budget") {
  // The declared budget is visible before and after construction, because a
  // constructor cannot refuse and a silently narrowed set would drop entities a
  // relevance function believed it had admitted.
  CHECK(net::relevant_set_capacity_fits(net::relevant_set_max_slots));
  CHECK_FALSE(net::relevant_set_capacity_fits(net::relevant_set_max_slots + 1));
  CHECK(net::relevant_set(uint16_t(net::relevant_set_max_slots + 1)).capacity() ==
        net::relevant_set_max_slots);
  CHECK(net::relevant_set_mirror(uint16_t(net::relevant_set_max_slots + 1)).capacity() ==
        net::relevant_set_max_slots);

  net::relevant_set set(2);
  CHECK(set.enter(1).status == net::slot_admission::assigned);
  CHECK(set.enter(2).status == net::slot_admission::assigned);
  // A relevance function which grew without bound says so instead of quietly
  // dropping the entity a client needed most.
  CHECK(set.enter(3).status == net::slot_admission::capacity_exhausted);
  CHECK(set.size() == 2);
}

TEST_CASE("network relevant set update survives its wire trip") {
  net::relevant_set set(8);
  for (uint64_t handle = 10; handle < 14; ++handle) REQUIRE(set.enter(handle).usable());
  auto changes = prepared_changes(16);
  const auto first_generation = set.publish(changes);
  REQUIRE(changes.size() == 4);

  net::relevant_set_mirror mirror(8);
  auto bytes = prepared(net::relevant_set_update_max_bytes);
  REQUIRE(net::try_encode_relevant_set_update(first_generation, changes,
                                             net::relevant_set_scope::diff, bytes) ==
          net::transform_wire_status::ok);
  net::relevant_set_update_view view;
  auto decoded = prepared_changes(16);
  REQUIRE(net::try_decode_relevant_set_update(bytes, view, decoded) ==
          net::transform_wire_status::ok);
  REQUIRE(mirror.apply(view.generation, decoded) == net::transform_wire_status::ok);
  CHECK(mirror.size() == 4);

  // Two slots freed and one refilled: the lowest free slot is reused, so the
  // update carries one enter and one leave, and their widths differ on the wire.
  REQUIRE(set.leave(10));
  REQUIRE(set.leave(11));
  CHECK(set.enter(30).slot == 0);
  const auto generation = set.publish(changes);
  CHECK(generation == uint16_t(first_generation + 1));
  REQUIRE(changes.size() == 2);
  CHECK(changes[0] == net::slot_change{30, 0, net::slot_operation::enter});
  CHECK(changes[1] == net::slot_change{0, 1, net::slot_operation::leave});

  REQUIRE(net::try_encode_relevant_set_update(generation, changes,
                                             net::relevant_set_scope::diff, bytes) ==
          net::transform_wire_status::ok);
  CHECK(bytes.size() == net::relevant_set_update_bytes(changes));
  CHECK(bytes.size() == 3 + 11 + 3);

  REQUIRE(net::try_decode_relevant_set_update(bytes, view, decoded) ==
          net::transform_wire_status::ok);
  CHECK(view.generation == generation);
  CHECK(view.count == 2);
  CHECK(decoded == changes);

  REQUIRE(mirror.apply(view.generation, decoded) == net::transform_wire_status::ok);
  CHECK(mirror.generation() == generation);
  CHECK(mirror.size() == 3);
  CHECK(mirror.handle_at(0) == 30);
  CHECK_FALSE(mirror.occupied(1));

  // The mirror's table equals the sender's, slot for slot, by construction
  // rather than by careful bookkeeping.
  for (uint16_t slot = 0; slot < set.capacity(); ++slot)
    CHECK(mirror.handle_at(slot) == set.handle_at(slot));

  SUBCASE("a truncated record is refused whole") {
    auto clipped = bytes;
    clipped.pop_back();
    CHECK(net::try_decode_relevant_set_update(clipped, view, decoded) ==
          net::transform_wire_status::truncated);
    // Nothing of a refused message reaches the caller: membership is applied
    // whole or not at all.
    CHECK(decoded.empty());
    CHECK(view.count == 0);
    CHECK(decoded.capacity() == 16);
  }

  SUBCASE("a handle of zero cannot travel in either direction") {
    const std::array<net::slot_change, 1> forged{
      net::slot_change{0, 2, net::slot_operation::enter}};
    CHECK(net::try_encode_relevant_set_update(generation, forged,
                                            net::relevant_set_scope::diff, bytes) ==
          net::transform_wire_status::invalid_layout);
    auto tampered = prepared(net::relevant_set_update_max_bytes);
    const std::array<net::slot_change, 1> honest{
      net::slot_change{7, 2, net::slot_operation::enter}};
    REQUIRE(net::try_encode_relevant_set_update(generation, honest,
                                              net::relevant_set_scope::diff, tampered) ==
            net::transform_wire_status::ok);
    for (size_t index = 3 + 2; index < tampered.size(); ++index)
      tampered[index] = std::byte(0);
    CHECK(net::try_decode_relevant_set_update(tampered, view, decoded) ==
          net::transform_wire_status::invalid_layout);
  }

  SUBCASE("a gap in the reliable lane is reported, not absorbed") {
    net::relevant_set_mirror fresh(8);
    // The membership lane is reliable and ordered, so the only generation this
    // mirror can legitimately be handed is the next one.
    CHECK(fresh.apply(uint16_t(view.generation + 1), decoded) ==
          net::transform_wire_status::generation_gap);
    CHECK(fresh.generation() == 0);
    CHECK(fresh.size() == 0);
  }

  SUBCASE("a reconnecting client adopts the full set at its generation") {
    // A restarted process comes back with an empty mirror at generation zero,
    // so a diff is useless to it: the only thing it can apply is the whole set
    // at whatever generation the authority has reached. Byte zero says which of
    // the two this message is, because they are read differently.
    net::relevant_set_mirror rejoined(8);
    auto full = prepared_changes(16);
    set.publish_full(full);
    REQUIRE(full.size() == 3);

    auto full_bytes = prepared(net::relevant_set_update_max_bytes);
    REQUIRE(net::try_encode_relevant_set_update(set.generation(), full,
                                                net::relevant_set_scope::full, full_bytes) ==
            net::transform_wire_status::ok);
    net::relevant_set_update_view full_view;
    auto full_decoded = prepared_changes(16);
    REQUIRE(net::try_decode_relevant_set_update(full_bytes, full_view, full_decoded) ==
            net::transform_wire_status::ok);
    CHECK(full_view.scope == net::relevant_set_scope::full);
    CHECK(full_view.generation == set.generation());

    // The diff would be refused by this mirror, and that refusal is the reason
    // the full scope has to exist rather than being a convenience.
    CHECK(rejoined.apply(full_view.generation, full_decoded) ==
          net::transform_wire_status::generation_gap);
    CHECK(rejoined.adopt(full_view.generation, full_decoded) == net::transform_wire_status::ok);
    CHECK(rejoined.generation() == set.generation());
    CHECK(rejoined.size() == set.size());
    for (uint16_t slot = 0; slot < set.capacity(); ++slot)
      CHECK(rejoined.handle_at(slot) == set.handle_at(slot));

    // An empty diff says nothing; an empty full set says "you are relevant to
    // nothing", which a receiver must be able to hear.
    auto empty = prepared(net::relevant_set_update_max_bytes);
    CHECK(net::try_encode_relevant_set_update(9, {}, net::relevant_set_scope::diff, empty) ==
          net::transform_wire_status::empty_frame);
    REQUIRE(net::try_encode_relevant_set_update(9, {}, net::relevant_set_scope::full, empty) ==
            net::transform_wire_status::ok);
    REQUIRE(net::try_decode_relevant_set_update(empty, full_view, full_decoded) ==
            net::transform_wire_status::ok);
    CHECK(full_view.scope == net::relevant_set_scope::full);
    CHECK(full_view.count == 0);
    CHECK(rejoined.adopt(full_view.generation, full_decoded) == net::transform_wire_status::ok);
    CHECK(rejoined.size() == 0);
  }
}

TEST_CASE("network transform frame survives its wire trip in both modes") {
  const auto layouts = make_layouts();
  net::relevant_set set(16);
  for (uint64_t handle = 100; handle < 108; ++handle) REQUIRE(set.enter(handle).usable());
  auto changes = prepared_changes(32);
  const auto generation = set.publish(changes);
  net::relevant_set_mirror mirror(16);
  REQUIRE(mirror.apply(generation, changes) == net::transform_wire_status::ok);

  const net::transform_frame_origin origin{{1000, -4, 7}};
  const std::array<net::transform_sample, 4> dense{
    sample_at(2, 1000, 0), sample_at(3, 1001, 1023), sample_at(4, 999, 512),
    sample_at(5, 1127, 65535)};
  // Cells are carried relative to the frame's origin per axis, so the sample's
  // own key must survive a base that differs on every axis.
  std::array<net::transform_sample, 4> expected = dense;
  for (auto& value : expected) value.key = {value.key[0], -4 + (value.key[0] - 1000),
                                            7 + (value.key[0] - 1000)};
  std::array<net::transform_sample, 4> sent = expected;

  auto bytes = prepared(net::transform_frame_max_bytes);
  REQUIRE(net::try_encode_transform_frame(class_remote, 70000, generation,
                                          net::transform_frame_mode::dense, origin, sent,
                                          layouts, bytes) == net::transform_wire_status::ok);
  CHECK(bytes.size() == net::transform_frame_bytes(point_and_turn,
                                                   net::transform_frame_mode::dense, 4));
  CHECK(bytes.size() == 6 + 3 * 4 + 2 + 4 * (3 * 3 + 2));

  net::transform_frame_view view;
  auto decoded = prepared_samples(64);
  REQUIRE(net::try_decode_transform_frame(bytes, layouts, mirror, 70003, view, decoded) ==
          net::transform_wire_status::ok);
  CHECK(view.class_id == class_remote);
  CHECK(view.mode == net::transform_frame_mode::dense);
  CHECK(view.generation == generation);
  // The tick is widened against the receiver's own progress, so the frame needs
  // only its low sixteen bits.
  CHECK(view.tick == 70000);
  CHECK(view.count == 4);
  REQUIRE(decoded.size() == 4);
  for (size_t index = 0; index < 4; ++index) CHECK(decoded[index] == expected[index]);

  SUBCASE("sparse mode carries the slot and costs two bytes more per record") {
    const std::array<net::transform_sample, 3> scattered{sent[0], sent[2], sent[3]};
    auto sparse_bytes = prepared(net::transform_frame_max_bytes);
    REQUIRE(net::try_encode_transform_frame(class_remote, 70000, generation,
                                            net::transform_frame_mode::sparse, origin,
                                            scattered, layouts, sparse_bytes) ==
            net::transform_wire_status::ok);
    CHECK(sparse_bytes.size() == 6 + 3 * 4 + 3 * (2 + 3 * 3 + 2));
    REQUIRE(net::try_decode_transform_frame(sparse_bytes, layouts, mirror, 70000, view,
                                            decoded) == net::transform_wire_status::ok);
    CHECK(view.mode == net::transform_frame_mode::sparse);
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0] == expected[0]);
    CHECK(decoded[1] == expected[2]);
    CHECK(decoded[2] == expected[3]);
  }

  SUBCASE("a frame built against another set generation is refused both ways") {
    // The membership lane and the frame lane are different lanes, so either can
    // arrive first. Neither is guessed.
    auto ahead = prepared(net::transform_frame_max_bytes);
    REQUIRE(net::try_encode_transform_frame(class_remote, 70000,
                                            uint16_t(generation + 1),
                                            net::transform_frame_mode::dense, origin, sent,
                                            layouts, ahead) == net::transform_wire_status::ok);
    CHECK(net::try_decode_transform_frame(ahead, layouts, mirror, 70000, view, decoded) ==
          net::transform_wire_status::generation_mismatch);
    auto behind = prepared(net::transform_frame_max_bytes);
    REQUIRE(net::try_encode_transform_frame(class_remote, 70000,
                                            uint16_t(generation - 1),
                                            net::transform_frame_mode::dense, origin, sent,
                                            layouts, behind) == net::transform_wire_status::ok);
    CHECK(net::try_decode_transform_frame(behind, layouts, mirror, 70000, view, decoded) ==
          net::transform_wire_status::generation_mismatch);
    CHECK(decoded.empty());
  }

  SUBCASE("a slot the receiver's set does not occupy is refused at the boundary") {
    net::relevant_set set_without_five(16);
    for (uint64_t handle = 100; handle < 108; ++handle)
      REQUIRE(set_without_five.enter(handle).usable());
    REQUIRE(set_without_five.leave(105));
    auto reduced = prepared_changes(32);
    const auto reduced_generation = set_without_five.publish(reduced);
    net::relevant_set_mirror narrow(16);
    REQUIRE(narrow.apply(reduced_generation, reduced) == net::transform_wire_status::ok);
    auto same_generation = prepared(net::transform_frame_max_bytes);
    REQUIRE(net::try_encode_transform_frame(class_remote, 70000, reduced_generation,
                                            net::transform_frame_mode::dense, origin, sent,
                                            layouts, same_generation) ==
            net::transform_wire_status::ok);
    CHECK(net::try_decode_transform_frame(same_generation, layouts, narrow, 70000, view,
                                          decoded) == net::transform_wire_status::unknown_slot);
  }

  SUBCASE("a payload which is not a whole number of records is truncated") {
    auto clipped = bytes;
    clipped.pop_back();
    CHECK(net::try_decode_transform_frame(clipped, layouts, mirror, 70000, view, decoded) ==
          net::transform_wire_status::truncated);
    CHECK(decoded.empty());
  }

  SUBCASE("decoding never grows the caller's storage") {
    auto small = prepared_samples(3);
    CHECK(net::try_decode_transform_frame(bytes, layouts, mirror, 70000, view, small) ==
          net::transform_wire_status::capacity_exceeded);
    CHECK(small.capacity() == 3);
  }

  SUBCASE("the reserved bit and an undeclared class are refused") {
    auto tampered = bytes;
    tampered[1] |= std::byte(0x80);
    CHECK(net::try_decode_transform_frame(tampered, layouts, mirror, 70000, view, decoded) ==
          net::transform_wire_status::reserved_bit_set);
    tampered = bytes;
    tampered[1] = std::byte(9);
    CHECK(net::try_decode_transform_frame(tampered, layouts, mirror, 70000, view, decoded) ==
          net::transform_wire_status::unknown_class);
    tampered = bytes;
    tampered[0] = std::byte(uint8_t(net::hot_message_type::intent_batch));
    CHECK(net::try_decode_transform_frame(tampered, layouts, mirror, 70000, view, decoded) ==
          net::transform_wire_status::wrong_message_type);
  }
}

TEST_CASE("network transform frame refuses what a sender must not produce") {
  const auto layouts = make_layouts();
  net::relevant_set_mirror mirror(16);
  auto bytes = prepared(net::transform_frame_max_bytes);
  const net::transform_frame_origin origin{{0, 0, 0}};

  const std::array<net::transform_sample, 1> one{sample_at(0, 0, 0)};
  CHECK(net::try_encode_transform_frame(9, 1, 1, net::transform_frame_mode::dense, origin, one,
                                        layouts, bytes) == net::transform_wire_status::unknown_class);

  // A latest-value observation with nothing to observe is not sent. An intent
  // bundle's explicit empty case says "this tick is closed"; a transform frame
  // has no such statement to make.
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::dense,
                                        origin, {}, layouts, bytes) ==
        net::transform_wire_status::empty_frame);

  const std::array<net::transform_sample, 2> descending{sample_at(4, 0, 0), sample_at(2, 0, 0)};
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::sparse,
                                        origin, descending, layouts, bytes) ==
        net::transform_wire_status::slots_not_ascending);

  const std::array<net::transform_sample, 2> gapped{sample_at(2, 0, 0), sample_at(4, 0, 0)};
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::dense,
                                        origin, gapped, layouts, bytes) ==
        net::transform_wire_status::dense_slots_not_consecutive);
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::sparse,
                                        origin, gapped, layouts, bytes) ==
        net::transform_wire_status::ok);

  // Out of reach means the entity is further from the frame's origin than the
  // format can express, which is further than a relevance function should ever
  // have admitted. It names a relevance fault instead of teleporting anyone.
  const std::array<net::transform_sample, 1> distant{sample_at(0, 128, 0)};
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::dense,
                                        origin, distant, layouts, bytes) ==
        net::transform_wire_status::cell_out_of_reach);
  // Reach is checked while encoding, so a refusal must take its own bytes back:
  // a caller handed a truncated frame would send a message its own encoder
  // rejected.
  CHECK(bytes.empty());
  const std::array<net::transform_sample, 1> reachable{sample_at(0, 127, 0)};
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::dense,
                                        origin, reachable, layouts, bytes) ==
        net::transform_wire_status::ok);
  CHECK(axis.cell_size() * 127.0 == 8128.0);

  auto unprepared = prepared(8);
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::dense,
                                        origin, reachable, layouts, unprepared) ==
        net::transform_wire_status::buffer_too_small);

  // One frame must stay inside one packet, so a set larger than one frame is
  // several frames rather than one grown frame.
  const size_t capacity =
    net::transform_frame_capacity(point_and_turn, net::transform_frame_mode::dense);
  CHECK(capacity == (net::transform_frame_max_bytes - 20) / 11);
  auto oversized = prepared_samples(capacity + 1);
  for (size_t index = 0; index <= capacity; ++index)
    oversized.push_back(sample_at(uint16_t(index), 0, 0));
  auto wide = prepared(net::transform_frame_max_bytes * 2);
  CHECK(net::try_encode_transform_frame(class_remote, 1, 1, net::transform_frame_mode::dense,
                                        origin, oversized, layouts, wide) ==
        net::transform_wire_status::too_large);
}

TEST_CASE("network transform send plan makes the declared byte budget bite") {
  constexpr size_t capacity =
    (net::transform_frame_max_bytes - 20) / 11; // dense header 20, record 11

  SUBCASE("without a budget the whole set is planned, in as many frames as it needs") {
    const auto plan = net::plan_transform_send(point_and_turn, net::transform_frame_mode::dense,
                                               capacity + 5, net::cadence_policy{});
    CHECK(plan.complete);
    CHECK(plan.frames == 2);
    CHECK(plan.records == capacity + 5);
    CHECK(plan.bytes == net::transform_frame_bytes(point_and_turn,
                                                   net::transform_frame_mode::dense, capacity) +
                          net::transform_frame_bytes(point_and_turn,
                                                     net::transform_frame_mode::dense, 5));
    for (size_t frames = 0; frames < plan.frames; ++frames)
      CHECK(net::transform_frame_bytes(point_and_turn, net::transform_frame_mode::dense,
                                       capacity) <= net::transform_frame_max_bytes);
  }

  SUBCASE("a budget short of the set truncates it and says so") {
    net::cadence_policy policy;
    policy.byte_budget = 300;
    const auto plan = net::plan_transform_send(point_and_turn, net::transform_frame_mode::dense,
                                               capacity, policy);
    // A latest-value class loses nothing by carrying fewer entities this
    // opportunity, but the sender must know it did not fit: choosing who is cut
    // is the project's decision, which is what priority exists for.
    CHECK_FALSE(plan.complete);
    CHECK(plan.frames == 1);
    CHECK(plan.records == (300 - 20) / 11);
    CHECK(plan.bytes <= policy.byte_budget);
  }

  SUBCASE("a budget which cannot pay for one record buys no frame at all") {
    net::cadence_policy policy;
    policy.byte_budget = 25; // a dense header is 20, a record 11
    const auto plan = net::plan_transform_send(point_and_turn, net::transform_frame_mode::dense,
                                               4, policy);
    CHECK(plan == net::transform_send_plan{});
    CHECK_FALSE(plan.complete);
  }

  SUBCASE("nothing pending plans nothing, and that counts as complete") {
    const auto plan = net::plan_transform_send(point_and_turn, net::transform_frame_mode::dense,
                                               0, net::cadence_policy{});
    CHECK(plan.frames == 0);
    CHECK(plan.bytes == 0);
    CHECK(plan.complete);
  }
}

TEST_CASE("network transform classes declare shape and cadence separately") {
  const auto layouts = make_layouts();

  CHECK(layouts.fields(class_remote).record_bytes(false) == 11);
  CHECK(layouts.fields(class_remote).record_bytes(true) == 13);
  CHECK_FALSE((net::transform_field_layout{0, false}).valid());
  CHECK_FALSE((net::transform_field_layout{4, true}).valid());
  CHECK((net::transform_field_layout{0, true}).valid());

  // Priority decides who goes first when one tick cannot afford every class;
  // the class id breaks the tie so two peers walk the classes in one order.
  const auto ordered = layouts.classes();
  REQUIRE(ordered.size() == 3);
  CHECK(ordered[0] == class_owned);
  CHECK(ordered[1] == class_remote);
  CHECK(ordered[2] == class_refresh);

  std::array<uint8_t, 4> due{};
  CHECK(layouts.due_classes(0, due) == 1);
  CHECK(due[0] == class_owned);
  CHECK(layouts.due_classes(1, due) == 1);
  CHECK(due[0] == class_remote);
  CHECK(layouts.due_classes(2, due) == 0);
  CHECK(layouts.due_classes(3, due) == 1);
  CHECK(due[0] == class_owned);
  CHECK(layouts.due_classes(7, due) == 2); // the refresh shares tick 7 with the remote class
  CHECK(due[0] == class_remote);
  CHECK(due[1] == class_refresh);

  // A staleness bound below the interval would be exceeded on a perfect link,
  // which makes it a declaration error rather than a strict policy.
  net::transform_layout_table rejected;
  const std::array<net::transform_class_layout, 1> impossible{
    net::transform_class_layout{point_and_turn, {0, 6, 0, 3, 0}, class_remote}};
  CHECK(rejected.assign(impossible) == net::transform_wire_status::invalid_cadence);
  const std::array<net::transform_class_layout, 1> phase_past_interval{
    net::transform_class_layout{point_and_turn, {0, 3, 3, 0, 0}, class_remote}};
  CHECK(rejected.assign(phase_past_interval) == net::transform_wire_status::invalid_cadence);
  const std::array<net::transform_class_layout, 2> twice{
    net::transform_class_layout{point_and_turn, {0, 3, 0, 0, 0}, class_remote},
    net::transform_class_layout{point_and_turn, {0, 6, 0, 0, 0}, class_remote}};
  CHECK(rejected.assign(twice) == net::transform_wire_status::duplicate_class);

  SUBCASE("the fingerprint covers the shape and deliberately not the cadence") {
    net::transform_layout_table faster;
    const std::array<net::transform_class_layout, 3> adapted{
      net::transform_class_layout{point_and_turn, {0, 1, 0, 6, 2}, class_owned},
      net::transform_class_layout{point_and_turn, {0, 2, 1, 18, 1}, class_remote},
      net::transform_class_layout{point_and_turn, {0, 30, 7, 0, 0}, class_refresh}};
    REQUIRE(faster.assign(adapted) == net::transform_wire_status::ok);
    // An adaptive sender stays compatible: correctness must not depend on
    // receiving any particular frame.
    CHECK(faster.shape_fingerprint() == layouts.shape_fingerprint());

    net::transform_layout_table reshaped;
    const std::array<net::transform_class_layout, 3> without_turn{
      net::transform_class_layout{{3, false}, {0, 3, 0, 6, 2}, class_owned},
      net::transform_class_layout{point_and_turn, {0, 6, 1, 18, 1}, class_remote},
      net::transform_class_layout{point_and_turn, {0, 60, 7, 0, 0}, class_refresh}};
    REQUIRE(reshaped.assign(without_turn) == net::transform_wire_status::ok);
    // A differing shape is a silent misreading of every record, so it belongs
    // in the session's numeric profile and must refuse the peer.
    CHECK(reshaped.shape_fingerprint() != layouts.shape_fingerprint());
  }
}

TEST_CASE("network transform gate keeps the latest value per class") {
  const auto layouts = make_layouts();
  const auto policy = layouts.cadence(class_remote);
  net::transform_frame_gate gate;
  CHECK_FALSE(gate.newest().has_value());
  // No frame has ever arrived, so a class with a declared bound is already past
  // it: presentation must not extrapolate from nothing.
  CHECK(gate.exceeded_max_age(0, policy));

  CHECK(gate.commit(100) == net::transform_frame_acceptance::accepted);
  CHECK(gate.commit(100) == net::transform_frame_acceptance::duplicate);
  CHECK(gate.commit(94) == net::transform_frame_acceptance::stale);
  CHECK(gate.commit(106) == net::transform_frame_acceptance::accepted);
  // A jump forward is accepted, and that is the point: a receiver which fell
  // behind through loss or a reconnect must not be able to freeze the class by
  // refusing everything that follows. NET-LAB-01 froze one for 91 consecutive
  // frames when this gate still had a forward window.
  CHECK(gate.commit(400) == net::transform_frame_acceptance::accepted);
  CHECK(gate.newest() == 400);
  CHECK(gate.commit(399) == net::transform_frame_acceptance::stale);
  gate.reset();
  CHECK_FALSE(gate.newest().has_value());
  CHECK(gate.commit(106) == net::transform_frame_acceptance::accepted);

  CHECK_FALSE(gate.exceeded_max_age(106 + policy.max_age_ticks, policy));
  CHECK(gate.exceeded_max_age(107 + policy.max_age_ticks, policy));
  CHECK_FALSE(gate.exceeded_max_age(100, policy));
  // A class which declared no bound never reports one.
  CHECK_FALSE(gate.exceeded_max_age(100000, layouts.cadence(class_refresh)));
}

TEST_CASE("network correction threshold is exactly the declared quantum") {
  constexpr net::correction_policy policy{net::minimum_correction_codes};
  REQUIRE(policy.valid());
  CHECK(policy.world_tolerance(axis) == axis.quantum());
  CHECK_FALSE((net::correction_policy{0}).valid());

  const auto authoritative = sample_at(0, 3, 700);
  const double world = net::join_axis(axis, 3, 700);

  // Exact agreement, and agreement inside the dead band the format itself
  // created: snapping onto a quantized value injects up to half a quantum even
  // when nothing diverged.
  for (const double offset : {0.0, 0.4 * axis.quantum(), -0.4 * axis.quantum(),
                              1.0 * axis.quantum(), -1.0 * axis.quantum()}) {
    const std::array<double, 3> predicted{world + offset, world, world};
    const auto verdict = net::evaluate_correction(axis, 3, predicted, authoritative, policy);
    CAPTURE(offset);
    CHECK_FALSE(verdict.needs_correction);
  }

  // Two quanta out is a divergence, and the verdict names the axis so that a
  // correction is diagnosable rather than merely visible.
  const std::array<double, 3> diverged{world, world, world + 2.0 * axis.quantum()};
  const auto verdict = net::evaluate_correction(axis, 3, diverged, authoritative, policy);
  CHECK(verdict.needs_correction);
  CHECK(verdict.axis == 2);
  CHECK(verdict.worst_error_codes == 2);

  // The error is exact integer arithmetic in code space, so it stays exact at a
  // distance where a world-space epsilon would not.
  const int32_t distant_key = 1 << 20;
  const auto far_sample = sample_at(0, distant_key, 0);
  const double far_world = net::join_axis(axis, distant_key, 0);
  CHECK(net::axis_error_codes(axis, far_world + 3.0 * axis.quantum(), distant_key, 0) == 3);
  const std::array<double, 3> far_predicted{far_world + 3.0 * axis.quantum(), far_world,
                                            far_world};
  const auto far_verdict = net::evaluate_correction(axis, 3, far_predicted, far_sample,
                                                    net::correction_policy{2});
  CHECK(far_verdict.needs_correction);
  CHECK(far_verdict.worst_error_codes == 3);
  CHECK(far_verdict.axis == 0);
}

// The measurement HOT-02 exists for. The profile below is DECLARED, not
// observed: it is the project's target shape (a party-scale co-op session in a
// loaded neighbourhood), and every figure is the codec's own arithmetic at that
// shape. What is being proved is the ORDER of the levers, which is why each
// rung of the ladder removes exactly one of them.
TEST_CASE("network transform budget per client is measured, not assumed") {
  const auto layouts = make_layouts();
  constexpr size_t tick_rate = 60;         // TIME-02's fixed step
  constexpr size_t world_entities = 2048;  // simulated in the loaded neighbourhood
  constexpr size_t relevant = 96;          // what one client is told about
  constexpr size_t moving = 38;            // ~40% actually changed since the last send
  constexpr size_t membership_changes_per_second = 8;

  // Rung 0: no relevance, no cadence, no shared origin. Every entity, every
  // tick, addressed by its 64-bit handle and carrying absolute cell keys.
  constexpr size_t naive_record = 8 + 3 * (4 + 2) + 2;
  const size_t rung_naive = world_entities * naive_record * tick_rate;

  // Rung 1: relevance alone.
  const size_t rung_relevant = relevant * naive_record * tick_rate;

  // Rung 2: the dense slot and the frame-relative cell — 11 bytes a record
  // instead of 28, with the origin paid once per frame.
  const size_t rung_addressed =
    pass_bytes(point_and_turn, net::transform_frame_mode::dense, relevant) * tick_rate;

  // Rung 3: cadence. One owned entity at 20 Hz, the rest at 10 Hz.
  const size_t owned_bytes =
    pass_bytes(point_and_turn, net::transform_frame_mode::dense, 1) *
    (tick_rate / layouts.cadence(class_owned).interval_ticks);
  const size_t remote_bytes =
    pass_bytes(point_and_turn, net::transform_frame_mode::dense, relevant - 1) *
    (tick_rate / layouts.cadence(class_remote).interval_ticks);
  const size_t rung_cadenced = owned_bytes + remote_bytes;

  // Rung 4: send only what changed, in sparse frames, and keep one full dense
  // pass per second because the declared age bound demands it.
  const size_t changed_bytes =
    pass_bytes(point_and_turn, net::transform_frame_mode::sparse, moving) *
    (tick_rate / layouts.cadence(class_remote).interval_ticks);
  const size_t refresh_bytes =
    pass_bytes(point_and_turn, net::transform_frame_mode::dense, relevant) *
    (tick_rate / layouts.cadence(class_refresh).interval_ticks);
  const size_t membership_bytes =
    net::relevant_set_update_header_bytes + membership_changes_per_second * 11;
  const size_t rung_delta = owned_bytes + changed_bytes + refresh_bytes + membership_bytes;

  MESSAGE("transform budget, bytes per second per client (60 Hz, 96 relevant of 2048):");
  MESSAGE("  0 everything, every tick, by handle : " << rung_naive);
  MESSAGE("  1 + relevance                       : " << rung_relevant);
  MESSAGE("  2 + dense slot and frame origin     : " << rung_addressed);
  MESSAGE("  3 + declared cadence                : " << rung_cadenced);
  MESSAGE("  4 + change-only sparse frames       : " << rung_delta);
  MESSAGE("  membership share of rung 4          : " << membership_bytes);

  // Each lever is worth strictly more than nothing, and they compose.
  CHECK(rung_relevant < rung_naive);
  CHECK(rung_addressed < rung_relevant);
  CHECK(rung_cadenced < rung_addressed);
  CHECK(rung_delta < rung_cadenced);

  // The order of the levers is the result. Relevance and cadence each buy more
  // than a factor of five; addressing and the change set buy less than three
  // between them, which is the opposite of where encoding effort usually goes.
  CHECK(rung_naive / rung_relevant == world_entities / relevant);
  CHECK(rung_relevant / rung_addressed >= 2);
  CHECK(rung_addressed / rung_cadenced >= 5);
  CHECK(rung_cadenced / rung_delta >= 1);
  CHECK(rung_cadenced / rung_delta < 2);

  // Where the two modes cross, measured rather than assumed. A sparse record
  // costs 13 bytes against a dense record's 11, so sending only what changed
  // wins until the change set is most of the run — which means the dense mode
  // earns its place only on the genuinely full pass the age bound demands, and
  // not on the per-tick traffic where a "keyframe" instinct would put it.
  size_t sparse_cheaper_up_to = 0;
  const size_t full_dense_pass = pass_bytes(point_and_turn, net::transform_frame_mode::dense,
                                            relevant);
  for (size_t changed = 1; changed <= relevant; ++changed)
    if (pass_bytes(point_and_turn, net::transform_frame_mode::sparse, changed) < full_dense_pass)
      sparse_cheaper_up_to = changed;
  MESSAGE("  sparse wins up to (of " << relevant << " relevant): " << sparse_cheaper_up_to);
  CHECK(sparse_cheaper_up_to == 81);

  // A party-scale session on one home uplink: four clients at the final rung.
  CHECK(rung_delta * 4 < 64 * 1024);

  // Every frame in rungs 2-4 fits one packet, which is what makes the whole
  // ladder legitimate: none of it depends on fragmentation.
  CHECK(net::transform_frame_bytes(point_and_turn, net::transform_frame_mode::dense,
                                   net::transform_frame_capacity(
                                     point_and_turn, net::transform_frame_mode::dense)) <=
        net::transform_frame_max_bytes);
  CHECK(net::transform_frame_bytes(point_and_turn, net::transform_frame_mode::sparse,
                                   net::transform_frame_capacity(
                                     point_and_turn, net::transform_frame_mode::sparse)) <=
        net::transform_frame_max_bytes);
}
