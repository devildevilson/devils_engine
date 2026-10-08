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
namespace devils_engine::network {

inline constexpr size_t canonical_tick_max_bytes = 1024;
inline constexpr size_t canonical_tick_max_records = 32;
inline constexpr size_t canonical_tick_max_payload_bytes = 256;
inline constexpr size_t canonical_tick_header_bytes = 1 + 8 + 1 + 1;
inline constexpr size_t canonical_tick_root_bytes = 8;
inline constexpr size_t canonical_tick_record_header_bytes = 8 + 8 + 2;

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
};

namespace detail {

[[nodiscard]] inline bool canonical_record_valid(const canonical_tick_record_view& value) noexcept {
  return value.origin_peer != 0 && value.input_sequence != 0 && !value.payload.empty() &&
         value.payload.size() <= canonical_tick_max_payload_bytes;
}

[[nodiscard]] inline bool canonical_input_duplicate(
  const std::span<const canonical_tick_record_view> records, const size_t index) noexcept {
  for (size_t i = 0; i < index; ++i) {
    if (records[i].origin_peer == records[index].origin_peer &&
        records[i].input_sequence == records[index].input_sequence) return true;
  }
  return false;
}

} // namespace detail

// Caller prepares the output capacity. A refused message leaves it empty.
[[nodiscard]] inline canonical_tick_wire_status try_encode_canonical_tick(
  const uint64_t tick, const std::optional<uint64_t> state_root,
  const std::span<const canonical_tick_record_view> records, std::vector<std::byte>& out) {
  out.clear();
  if (tick == 0) return canonical_tick_wire_status::invalid_header;
  if (records.size() > canonical_tick_max_records) return canonical_tick_wire_status::too_many_records;
  size_t required = canonical_tick_header_bytes +
                    (state_root ? canonical_tick_root_bytes : 0);
  for (size_t i = 0; i < records.size(); ++i) {
    if (!detail::canonical_record_valid(records[i])) return canonical_tick_wire_status::invalid_record;
    if (detail::canonical_input_duplicate(records, i)) return canonical_tick_wire_status::duplicate_input;
    required += canonical_tick_record_header_bytes + records[i].payload.size();
  }
  if (required > canonical_tick_max_bytes) return canonical_tick_wire_status::too_large;
  if (out.capacity() < required) return canonical_tick_wire_status::buffer_too_small;

  state_writer writer(out, false);
  writer.u8(uint8_t(hot_message_type::canonical_tick));
  writer.u64(tick);
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
[[nodiscard]] inline canonical_tick_wire_status try_decode_canonical_tick(
  const std::span<const std::byte> bytes, const std::span<canonical_tick_record_view> storage,
  canonical_tick_view& out) noexcept {
  if (bytes.size() > canonical_tick_max_bytes) return canonical_tick_wire_status::too_large;
  if (bytes.size() < canonical_tick_header_bytes) return canonical_tick_wire_status::truncated;
  state_reader reader(bytes);
  if (reader.u8() != uint8_t(hot_message_type::canonical_tick))
    return canonical_tick_wire_status::wrong_message_type;
  const uint64_t tick = reader.u64();
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
    if (detail::canonical_input_duplicate(std::span(staged).first(i + 1), i))
      return canonical_tick_wire_status::duplicate_input;
  }
  if (reader.position() != reader.size()) return canonical_tick_wire_status::trailing_bytes;
  for (size_t i = 0; i < count; ++i) storage[i] = staged[i];
  out = {tick, root, storage.first(count)};
  return canonical_tick_wire_status::ok;
}

} // namespace devils_engine::network

#endif
