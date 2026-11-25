#ifndef DEVILS_ENGINE_NETWORK_CANONICAL_TICK_WIRE_H
#define DEVILS_ENGINE_NETWORK_CANONICAL_TICK_WIRE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "intent_wire.h"

// One reliable ordered message closes one simulation tick, including a tick
// with no accepted external actions. Payload bytes belong to the project;
// this envelope owns their tick, origin and bounded framing. The optional root
// is the causal state AFTER stepping this tick, never a transform-frame digest.
//
// The tick travels as its low 16 bits and is widened against the receiver's
// own tick, as in every hot class. The lane is reliable and ordered and every
// tick has a bundle, so the receiver always knows the previous tick: the field
// is a check, not information, and 16 bits keep that check exact while saving
// six bytes on every tick of every peer. Tick 0 is not a step and is refused.
//
// Records are strictly ascending by (origin_peer, input_sequence): the order is
// part of the format (one tick has one byte form), the sealed tick_journal
// already produces it, and both encoder and decoder refuse anything else
// rather than normalize it.
namespace devils_engine::network {

inline constexpr size_t canonical_tick_max_bytes = 1024;
inline constexpr size_t canonical_tick_max_records = 32;
inline constexpr size_t canonical_tick_max_payload_bytes = 256;
inline constexpr size_t canonical_tick_header_bytes = 1 + 2 + 1 + 1;
inline constexpr size_t canonical_tick_root_bytes = 8;
inline constexpr size_t canonical_tick_record_header_bytes = 8 + 8 + 2;

// Encoded size of a tick without records, and what one record adds: the same
// arithmetic the encoder applies, for producers that plan a tick before encoding it.
[[nodiscard]] constexpr size_t canonical_tick_base_bytes(const bool with_root) noexcept {
  return canonical_tick_header_bytes + (with_root ? canonical_tick_root_bytes : 0);
}
[[nodiscard]] constexpr size_t canonical_tick_record_bytes(const size_t payload_bytes) noexcept {
  return canonical_tick_record_header_bytes + payload_bytes;
}
// How many records of one payload size a tick holds under both budgets.
[[nodiscard]] constexpr size_t canonical_tick_record_capacity(const bool with_root,
                                                              const size_t payload_bytes) noexcept {
  const size_t by_bytes =
    (canonical_tick_max_bytes - canonical_tick_base_bytes(with_root)) / canonical_tick_record_bytes(payload_bytes);
  return by_bytes < canonical_tick_max_records ? by_bytes : canonical_tick_max_records;
}

// Running account of a tick under construction: `take` admits a record only if
// the encoded tick stays within canonical_tick_max_records and canonical_tick_max_bytes.
class canonical_tick_budget {
public:
  explicit constexpr canonical_tick_budget(const bool with_root) noexcept
    : bytes_(canonical_tick_base_bytes(with_root)) {}
  [[nodiscard]] constexpr bool fits(const size_t payload_bytes) const noexcept {
    return records_ < canonical_tick_max_records && payload_bytes <= canonical_tick_max_payload_bytes &&
           canonical_tick_record_bytes(payload_bytes) <= canonical_tick_max_bytes - bytes_;
  }
  // Refusal leaves the account unchanged.
  [[nodiscard]] constexpr bool take(const size_t payload_bytes) noexcept {
    if (!fits(payload_bytes)) return false;
    ++records_;
    bytes_ += canonical_tick_record_bytes(payload_bytes);
    return true;
  }
  [[nodiscard]] constexpr size_t records() const noexcept { return records_; }
  [[nodiscard]] constexpr size_t bytes() const noexcept { return bytes_; }

private:
  size_t records_ = 0;
  size_t bytes_ = 0;
};

struct canonical_tick_record_view {
  uint64_t origin_peer = 0; // logical peer within the session, not a socket handle
  uint64_t input_sequence = 0; // full sequence; zero is reserved
  std::span<const std::byte> payload;
};

struct canonical_tick_view {
  uint64_t tick = 0;
  std::optional<uint64_t> state_root;
  std::span<const canonical_tick_record_view> records;
};

enum class canonical_tick_wire_status : uint8_t {
  ok,
  buffer_too_small,
  too_large,
  too_many_records,
  truncated,
  wrong_message_type,
  invalid_header,
  invalid_record,
  duplicate_input,
  capacity_exceeded,
  trailing_bytes,
  unsorted_input,
};

namespace detail {

[[nodiscard]] inline bool canonical_record_valid(const canonical_tick_record_view& value) noexcept {
  return value.origin_peer != 0 && value.input_sequence != 0 && !value.payload.empty() &&
         value.payload.size() <= canonical_tick_max_payload_bytes;
}

// Record `index` against its predecessor: equal pair = duplicate, descending = unsorted.
[[nodiscard]] inline canonical_tick_wire_status canonical_input_order(
  const std::span<const canonical_tick_record_view> records, const size_t index) noexcept {
  if (index == 0) return canonical_tick_wire_status::ok;
  const auto& prev = records[index - 1];
  const auto& cur = records[index];
  if (prev.origin_peer == cur.origin_peer && prev.input_sequence == cur.input_sequence)
    return canonical_tick_wire_status::duplicate_input;
  if (prev.origin_peer > cur.origin_peer ||
      (prev.origin_peer == cur.origin_peer && prev.input_sequence > cur.input_sequence))
    return canonical_tick_wire_status::unsorted_input;
  return canonical_tick_wire_status::ok;
}

} // namespace detail

// Caller prepares the output capacity. A refused message leaves it empty.
[[nodiscard]] inline canonical_tick_wire_status try_encode_canonical_tick(
  const uint64_t tick, const std::optional<uint64_t> state_root,
  const std::span<const canonical_tick_record_view> records, std::vector<std::byte>& out) {
  out.clear();
  if (tick == 0) return canonical_tick_wire_status::invalid_header;
  if (records.size() > canonical_tick_max_records) return canonical_tick_wire_status::too_many_records;
  size_t required = canonical_tick_base_bytes(state_root.has_value());
  for (size_t i = 0; i < records.size(); ++i) {
    if (!detail::canonical_record_valid(records[i])) return canonical_tick_wire_status::invalid_record;
    if (const auto order = detail::canonical_input_order(records, i); order != canonical_tick_wire_status::ok)
      return order;
    required += canonical_tick_record_bytes(records[i].payload.size());
  }
  if (required > canonical_tick_max_bytes) return canonical_tick_wire_status::too_large;
  if (out.capacity() < required) return canonical_tick_wire_status::buffer_too_small;

  state_writer writer(out, false);
  writer.u8(uint8_t(hot_message_type::canonical_tick));
  writer.u16(uint16_t(tick & 0xffffu));
  writer.u8(state_root ? 1 : 0);
  writer.u8(uint8_t(records.size()));
  if (state_root) writer.u64(*state_root);
  for (const auto& record : records) {
    writer.u64(record.origin_peer);
    writer.u64(record.input_sequence);
    writer.u16(uint16_t(record.payload.size()));
    writer.bytes(record.payload);
  }
  if (!writer.good()) {
    out.clear();
    return canonical_tick_wire_status::buffer_too_small;
  }
  return canonical_tick_wire_status::ok;
}

// `storage` receives views into `bytes` only after the whole message validates.
// Both outputs stay untouched on refusal. The caller keeps bytes alive while
// consuming the view, and validates each project payload before stepping.
// `receiver_tick` is the receiver's own committed tick; a follower expects the
// decoded tick to be exactly receiver_tick + 1, and checks that itself.
[[nodiscard]] inline canonical_tick_wire_status try_decode_canonical_tick(
  const std::span<const std::byte> bytes, const uint64_t receiver_tick,
  const std::span<canonical_tick_record_view> storage, canonical_tick_view& out) noexcept {
  if (bytes.size() > canonical_tick_max_bytes) return canonical_tick_wire_status::too_large;
  if (bytes.size() < canonical_tick_header_bytes) return canonical_tick_wire_status::truncated;
  state_reader reader(bytes);
  if (reader.u8() != uint8_t(hot_message_type::canonical_tick))
    return canonical_tick_wire_status::wrong_message_type;
  const uint64_t tick = widen_tick_low16(receiver_tick, reader.u16());
  const uint8_t flags = reader.u8();
  const uint8_t count = reader.u8();
  if (!reader.good()) return canonical_tick_wire_status::truncated;
  if (tick == 0 || flags > 1) return canonical_tick_wire_status::invalid_header;
  if (count > canonical_tick_max_records) return canonical_tick_wire_status::too_many_records;
  if (count > storage.size()) return canonical_tick_wire_status::capacity_exceeded;
  std::optional<uint64_t> root;
  if (flags != 0) root = reader.u64();
  if (!reader.good()) return canonical_tick_wire_status::truncated;

  std::array<canonical_tick_record_view, canonical_tick_max_records> staged{};
  for (size_t i = 0; i < count; ++i) {
    staged[i].origin_peer = reader.u64();
    staged[i].input_sequence = reader.u64();
    const uint16_t length = reader.u16();
    if (!reader.good()) return canonical_tick_wire_status::truncated;
    if (length == 0 || length > canonical_tick_max_payload_bytes ||
        staged[i].origin_peer == 0 || staged[i].input_sequence == 0)
      return canonical_tick_wire_status::invalid_record;
    staged[i].payload = reader.take(length);
    if (!reader.good()) return canonical_tick_wire_status::truncated;
    if (const auto order = detail::canonical_input_order(std::span(staged).first(i + 1), i);
        order != canonical_tick_wire_status::ok)
      return order;
  }
  if (reader.position() != reader.size()) return canonical_tick_wire_status::trailing_bytes;
  for (size_t i = 0; i < count; ++i) storage[i] = staged[i];
  out = {tick, root, storage.first(count)};
  return canonical_tick_wire_status::ok;
}

} // namespace devils_engine::network

#endif
