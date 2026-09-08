#ifndef DEVILS_ENGINE_NETWORK_TRANSFORM_WIRE_H
#define DEVILS_ENGINE_NETWORK_TRANSFORM_WIRE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "fixed_point.h"
#include "intent_wire.h"

// The hot downstream class: what the authority tells one client about the
// entities that client is allowed to know about, and how often.
//
// Three separate decisions live here, and keeping them separate is the whole
// design. WHO is replicated is the relevant set, a reliable ordered class with
// its own generation. WHAT travels is a declared per-class shape, so a frame
// needs no per-field presence flags. HOW OFTEN it travels is a cadence policy
// which the sender may adapt freely, because correctness must never depend on
// receiving any particular frame.
//
// The consequence worth stating first: a transform frame addresses entities by
// a dense per-client SLOT, never by an entity identifier. The slot is small,
// stable only within one set generation, and meaningless to anyone but that
// client. A client is therefore never TOLD about an entity outside its own
// set, which makes relevance a disclosure boundary and not only a bandwidth
// one — the classic wallhack is a replication decision, not a rendering one.
//
// The trust direction is the reverse of HOT-01's: this class travels from the
// authority to a client, so nothing here needs to defend against a forged
// field. What it must still defend against is a DISAGREEMENT — the two sides
// reading one message as two different sets of entities — which is why every
// refusal below is about the two peers' state and not about a hostile sender.
//
// The two classes travel on two different lanes, and that is exactly why a
// frame carries the generation it was built against: an unreliable frame can
// overtake the reliable membership update that gives its slots meaning. Both
// directions of that race are refused rather than guessed, and the refusal is
// the measurable cost of the split.

namespace devils_engine::network {

inline constexpr uint8_t transform_max_axes = 3;
inline constexpr unsigned transform_class_bits = 6;
inline constexpr uint8_t transform_class_limit = (1u << transform_class_bits) - 1;

// One frame must stay inside one transport packet, for the same reason an
// intent batch must: fragmenting a latest-value observation trades its only
// virtue, arriving now, for a completeness it does not need. A set larger than
// one frame is sent as several frames, which is why the dense mode carries a
// first slot rather than assuming the whole set.
inline constexpr size_t transform_frame_max_bytes = 1024;

// The membership class is reliable, so it may exceed one packet: the transport
// fragments it and the receiver applies it whole. The bound is here to keep a
// forged or corrupted length from being read as a legitimate huge update.
inline constexpr size_t relevant_set_update_max_bytes = 16 * 1024;

// A declared budget rather than a wire limit: the slot field would address
// 65536 entities, but the measured ladder puts 96 relevant entities at about
// 7 KB/s per client, so a full thousand is already an order of magnitude past
// what the cadence table plans for. Exhausting it is a reported refusal, so a
// relevance function which grew without bound says so instead of quietly
// costing a client its uplink.
inline constexpr uint16_t relevant_set_max_slots = 1024;

inline constexpr size_t relevant_set_update_header_bytes = 3; // type, generation
inline constexpr size_t slot_enter_record_bytes = 11;         // operation, slot, handle
inline constexpr size_t slot_leave_record_bytes = 3;          // operation, slot
inline constexpr size_t transform_frame_header_bytes = 6;     // type, class, tick low, generation

enum class transform_wire_status : uint8_t {
  ok,
  // Encoding refusals.
  buffer_too_small,
  too_large,
  empty_frame,
  slots_not_ascending,
  dense_slots_not_consecutive,
  cell_out_of_reach,
  // Declaration refusals.
  unknown_class,
  duplicate_class,
  invalid_layout,
  invalid_cadence,
  // Decoding refusals.
  truncated,
  wrong_message_type,
  reserved_bit_set,
  capacity_exceeded,
  slot_out_of_range,
  unknown_slot,
  // The two lanes disagree about which generation of the set is current.
  generation_mismatch,
  generation_gap
};

// ---------------------------------------------------------------------------
// The relevant set: membership as its own reliable class.
// ---------------------------------------------------------------------------

enum class slot_operation : uint8_t { leave = 0, enter = 1 };

struct slot_change {
  // The project's entity handle, opaque here. Zero is reserved as "no entity"
  // so that a free slot has a representation which cannot be confused with a
  // real handle; the project's versioned identifier must never encode to zero.
  uint64_t handle = 0;
  uint16_t slot = 0;
  slot_operation operation = slot_operation::leave;

  bool operator==(const slot_change&) const = default;
};

enum class slot_admission : uint8_t { assigned, already_present, capacity_exhausted, invalid_handle };

struct slot_result {
  uint16_t slot = 0;
  slot_admission status = slot_admission::invalid_handle;

  [[nodiscard]] constexpr bool usable() const noexcept {
    return status == slot_admission::assigned || status == slot_admission::already_present;
  }
};

// A constructor cannot refuse, so a request above the declared slot budget is
// clamped and capacity() reports what was actually taken. This answers the
// question before construction, because a silently narrowed set would drop
// entities a relevance function believed it had admitted.
[[nodiscard]] inline constexpr bool relevant_set_capacity_fits(
  const uint16_t requested) noexcept {
  return requested <= relevant_set_max_slots;
}

// The authority's view of one client's set. Slots are dense and the lowest free
// slot is always reused, which is what lets a frame omit slot indices entirely
// for a full pass.
//
// Changes are not accumulated as a log. publish() diffs the current table
// against the last published one, so an entity which entered and left between
// two publications produces no wire traffic at all, and a slot which changed
// occupant produces one enter rather than a leave/enter pair whose order could
// matter. The mirror's state after applying a published diff is the sender's
// table by construction, not by careful bookkeeping.
class relevant_set {
public:
  explicit relevant_set(const uint16_t capacity)
    : occupant_(std::min(capacity, relevant_set_max_slots), 0),
      published_(std::min(capacity, relevant_set_max_slots), 0) {}

  [[nodiscard]] uint16_t capacity() const noexcept {
    return uint16_t(occupant_.size());
  }

  [[nodiscard]] uint16_t generation() const noexcept {
    return generation_;
  }

  [[nodiscard]] size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] std::optional<uint16_t> slot_of(const uint64_t handle) const noexcept {
    if (handle == 0) return std::nullopt;
    for (size_t slot = 0; slot < occupant_.size(); ++slot)
      if (occupant_[slot] == handle) return uint16_t(slot);
    return std::nullopt;
  }

  [[nodiscard]] uint64_t handle_at(const uint16_t slot) const noexcept {
    return size_t(slot) < occupant_.size() ? occupant_[slot] : 0;
  }

  slot_result enter(const uint64_t handle) {
    if (handle == 0) return slot_result{0, slot_admission::invalid_handle};
    if (const auto existing = slot_of(handle))
      return slot_result{*existing, slot_admission::already_present};
    // The lowest free slot, found by scanning. Bounded by the declared slot
    // budget and obviously correct, which matters more here than the constant
    // factor: density is the property the dense frame mode depends on.
    for (size_t slot = 0; slot < occupant_.size(); ++slot) {
      if (occupant_[slot] != 0) continue;
      occupant_[slot] = handle;
      ++size_;
      return slot_result{uint16_t(slot), slot_admission::assigned};
    }
    return slot_result{0, slot_admission::capacity_exhausted};
  }

  bool leave(const uint64_t handle) {
    const auto slot = slot_of(handle);
    if (!slot) return false;
    occupant_[*slot] = 0;
    --size_;
    return true;
  }

  // Publishes the difference and takes the next generation. An empty diff emits
  // nothing and leaves the generation alone: a generation which advanced
  // without a change would invalidate every frame in flight for nothing.
  uint16_t publish(std::vector<slot_change>& out) {
    out.clear();
    for (size_t slot = 0; slot < occupant_.size(); ++slot) {
      if (occupant_[slot] == published_[slot]) continue;
      out.push_back(occupant_[slot] == 0
                      ? slot_change{0, uint16_t(slot), slot_operation::leave}
                      : slot_change{occupant_[slot], uint16_t(slot), slot_operation::enter});
    }
    if (!out.empty()) {
      published_ = occupant_;
      ++generation_;
    }
    return generation_;
  }

  // The full set as one diff, for a client which must adopt it wholesale after
  // a reconnect. This is the recovery path and it is explicit for the same
  // reason state_frame_window::reset is: re-establishing a distant state must
  // never be reachable by an ordinary sequenced arrival.
  void publish_full(std::vector<slot_change>& out) const {
    out.clear();
    for (size_t slot = 0; slot < occupant_.size(); ++slot)
      if (occupant_[slot] != 0)
        out.push_back(slot_change{occupant_[slot], uint16_t(slot), slot_operation::enter});
  }

private:
  std::vector<uint64_t> occupant_;
  std::vector<uint64_t> published_;
  size_t size_ = 0;
  uint16_t generation_ = 0;
};

// The client's copy. It resolves a slot to a handle and nothing else: the
// project owns what a handle means, and a library which resolved handles to
// entities would have to know the project's world.
class relevant_set_mirror {
public:
  explicit relevant_set_mirror(const uint16_t capacity)
    : occupant_(std::min(capacity, relevant_set_max_slots), 0) {}

  [[nodiscard]] uint16_t capacity() const noexcept {
    return uint16_t(occupant_.size());
  }

  [[nodiscard]] uint16_t generation() const noexcept {
    return generation_;
  }

  [[nodiscard]] size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] uint64_t handle_at(const uint16_t slot) const noexcept {
    return size_t(slot) < occupant_.size() ? occupant_[slot] : 0;
  }

  [[nodiscard]] bool occupied(const uint16_t slot) const noexcept {
    return handle_at(slot) != 0;
  }

  // The membership lane is reliable and ordered, so the only generation this
  // mirror can legitimately be handed is the next one. A gap is therefore not a
  // network event to absorb but a lane misuse to report: absorbing it would
  // leave the mirror silently describing a set the authority never had.
  transform_wire_status apply(const uint16_t generation,
                              const std::span<const slot_change> changes) {
    if (generation != uint16_t(generation_ + 1)) return transform_wire_status::generation_gap;
    for (const auto& change : changes)
      if (size_t(change.slot) >= occupant_.size())
        return transform_wire_status::slot_out_of_range;
    for (const auto& change : changes) {
      const bool was = occupant_[change.slot] != 0;
      const bool now = change.operation == slot_operation::enter;
      occupant_[change.slot] = now ? change.handle : 0;
      if (was == now) continue;
      if (now) ++size_;
      else --size_;
    }
    generation_ = generation;
    return transform_wire_status::ok;
  }

  // Adopting a full set at an arbitrary generation, for the reconnect path.
  transform_wire_status adopt(const uint16_t generation,
                              const std::span<const slot_change> changes) {
    for (const auto& change : changes)
      if (size_t(change.slot) >= occupant_.size())
        return transform_wire_status::slot_out_of_range;
    std::fill(occupant_.begin(), occupant_.end(), 0);
    size_ = 0;
    for (const auto& change : changes) {
      if (change.operation != slot_operation::enter) continue;
      if (occupant_[change.slot] == 0) ++size_;
      occupant_[change.slot] = change.handle;
    }
    generation_ = generation;
    return transform_wire_status::ok;
  }

private:
  std::vector<uint64_t> occupant_;
  size_t size_ = 0;
  uint16_t generation_ = 0;
};

[[nodiscard]] inline size_t relevant_set_update_bytes(
  const std::span<const slot_change> changes) noexcept {
  size_t total = relevant_set_update_header_bytes;
  for (const auto& change : changes)
    total += change.operation == slot_operation::enter ? slot_enter_record_bytes
                                                       : slot_leave_record_bytes;
  return total;
}

namespace detail {

[[nodiscard]] inline transform_wire_status encode_relevant_set_update_body(
  const uint16_t generation, const std::span<const slot_change> changes,
  std::vector<std::byte>& out) {
  out.clear();
  if (changes.empty()) return transform_wire_status::empty_frame;
  const size_t required = relevant_set_update_bytes(changes);
  if (required > relevant_set_update_max_bytes) return transform_wire_status::too_large;
  if (out.capacity() < required) return transform_wire_status::buffer_too_small;

  state_writer w(out, false);
  w.u8(uint8_t(hot_message_type::relevant_set_update));
  w.u16(generation);
  for (const auto& change : changes) {
    if (change.operation == slot_operation::enter && change.handle == 0)
      return transform_wire_status::invalid_layout;
    w.u8(uint8_t(change.operation));
    w.u16(change.slot);
    if (change.operation == slot_operation::enter) w.u64(change.handle);
  }
  return w.good() ? transform_wire_status::ok : transform_wire_status::buffer_too_small;
}

} // namespace detail

// Like the intent batch this message carries no record count: an operation byte
// decides the width of its own record and the reader consumes until the buffer
// ends. A remainder which cannot form a whole record is a refusal, never a
// partially applied membership change.
//
// A refusal leaves the output empty on both sides of the wire. An encoder which
// left half a message behind would hand a caller bytes to send that describe a
// membership change the sender itself rejected.
[[nodiscard]] inline transform_wire_status try_encode_relevant_set_update(
  const uint16_t generation, const std::span<const slot_change> changes,
  std::vector<std::byte>& out) {
  const auto status = detail::encode_relevant_set_update_body(generation, changes, out);
  if (status != transform_wire_status::ok) out.clear();
  return status;
}

struct relevant_set_update_view {
  uint16_t generation = 0;
  size_t count = 0;
};

namespace detail {

[[nodiscard]] inline transform_wire_status decode_relevant_set_update_body(
  const std::span<const std::byte> bytes, relevant_set_update_view& view,
  std::vector<slot_change>& out) {
  out.clear();
  if (bytes.size() > relevant_set_update_max_bytes) return transform_wire_status::too_large;
  if (bytes.size() < relevant_set_update_header_bytes) return transform_wire_status::truncated;

  state_reader r(bytes);
  if (r.u8() != uint8_t(hot_message_type::relevant_set_update))
    return transform_wire_status::wrong_message_type;
  view.generation = r.u16();
  view.count = 0;
  if (!r.good()) return transform_wire_status::truncated;

  while (r.position() != r.size()) {
    const auto operation = r.u8();
    if (!r.good()) return transform_wire_status::truncated;
    if (operation > uint8_t(slot_operation::enter)) return transform_wire_status::invalid_layout;
    slot_change change;
    change.operation = slot_operation(operation);
    const size_t remaining = (change.operation == slot_operation::enter
                                ? slot_enter_record_bytes
                                : slot_leave_record_bytes) - 1;
    if (r.size() - r.position() < remaining) return transform_wire_status::truncated;
    change.slot = r.u16();
    if (change.operation == slot_operation::enter) {
      change.handle = r.u64();
      if (change.handle == 0) return transform_wire_status::invalid_layout;
    }
    if (!r.good()) return transform_wire_status::truncated;
    if (out.size() == out.capacity()) return transform_wire_status::capacity_exceeded;
    out.push_back(change);
    ++view.count;
  }
  return transform_wire_status::ok;
}

} // namespace detail

// A refused update leaves nothing behind, for the same reason the intent batch
// does: membership is applied whole or not at all, so a caller must never find
// the prefix of a message which was never accepted.
[[nodiscard]] inline transform_wire_status try_decode_relevant_set_update(
  const std::span<const std::byte> bytes, relevant_set_update_view& view,
  std::vector<slot_change>& out) {
  const auto status = detail::decode_relevant_set_update_body(bytes, view, out);
  if (status != transform_wire_status::ok) {
    out.clear();
    view.count = 0;
  }
  return status;
}

// ---------------------------------------------------------------------------
// Declared classes: shape, and separately, cadence.
// ---------------------------------------------------------------------------

enum class transform_delivery : uint8_t { unreliable_sequenced, reliable_ordered };

// The shape of one replicated data class. Position is a cell key plus a code,
// and direction is an INDEPENDENT field rather than something derived from
// velocity: a receiver which infers facing from motion cannot represent an
// entity turning in place, and two receivers inferring it from a lossy sample
// stream would not even agree with each other.
struct transform_field_layout {
  uint8_t axes = 0;
  bool turn = false;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return axes <= transform_max_axes && (axes != 0 || turn);
  }

  [[nodiscard]] constexpr size_t record_bytes(const bool sparse) const noexcept {
    return (sparse ? 2u : 0u) + size_t(axes) * 3 + (turn ? 2u : 0u);
  }

  [[nodiscard]] constexpr size_t header_bytes(const bool sparse) const noexcept {
    // A frame states one base cell per axis and every record carries a signed
    // delta from it. Four bytes per axis once buys three bytes per axis per
    // record instead of seven, and the base is a quantity the sender already
    // has: the region the client is looking at.
    return transform_frame_header_bytes + size_t(axes) * 4 + (sparse ? 0u : 2u);
  }
};

// How often a class is sent, and how stale it may become. This is deliberately
// NOT part of the shape fingerprint below: the sender may vary an interval
// within its declared bounds using relevance, velocity and link conditions, so
// two peers with different cadences are compatible by design. A differing
// SHAPE, by contrast, is a silent misreading of every record.
struct cadence_policy {
  uint32_t byte_budget = 0; // per send opportunity; zero means unbudgeted
  uint16_t interval_ticks = 1;
  uint16_t phase_ticks = 0;
  // Zero declares no bound. A non-zero bound below the interval would be
  // exceeded on a perfect link, which makes it a declaration error rather than
  // a strict policy.
  uint16_t max_age_ticks = 0;
  uint8_t priority = 0; // higher goes first when one tick cannot afford all classes
  transform_delivery delivery = transform_delivery::unreliable_sequenced;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return interval_ticks != 0 && phase_ticks < interval_ticks &&
           (max_age_ticks == 0 || max_age_ticks >= interval_ticks);
  }

  [[nodiscard]] constexpr bool due(const uint64_t tick) const noexcept {
    return interval_ticks != 0 && tick % uint64_t(interval_ticks) == uint64_t(phase_ticks);
  }
};

struct transform_class_layout {
  transform_field_layout fields;
  cadence_policy cadence;
  uint8_t class_id = 0;
};

class transform_layout_table {
public:
  [[nodiscard]] transform_wire_status assign(
    const std::span<const transform_class_layout> layouts) {
    known_.fill(false);
    fields_.fill(transform_field_layout{});
    cadence_.fill(cadence_policy{});
    order_.clear();
    for (const auto& layout : layouts) {
      if (layout.class_id > transform_class_limit || !layout.fields.valid())
        return transform_wire_status::invalid_layout;
      if (!layout.cadence.valid()) return transform_wire_status::invalid_cadence;
      if (known_[layout.class_id]) return transform_wire_status::duplicate_class;
      known_[layout.class_id] = true;
      fields_[layout.class_id] = layout.fields;
      cadence_[layout.class_id] = layout.cadence;
      order_.push_back(layout.class_id);
    }
    // Highest priority first, class id breaking the tie so that two peers, and
    // two runs, walk the classes in the same order.
    std::sort(order_.begin(), order_.end(), [this](const uint8_t left, const uint8_t right) {
      if (cadence_[left].priority != cadence_[right].priority)
        return cadence_[left].priority > cadence_[right].priority;
      return left < right;
    });
    fingerprint_ = compute_fingerprint();
    return transform_wire_status::ok;
  }

  [[nodiscard]] bool known(const uint8_t class_id) const noexcept {
    return class_id <= transform_class_limit && known_[class_id];
  }

  [[nodiscard]] transform_field_layout fields(const uint8_t class_id) const noexcept {
    return class_id <= transform_class_limit ? fields_[class_id] : transform_field_layout{};
  }

  [[nodiscard]] cadence_policy cadence(const uint8_t class_id) const noexcept {
    return class_id <= transform_class_limit ? cadence_[class_id] : cadence_policy{};
  }

  [[nodiscard]] std::span<const uint8_t> classes() const noexcept {
    return order_;
  }

  // Which classes this tick owes, in send order. The phase is what keeps two
  // classes of the same interval off the same tick: NET-LAB-01 had to learn
  // that a shared phase turns a staggered schedule back into a synchronous one.
  size_t due_classes(const uint64_t tick, const std::span<uint8_t> out) const noexcept {
    size_t count = 0;
    for (const auto class_id : order_) {
      if (!cadence_[class_id].due(tick)) continue;
      if (count == out.size()) break;
      out[count++] = class_id;
    }
    return count;
  }

  // Shape only, and that is the point: this belongs in the session's numeric
  // profile next to the declared quanta, because a peer reading a record with
  // a different field set produces a plausible wrong world rather than an
  // error. Cadence is excluded so an adaptive sender stays compatible.
  [[nodiscard]] uint32_t shape_fingerprint() const noexcept {
    return fingerprint_;
  }

private:
  [[nodiscard]] uint32_t compute_fingerprint() const {
    std::vector<std::byte> canonical;
    canonical.reserve(order_.size() * 3);
    state_writer writer(canonical, true);
    for (const auto class_id : order_) {
      writer.u8(class_id);
      writer.u8(fields_[class_id].axes);
      writer.u8(fields_[class_id].turn ? 1 : 0);
    }
    return utils::murmur_hash3_32(std::span<const std::byte>(canonical));
  }

  std::array<transform_field_layout, transform_class_limit + 1> fields_{};
  std::array<cadence_policy, transform_class_limit + 1> cadence_{};
  std::array<bool, transform_class_limit + 1> known_{};
  std::vector<uint8_t> order_;
  uint32_t fingerprint_ = 0;
};

// ---------------------------------------------------------------------------
// The frame itself.
// ---------------------------------------------------------------------------

enum class transform_frame_mode : uint8_t { dense = 0, sparse = 1 };

// One entity's transform in absolute terms. The wire-relative cell delta never
// leaves the codec: a caller which had to convert keys itself is a caller which
// can get the conversion wrong, and getting it wrong places an entity
// somewhere plausible.
struct transform_sample {
  std::array<int32_t, transform_max_axes> key{};
  std::array<uint16_t, transform_max_axes> code{};
  uint16_t slot = 0;
  uint16_t turn = 0;

  bool operator==(const transform_sample&) const = default;
};

// The cell the frame's deltas are measured from — normally the region the
// client is looking at.
struct transform_frame_origin {
  std::array<int32_t, transform_max_axes> key{};

  bool operator==(const transform_frame_origin&) const = default;
};

[[nodiscard]] inline size_t transform_frame_bytes(const transform_field_layout fields,
                                                  const transform_frame_mode mode,
                                                  const size_t records) noexcept {
  const bool sparse = mode == transform_frame_mode::sparse;
  return fields.header_bytes(sparse) + records * fields.record_bytes(sparse);
}

// How many records one frame can carry. A sender splits a larger set across
// frames rather than growing one, so this is the arithmetic that decides where
// the split falls.
[[nodiscard]] inline size_t transform_frame_capacity(
  const transform_field_layout fields, const transform_frame_mode mode,
  const size_t max_bytes = transform_frame_max_bytes) noexcept {
  const bool sparse = mode == transform_frame_mode::sparse;
  const size_t header = fields.header_bytes(sparse);
  const size_t record = fields.record_bytes(sparse);
  if (record == 0 || max_bytes <= header) return 0;
  return (max_bytes - header) / record;
}

// What one send opportunity can actually carry. A set larger than one frame is
// several frames, and a declared byte budget may not afford even those, so the
// question "does everything pending fit right now" has to be answerable before
// anything is encoded — that is what makes `byte_budget` a policy rather than a
// comment. A sender which cannot fit its whole set learns it here, and choosing
// WHO gets cut is the project's decision, which is what priority is for.
struct transform_send_plan {
  size_t bytes = 0;
  size_t records = 0;
  size_t frames = 0;
  bool complete = false;

  bool operator==(const transform_send_plan&) const = default;
};

[[nodiscard]] inline transform_send_plan plan_transform_send(
  const transform_field_layout fields, const transform_frame_mode mode, const size_t pending,
  const cadence_policy policy) noexcept {
  transform_send_plan plan;
  const bool sparse = mode == transform_frame_mode::sparse;
  const size_t capacity = transform_frame_capacity(fields, mode);
  const size_t header = fields.header_bytes(sparse);
  const size_t record = fields.record_bytes(sparse);
  if (capacity == 0 || record == 0) return plan;

  size_t left = pending;
  while (left != 0) {
    const size_t take = left < capacity ? left : capacity;
    const size_t bytes = transform_frame_bytes(fields, mode, take);
    if (policy.byte_budget == 0 || plan.bytes + bytes <= size_t(policy.byte_budget)) {
      plan.bytes += bytes;
      plan.records += take;
      ++plan.frames;
      left -= take;
      continue;
    }
    // The budget cannot afford a whole frame. A partial frame is still worth
    // sending — a latest-value class loses nothing by carrying fewer entities —
    // but a budget which cannot pay for even one record buys no frame at all,
    // and reporting that is better than emitting a header carrying nothing.
    const size_t room = size_t(policy.byte_budget) - plan.bytes;
    if (room > header) {
      const size_t fit = (room - header) / record;
      if (fit != 0) {
        const size_t partial = fit < take ? fit : take;
        plan.bytes += transform_frame_bytes(fields, mode, partial);
        plan.records += partial;
        ++plan.frames;
      }
    }
    break;
  }
  plan.complete = plan.records == pending;
  return plan;
}

// Slots ascend in both modes, and in dense mode they are consecutive. Ascending
// order is canonical for the same reason the intent bundle's order is: it makes
// a duplicate cheap to detect and leaves the sender no freedom the receiver
// would have to tolerate.
namespace detail {

[[nodiscard]] inline transform_wire_status encode_transform_frame_body(
  const uint8_t class_id, const uint64_t tick, const uint16_t generation,
  const transform_frame_mode mode, const transform_frame_origin& origin,
  const std::span<const transform_sample> samples, const transform_layout_table& layouts,
  std::vector<std::byte>& out) {
  out.clear();
  if (!layouts.known(class_id)) return transform_wire_status::unknown_class;
  // An empty transform frame carries no information at all. Unlike an intent
  // bundle, where an explicit empty bundle is the statement "this tick is
  // closed", a latest-value observation with nothing to observe is simply not
  // sent, and a sender producing one has a bug worth reporting.
  if (samples.empty()) return transform_wire_status::empty_frame;

  const auto fields = layouts.fields(class_id);
  const bool sparse = mode == transform_frame_mode::sparse;
  for (size_t index = 1; index < samples.size(); ++index) {
    if (samples[index].slot <= samples[index - 1].slot)
      return transform_wire_status::slots_not_ascending;
    if (!sparse && samples[index].slot != uint16_t(samples[index - 1].slot + 1))
      return transform_wire_status::dense_slots_not_consecutive;
  }

  const size_t required = transform_frame_bytes(fields, mode, samples.size());
  if (required > transform_frame_max_bytes) return transform_wire_status::too_large;
  if (out.capacity() < required) return transform_wire_status::buffer_too_small;

  state_writer w(out, false);
  w.u8(uint8_t(hot_message_type::transform_frame));
  w.u8(uint8_t(class_id | (sparse ? (1u << transform_class_bits) : 0u)));
  w.u16(uint16_t(tick & 0xffffu));
  w.u16(generation);
  for (uint8_t axis = 0; axis < fields.axes; ++axis) w.u32(uint32_t(origin.key[axis]));
  if (!sparse) w.u16(samples.front().slot);

  for (const auto& sample : samples) {
    if (sparse) w.u16(sample.slot);
    for (uint8_t axis = 0; axis < fields.axes; ++axis) {
      const auto delta = relative_cell(origin.key[axis], sample.key[axis]);
      // Out of reach means the entity is further from the frame's origin than
      // the format can express — which is to say further than a relevance
      // function should ever have admitted. Reporting it names a relevance
      // fault instead of teleporting the entity to a plausible cell.
      if (delta.out_of_range) return transform_wire_status::cell_out_of_reach;
      w.u8(uint8_t(delta.value));
      w.u16(sample.code[axis]);
    }
    if (fields.turn) w.u16(sample.turn);
  }
  return w.good() ? transform_wire_status::ok : transform_wire_status::buffer_too_small;
}

} // namespace detail

// A refused frame leaves the output empty. Reach is checked per record while
// encoding, so without this a relevance fault would still hand the caller a
// truncated frame to send.
[[nodiscard]] inline transform_wire_status try_encode_transform_frame(
  const uint8_t class_id, const uint64_t tick, const uint16_t generation,
  const transform_frame_mode mode, const transform_frame_origin& origin,
  const std::span<const transform_sample> samples, const transform_layout_table& layouts,
  std::vector<std::byte>& out) {
  const auto status = detail::encode_transform_frame_body(class_id, tick, generation, mode,
                                                          origin, samples, layouts, out);
  if (status != transform_wire_status::ok) out.clear();
  return status;
}

struct transform_frame_view {
  uint64_t tick = 0; // widened against the receiver's own progress
  size_t count = 0;
  uint16_t generation = 0;
  uint8_t class_id = 0;
  transform_frame_mode mode = transform_frame_mode::dense;
};

// The mirror is passed in for the same reason the intent decoder is passed the
// registry: a slot which the receiver's set does not occupy is refused at the
// boundary rather than becoming a lookup miss inside the project. With the
// generation already matched, an unoccupied slot is not a race — it is the two
// sides disagreeing about a set they both claim to be the same version of.
namespace detail {

[[nodiscard]] inline transform_wire_status decode_transform_frame_body(
  const std::span<const std::byte> bytes, const transform_layout_table& layouts,
  const relevant_set_mirror& mirror, const uint64_t receiver_tick,
  transform_frame_view& view, std::vector<transform_sample>& out) {
  out.clear();
  if (bytes.size() > transform_frame_max_bytes) return transform_wire_status::too_large;
  if (bytes.size() < transform_frame_header_bytes) return transform_wire_status::truncated;

  state_reader r(bytes);
  if (r.u8() != uint8_t(hot_message_type::transform_frame))
    return transform_wire_status::wrong_message_type;
  const auto packed = r.u8();
  if ((packed & 0x80u) != 0) return transform_wire_status::reserved_bit_set;
  view.class_id = uint8_t(packed & transform_class_limit);
  view.mode = (packed & (1u << transform_class_bits)) != 0 ? transform_frame_mode::sparse
                                                           : transform_frame_mode::dense;
  const auto tick_low = r.u16();
  view.generation = r.u16();
  view.tick = widen_tick_low16(receiver_tick, tick_low);
  view.count = 0;
  if (!r.good()) return transform_wire_status::truncated;
  if (!layouts.known(view.class_id)) return transform_wire_status::unknown_class;

  // The membership lane and the frame lane are different lanes, so either can
  // be ahead. Both cases are refused, and a caller which counts the two
  // separately is measuring what the split actually costs.
  if (view.generation != mirror.generation()) return transform_wire_status::generation_mismatch;

  const auto fields = layouts.fields(view.class_id);
  const bool sparse = view.mode == transform_frame_mode::sparse;
  std::array<int32_t, transform_max_axes> origin{};
  for (uint8_t axis = 0; axis < fields.axes; ++axis) origin[axis] = int32_t(r.u32());
  uint16_t next_dense_slot = 0;
  if (!sparse) next_dense_slot = r.u16();
  if (!r.good()) return transform_wire_status::truncated;

  const size_t record = fields.record_bytes(sparse);
  const size_t payload = r.size() - r.position();
  // A payload which is not a whole number of records is a truncated frame, not
  // a frame with a trailing record of zeros.
  if (record == 0 || payload == 0 || payload % record != 0)
    return transform_wire_status::truncated;

  bool have_previous = false;
  uint16_t previous_slot = 0;
  while (r.position() != r.size()) {
    transform_sample sample;
    if (sparse) {
      sample.slot = r.u16();
      if (have_previous && sample.slot <= previous_slot)
        return transform_wire_status::slots_not_ascending;
    } else {
      // A dense run walking past the declared slot budget is refused by the
      // capacity check below before the counter could wrap.
      sample.slot = next_dense_slot++;
    }
    if (size_t(sample.slot) >= mirror.capacity()) return transform_wire_status::slot_out_of_range;
    if (!mirror.occupied(sample.slot)) return transform_wire_status::unknown_slot;

    for (uint8_t axis = 0; axis < fields.axes; ++axis) {
      sample.key[axis] = absolute_cell(origin[axis], int8_t(r.u8()));
      sample.code[axis] = r.u16();
    }
    if (fields.turn) sample.turn = r.u16();
    if (!r.good()) return transform_wire_status::truncated;
    if (out.size() == out.capacity()) return transform_wire_status::capacity_exceeded;
    out.push_back(sample);
    ++view.count;
    previous_slot = sample.slot;
    have_previous = true;
  }
  return transform_wire_status::ok;
}

} // namespace detail

[[nodiscard]] inline transform_wire_status try_decode_transform_frame(
  const std::span<const std::byte> bytes, const transform_layout_table& layouts,
  const relevant_set_mirror& mirror, const uint64_t receiver_tick,
  transform_frame_view& view, std::vector<transform_sample>& out) {
  const auto status =
    detail::decode_transform_frame_body(bytes, layouts, mirror, receiver_tick, view, out);
  if (status != transform_wire_status::ok) {
    out.clear();
    view.count = 0;
  }
  return status;
}

// ---------------------------------------------------------------------------
// Applying a frame: staleness, declared age, and the correction threshold.
// ---------------------------------------------------------------------------

enum class transform_frame_acceptance : uint8_t { accepted, duplicate, stale, too_far_ahead };

// Latest-value semantics per class. A frame older than the newest applied one
// carries nothing a receiver wants, so it is stale rather than useful — and
// staleness is per class, because two classes of different cadence legitimately
// sit at different ticks at the same moment.
class transform_frame_gate {
public:
  [[nodiscard]] std::optional<uint64_t> newest() const noexcept {
    return newest_;
  }

  [[nodiscard]] transform_frame_acceptance classify(
    const uint64_t tick, const uint64_t max_forward_ticks) const noexcept {
    if (!newest_) return transform_frame_acceptance::accepted;
    if (tick == *newest_) return transform_frame_acceptance::duplicate;
    if (tick < *newest_) return transform_frame_acceptance::stale;
    return tick - *newest_ <= max_forward_ticks ? transform_frame_acceptance::accepted
                                                : transform_frame_acceptance::too_far_ahead;
  }

  transform_frame_acceptance commit(const uint64_t tick,
                                    const uint64_t max_forward_ticks) noexcept {
    const auto result = classify(tick, max_forward_ticks);
    if (result == transform_frame_acceptance::accepted) newest_ = tick;
    return result;
  }

  void reset() noexcept {
    newest_.reset();
  }

  // The declared maximum staleness, made observable. Presentation extrapolating
  // past this bound is inventing motion, so the project needs to be told rather
  // than left to smooth over a class that stopped arriving.
  [[nodiscard]] bool exceeded_max_age(const uint64_t receiver_tick,
                                      const cadence_policy policy) const noexcept {
    if (policy.max_age_ticks == 0) return false;
    if (!newest_) return true;
    return receiver_tick > *newest_ &&
           receiver_tick - *newest_ > uint64_t(policy.max_age_ticks);
  }

private:
  std::optional<uint64_t> newest_;
};

// One quantum is the floor of the dead band, and this is the whole reason a
// shooter wants a fine quantum: snapping a local prediction onto a quantized
// authoritative value injects up to half a quantum even when nothing diverged,
// so a threshold below one quantum corrects noise the format itself created.
inline constexpr uint32_t minimum_correction_codes = 1;

struct correction_policy {
  uint32_t tolerance_codes = minimum_correction_codes;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return tolerance_codes >= minimum_correction_codes;
  }

  template <class Code>
  [[nodiscard]] constexpr double world_tolerance(const fixed_axis<Code> axis) const noexcept {
    return double(tolerance_codes) * axis.quantum();
  }
};

// The error between a local prediction and the authority, in exact quanta. The
// comparison happens in code space, so the threshold is exactly the declared
// quantum on every platform and no floating-point epsilon appears anywhere.
template <class Code>
[[nodiscard]] constexpr int64_t axis_error_codes(const fixed_axis<Code> axis,
                                                 const double predicted, const int32_t key,
                                                 const uint32_t code) noexcept {
  const auto split = split_axis(axis, predicted);
  return axis_codes<Code>(split.key, split.code) - axis_codes<Code>(key, code);
}

struct correction_verdict {
  int64_t worst_error_codes = 0; // signed, on the axis which diverged most
  uint8_t axis = 0;
  bool needs_correction = false;

  bool operator==(const correction_verdict&) const = default;
};

// Max-norm over the declared axes: one axis outside the dead band is a
// divergence even when the others agree, and reporting which axis it was is
// what makes a correction diagnosable instead of merely visible.
template <class Code>
[[nodiscard]] constexpr correction_verdict evaluate_correction(
  const fixed_axis<Code> axis, const uint8_t axes, const std::span<const double> predicted,
  const transform_sample& authoritative, const correction_policy policy) noexcept {
  correction_verdict verdict;
  int64_t worst_magnitude = -1;
  for (uint8_t index = 0; index < axes && index < predicted.size(); ++index) {
    const int64_t error = axis_error_codes(axis, predicted[index], authoritative.key[index],
                                           authoritative.code[index]);
    const int64_t magnitude = error < 0 ? -error : error;
    if (magnitude <= worst_magnitude) continue;
    worst_magnitude = magnitude;
    verdict.worst_error_codes = error;
    verdict.axis = index;
  }
  verdict.needs_correction = worst_magnitude > int64_t(policy.tolerance_codes);
  return verdict;
}

} // namespace devils_engine::network

#endif
