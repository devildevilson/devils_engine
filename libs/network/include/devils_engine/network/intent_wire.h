#ifndef DEVILS_ENGINE_NETWORK_INTENT_WIRE_H
#define DEVILS_ENGINE_NETWORK_INTENT_WIRE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <devils_engine/utils/hash.h>

#include "state_schema.h"

// The hot upstream class: what a client asks of the authority, every tick.
//
// This is deliberately NOT the project's simulation intent. A simulation intent
// carries everything the simulation needs, including fields which must never
// arrive from a peer — the acting entity (the connection already names it, and
// a client-supplied actor is the classic ownership forgery) and provenance
// (which action produced it, meaningless and untrusted here). `network::intent`
// carries only what the authority cannot derive, and the project translates it
// into its own intent at one seam, which is exactly where ownership, legality
// and rate validation belong.
//
// Unlike the handshake there is no magic and no length field: the transport
// delivers whole messages, so the received size is authoritative, and the peer
// is already established. Byte zero is the message type because it decides how
// every following byte is read — including, in other classes, the width of a
// length field. A batch carries no count either: intents are self-describing,
// the reader consumes until the buffer ends, and a remainder which cannot form
// a whole intent is a refusal rather than a partially accepted batch.

namespace devils_engine::network {

enum class hot_message_type : uint8_t {
  // 0 is not a message: a zero-filled buffer must not decode as anything.
  intent_batch = 1,
  // 2..63 remain for further upstream classes, 64..191 for authority classes,
  // 192..255 for bulk transfer.
  relevant_set_update = 64,
  transform_frame = 65,
};

inline constexpr size_t intent_batch_header_bytes = 3;
inline constexpr unsigned intent_kind_bits = 5;
inline constexpr unsigned intent_tick_delta_bits = 3;
inline constexpr uint8_t intent_kind_limit = (1u << intent_kind_bits) - 1;
inline constexpr uint8_t intent_tick_delta_limit = (1u << intent_tick_delta_bits) - 1;
inline constexpr uint8_t intent_max_axes = 3;

// One packet must stay inside one transport packet: a fragmented intent batch
// would trade the whole point of the class, which is arriving now, for a
// completeness it does not need.
inline constexpr size_t intent_batch_max_bytes = 1024;

enum class intent_wire_status : uint8_t {
  ok,
  buffer_too_small,
  too_large,
  truncated,
  wrong_message_type,
  unknown_kind,
  duplicate_kind,
  invalid_layout,
  tick_delta_out_of_range,
  reference_out_of_range,
  capacity_exceeded
};

// What one kind of intent carries. The project owns what a kind MEANS; the
// library owns how its declared shape travels, so no gameplay verb is named
// here. Sizes follow from the declaration, which is what lets a batch omit
// both a count and per-field presence flags.
struct intent_field_layout {
  uint8_t axes = 0;    // quantized world point: per axis a cell delta and a code
  bool reference = false;   // dense registry index
  bool target = false;      // opaque entity reference, validated by the project
  bool turn = false;        // quantized direction, independent of any movement

  [[nodiscard]] constexpr bool valid() const noexcept {
    return axes <= intent_max_axes;
  }

  [[nodiscard]] constexpr size_t wire_bytes() const noexcept {
    return 1 + size_t(axes) * 3 + (reference ? 2 : 0) + (target ? 4 : 0) + (turn ? 2 : 0);
  }
};

struct intent_kind_layout {
  uint8_t kind = 0;
  intent_field_layout fields;
};

// A point is carried relative to a base cell the receiver already knows: the
// actor's own cell. A signed cell delta keeps a target outside that cell
// addressable without paying a full 32-bit key per axis and per tick.
struct intent {
  uint32_t target = 0;
  std::array<uint16_t, intent_max_axes> code{};
  uint16_t reference = 0;
  uint16_t turn = 0;
  std::array<int8_t, intent_max_axes> cell_delta{};
  uint8_t kind = 0;
  // Ticks back from the batch's base tick. Resending recent ticks is how this
  // class survives loss without a retransmit protocol, so the window is part
  // of the encoding rather than a policy above it.
  uint8_t tick_delta = 0;

  bool operator==(const intent&) const = default;
};

// The dense translation table for 64-bit registered identifiers. The index of
// an identifier is its position in the sorted set, so peers need no agreement
// beyond registering the same things: sorting is the agreement.
//
// Index stability across versions is explicitly NOT a property. Adding one
// identifier shifts every later index, and that is safe only because the
// fingerprint below belongs in session_compatibility::intent_schema_fingerprint
// and refuses a peer with a different set before the first tick. Stability by
// refusal is cheaper and stricter than stability by reserved ranges.
class id_index_table {
public:
  enum class build_status : uint8_t { built, empty, too_many, duplicate };

  [[nodiscard]] static build_status try_build(const std::span<const uint64_t> ids,
                                              id_index_table& output) {
    if (ids.empty()) return build_status::empty;
    if (ids.size() > size_t(std::numeric_limits<uint16_t>::max()) + 1)
      return build_status::too_many;

    std::vector<uint64_t> sorted(ids.begin(), ids.end());
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
      return build_status::duplicate;

    // The fingerprint hashes canonical little-endian bytes, not the objects:
    // hashing native storage would give two platforms different fingerprints
    // for the same registry and refuse a legitimate peer.
    std::vector<std::byte> canonical;
    canonical.reserve(sorted.size() * sizeof(uint64_t));
    state_writer writer(canonical, false);
    for (const auto id : sorted) writer.u64(id);
    if (!writer.good()) return build_status::too_many;

    output.sorted_ = std::move(sorted);
    output.fingerprint_ = utils::murmur_hash3_32(std::span<const std::byte>(canonical));
    return build_status::built;
  }

  [[nodiscard]] std::optional<uint16_t> index_of(const uint64_t id) const noexcept {
    const auto it = std::lower_bound(sorted_.begin(), sorted_.end(), id);
    if (it == sorted_.end() || *it != id) return std::nullopt;
    return uint16_t(it - sorted_.begin());
  }

  [[nodiscard]] std::optional<uint64_t> id_at(const uint16_t index) const noexcept {
    if (size_t(index) >= sorted_.size()) return std::nullopt;
    return sorted_[index];
  }

  [[nodiscard]] size_t size() const noexcept {
    return sorted_.size();
  }

  [[nodiscard]] uint32_t fingerprint() const noexcept {
    return fingerprint_;
  }

private:
  std::vector<uint64_t> sorted_;
  uint32_t fingerprint_ = 0;
};

// Declared kinds, resolved once. A table is prepared rather than searched per
// intent, and a duplicate or malformed declaration is refused at preparation
// instead of producing two readings of one code on the wire.
class intent_layout_table {
public:
  [[nodiscard]] intent_wire_status assign(const std::span<const intent_kind_layout> layouts) {
    known_.fill(false);
    fields_.fill(intent_field_layout{});
    for (const auto& layout : layouts) {
      if (layout.kind > intent_kind_limit || !layout.fields.valid())
        return intent_wire_status::invalid_layout;
      if (known_[layout.kind]) return intent_wire_status::duplicate_kind;
      known_[layout.kind] = true;
      fields_[layout.kind] = layout.fields;
    }
    return intent_wire_status::ok;
  }

  [[nodiscard]] bool known(const uint8_t kind) const noexcept {
    return kind <= intent_kind_limit && known_[kind];
  }

  [[nodiscard]] intent_field_layout fields(const uint8_t kind) const noexcept {
    return kind <= intent_kind_limit ? fields_[kind] : intent_field_layout{};
  }

private:
  std::array<intent_field_layout, intent_kind_limit + 1> fields_{};
  std::array<bool, intent_kind_limit + 1> known_{};
};

namespace detail {

[[nodiscard]] inline size_t intent_batch_bytes(
  const std::span<const intent> intents, const intent_layout_table& layouts) noexcept {
  size_t total = intent_batch_header_bytes;
  for (const auto& value : intents) total += layouts.fields(value.kind).wire_bytes();
  return total;
}

} // namespace detail

// base_tick travels as its low 16 bits. The receiver reconstructs the absolute
// tick from the session's own progress, which is why the handshake establishes
// a wide absolute tick: 16 bits cannot be widened by a peer's assertion, only
// by what the receiver already knows.
namespace detail {

[[nodiscard]] inline intent_wire_status encode_intent_batch_body(
  const uint64_t base_tick, const std::span<const intent> intents,
  const intent_layout_table& layouts, const id_index_table& registry,
  std::vector<std::byte>& out) {
  out.clear();
  for (const auto& value : intents) {
    if (!layouts.known(value.kind)) return intent_wire_status::unknown_kind;
    if (value.tick_delta > intent_tick_delta_limit)
      return intent_wire_status::tick_delta_out_of_range;
    // Checked on both sides on purpose: an encoder which can produce a batch
    // its own decoder refuses is a bug that only shows up across the network.
    if (layouts.fields(value.kind).reference && !registry.id_at(value.reference))
      return intent_wire_status::reference_out_of_range;
  }
  const size_t required = detail::intent_batch_bytes(intents, layouts);
  if (required > intent_batch_max_bytes) return intent_wire_status::too_large;
  if (out.capacity() < required) return intent_wire_status::buffer_too_small;

  state_writer w(out, false);
  w.u8(uint8_t(hot_message_type::intent_batch));
  w.u16(uint16_t(base_tick & 0xffffu));
  for (const auto& value : intents) {
    const auto fields = layouts.fields(value.kind);
    w.u8(uint8_t(value.kind | (value.tick_delta << intent_kind_bits)));
    for (uint8_t axis = 0; axis < fields.axes; ++axis) {
      w.u8(uint8_t(value.cell_delta[axis]));
      w.u16(value.code[axis]);
    }
    if (fields.reference) w.u16(value.reference);
    if (fields.target) w.u32(value.target);
    if (fields.turn) w.u16(value.turn);
  }
  return w.good() ? intent_wire_status::ok : intent_wire_status::buffer_too_small;
}

} // namespace detail

// A refused batch leaves the output empty, the same rule the decoder follows: a
// caller must never be handed a prefix of a message its own encoder rejected.
[[nodiscard]] inline intent_wire_status try_encode_intent_batch(
  const uint64_t base_tick, const std::span<const intent> intents,
  const intent_layout_table& layouts, const id_index_table& registry,
  std::vector<std::byte>& out) {
  const auto status =
    detail::encode_intent_batch_body(base_tick, intents, layouts, registry, out);
  if (status != intent_wire_status::ok) out.clear();
  return status;
}

struct intent_batch_view {
  uint16_t base_tick_low = 0;
  size_t count = 0;
};

// Decoding fills prepared caller storage and never grows it: an authority which
// accepts an unbounded number of intents from one packet has handed a peer its
// allocator. The reference index is checked against the registry here, so an
// out-of-range index is a refusal at the boundary rather than a lookup miss
// inside the project.
namespace detail {

[[nodiscard]] inline intent_wire_status decode_intent_batch_body(
  const std::span<const std::byte> bytes, const intent_layout_table& layouts,
  const id_index_table& registry, intent_batch_view& view, std::vector<intent>& out) {
  out.clear();
  if (bytes.size() > intent_batch_max_bytes) return intent_wire_status::too_large;
  if (bytes.size() < intent_batch_header_bytes) return intent_wire_status::truncated;

  state_reader r(bytes);
  if (r.u8() != uint8_t(hot_message_type::intent_batch))
    return intent_wire_status::wrong_message_type;
  view.base_tick_low = r.u16();
  view.count = 0;
  if (!r.good()) return intent_wire_status::truncated;

  while (r.position() != r.size()) {
    const auto packed = r.u8();
    if (!r.good()) return intent_wire_status::truncated;
    intent value;
    value.kind = uint8_t(packed & intent_kind_limit);
    value.tick_delta = uint8_t(packed >> intent_kind_bits);
    if (!layouts.known(value.kind)) return intent_wire_status::unknown_kind;

    const auto fields = layouts.fields(value.kind);
    // The remaining bytes must hold this whole intent. Checking before reading
    // keeps a truncated tail from being reported as a decoded intent of zeros.
    if (r.size() - r.position() < fields.wire_bytes() - 1) return intent_wire_status::truncated;

    for (uint8_t axis = 0; axis < fields.axes; ++axis) {
      value.cell_delta[axis] = int8_t(r.u8());
      value.code[axis] = r.u16();
    }
    if (fields.reference) {
      value.reference = r.u16();
      if (!registry.id_at(value.reference)) return intent_wire_status::reference_out_of_range;
    }
    if (fields.target) value.target = r.u32();
    if (fields.turn) value.turn = r.u16();
    if (!r.good()) return intent_wire_status::truncated;
    if (out.size() == out.capacity()) return intent_wire_status::capacity_exceeded;
    out.push_back(value);
    ++view.count;
  }
  return r.good() ? intent_wire_status::ok : intent_wire_status::truncated;
}

} // namespace detail

// A refused batch leaves nothing behind. Decoding fills the caller's storage as
// it reads, so without this a caller that inspected `out` after a refusal would
// find the prefix of a message which was never accepted — the one shape of
// "partially accepted batch" the format is meant to make impossible. clear()
// keeps the capacity, so the no-growth guarantee is unaffected.
[[nodiscard]] inline intent_wire_status try_decode_intent_batch(
  const std::span<const std::byte> bytes, const intent_layout_table& layouts,
  const id_index_table& registry, intent_batch_view& view, std::vector<intent>& out) {
  const auto status = detail::decode_intent_batch_body(bytes, layouts, registry, view, out);
  if (status != intent_wire_status::ok) {
    out.clear();
    view.count = 0;
  }
  return status;
}

// Low bits alone cannot say which window they belong to, so the nearest
// candidate to the receiver's own tick is chosen. Every hot class carries its
// tick this way, so the arithmetic lives here once: two implementations of a
// wrap rule are two chances to disagree exactly once every 65536 ticks, which
// is the least reproducible bug this campaign could produce.
[[nodiscard]] inline constexpr uint64_t widen_tick_low16(
  const uint64_t receiver_tick, const uint16_t tick_low) noexcept {
  constexpr uint64_t window = uint64_t(1) << 16;
  constexpr uint64_t half = window / 2;
  uint64_t base = (receiver_tick & ~(window - 1)) | uint64_t(tick_low);
  if (base + half < receiver_tick) base += window;
  else if (base >= window && base > receiver_tick + half) base -= window;
  return base;
}

// The absolute tick of one intent, reconstructed from what the receiver already
// knows. A batch whose reconstructed tick lies in the future of the receiver is
// the caller's decision to refuse; the library only performs the arithmetic, and
// refuses only the case which has no answer at all — a delta reaching before the
// first tick.
[[nodiscard]] inline std::optional<uint64_t> intent_absolute_tick(
  const uint64_t receiver_tick, const uint16_t base_tick_low,
  const uint8_t tick_delta) noexcept {
  const uint64_t base = widen_tick_low16(receiver_tick, base_tick_low);
  if (base < uint64_t(tick_delta)) return std::nullopt;
  return base - uint64_t(tick_delta);
}

} // namespace devils_engine::network

#endif
