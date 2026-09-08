#ifndef DEVILS_ENGINE_NETLAB01_LAB_H
#define DEVILS_ENGINE_NETLAB01_LAB_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <devils_engine/network/network.h>
#include <devils_engine/network/credential.h>
#include <devils_engine/network/intent_wire.h>
#include <devils_engine/network/reconnect.h>
#include <devils_engine/network/session_wire.h>
#include <devils_engine/utils/sha256cpp.h>

// NET-LAB-01, shared between the authority and the follower process.
//
// The causal state here is INTEGERS ONLY, deliberately. This laboratory exists
// to attribute a mismatch to the protocol, and a float world would let every
// failure be blamed on `libm` instead. Numeric portability is the NUM branch's
// job; if this stand diverges, the bug is in the session, the transport or the
// codec, and nowhere else.
//
// The integers are not a toy substitute for a world coordinate either: they are
// exactly what the wire carries. A hot intent transports a cell key plus a code
// inside that cell, and `(key << 16) | code` is a signed count of quanta. The
// simulation stores that count, so the authority's decode seam performs no
// conversion at all and the only floating-point step in the whole path is the
// follower turning an authored target into a split, which is the one step
// HOT-01 already confined to `fixed_point.h`.

namespace netlab01 {
namespace net = devils_engine::network;
namespace utils = devils_engine::utils;

// ---------------------------------------------------------------- causal state

inline constexpr net::fixed_axis<uint16_t> lab_axis{.cell_shift = 6, .fraction_bits = 10};
static_assert(lab_axis.valid());

// One tick moves at most this many quanta toward the target.
inline constexpr int64_t lab_step_quanta = 96;

inline constexpr int64_t lab_max_quanta = int64_t(1) << 40;
inline constexpr int64_t lab_min_quanta = -lab_max_quanta;

struct lab_state {
  int64_t position = 0; // quanta
  int64_t target = 0;   // quanta
  uint64_t prng_cursor = 0;
  uint64_t tick = 0;

  bool operator==(const lab_state&) const = default;
};

struct lab_host {
  using staging_type = lab_state;
  lab_state state;
  // Not causal and not serialized: a replay which bumps this has published a
  // presentation effect it should have suppressed.
  uint64_t presentation_events = 0;
};

struct lab_section {
  static constexpr uint32_t id = 1, version = 1;

  static void write(const lab_host& host, net::state_writer& w) {
    w.u64(uint64_t(host.state.position));
    w.u64(uint64_t(host.state.target));
    w.u64(host.state.prng_cursor);
    w.u64(host.state.tick);
  }

  static bool read(lab_state& state, net::state_reader& r) {
    state.position = int64_t(r.u64());
    state.target = int64_t(r.u64());
    state.prng_cursor = r.u64();
    state.tick = r.u64();
    return r.good();
  }

  static bool validate(const lab_state& state) {
    // A checkpoint which cannot have been produced by this rule set is refused
    // before it replaces anything, exactly like project state would be.
    return state.position >= lab_min_quanta && state.position <= lab_max_quanta &&
           state.target >= lab_min_quanta && state.target <= lab_max_quanta;
  }
};

using lab_schema =
  net::state_schema<lab_host, net::state_writer, net::state_reader, lab_section>;

inline uint64_t splitmix64(uint64_t value) noexcept {
  value += UINT64_C(0x9e3779b97f4a7c15);
  uint64_t z = value;
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31);
}

// The one causal transition. Both processes run this and nothing else.
inline void lab_step(lab_host& host, const uint64_t tick) {
  auto& state = host.state;
  const int64_t remaining = state.target - state.position;
  if (remaining > 0) state.position += remaining < lab_step_quanta ? remaining : lab_step_quanta;
  else if (remaining < 0)
    state.position += -remaining < lab_step_quanta ? remaining : -lab_step_quanta;
  state.prng_cursor = splitmix64(state.prng_cursor ^ tick);
  state.tick = tick;
}

// ------------------------------------------------------------ canonical bundle

// The ordered, validated set of intents assigned to one tick. An explicitly
// empty bundle is a real bundle: replay needs a sealed bundle for every tick.
struct lab_bundle {
  uint64_t tick = 0;
  std::vector<net::intent> intents;

  bool operator==(const lab_bundle&) const = default;
};

struct lab_bundle_size {
  size_t operator()(const lab_bundle& bundle) const noexcept {
    return 8 + bundle.intents.size() * sizeof(net::intent);
  }
};

struct lab_blob_size {
  size_t operator()(const std::vector<std::byte>& bytes) const noexcept {
    return bytes.size();
  }
};

// The authority's ingress record. A wire intent carries a delta from the
// batch's base tick, never an absolute tick, so the absolute tick the ingress
// reconstructed has to travel beside it into the journal -- and so does the
// PRINCIPAL, which the connection established and the peer never asserted.
struct lab_intent_record {
  uint64_t tick = 0;
  uint64_t principal = 0;
  net::intent value;

  bool operator==(const lab_intent_record&) const = default;
};

struct lab_record_tick {
  uint64_t operator()(const lab_intent_record& record) const noexcept {
    return record.tick;
  }
};

// Semantic order inside one tick: PRINCIPAL first, then kind, then the target.
// With several followers this is the whole difference between a canonical
// bundle and a transcript of packet arrivals -- two peers whose packets
// interleave differently must still seal the same bytes, and only a total order
// keyed on something neither peer controls can promise that.
struct lab_record_less {
  bool operator()(const lab_intent_record& l, const lab_intent_record& r) const noexcept {
    if (l.principal != r.principal) return l.principal < r.principal;
    if (l.value.kind != r.value.kind) return l.value.kind < r.value.kind;
    if (l.value.cell_delta[0] != r.value.cell_delta[0])
      return l.value.cell_delta[0] < r.value.cell_delta[0];
    return l.value.code[0] < r.value.code[0];
  }
};
// One actor holds one intent of a kind per tick. A second one from the SAME
// principal is not a second order, it is the same order arriving twice; from a
// different principal it is a different order entirely. The ingress drops the
// former before the journal, so sealing never faults on a duplicate.
struct lab_record_equivalent {
  bool operator()(const lab_intent_record& l, const lab_intent_record& r) const noexcept {
    return l.principal == r.principal && l.value.kind == r.value.kind;
  }
};

using lab_journal = net::tick_journal<lab_intent_record, uint64_t, lab_record_tick,
                                      lab_record_less, lab_record_equivalent>;

// The single intent kind this laboratory declares: a 1-D quantized world point.
inline constexpr uint8_t lab_kind_move_to = 1;

inline net::intent_layout_table lab_layouts() {
  net::intent_layout_table table;
  const std::array<net::intent_kind_layout, 1> layouts{{
    {lab_kind_move_to, {.axes = 1, .reference = false, .target = false, .turn = false}},
  }};
  if (table.assign(layouts) != net::intent_wire_status::ok)
    utils::error{}("NET-LAB-01 intent layout table refused its own declaration");
  return table;
}

// Registered identifiers exist so the fingerprint below is a real value; the
// single kind above carries no reference field, which is exactly the point:
// the fingerprint refuses a peer with a different registry before tick one.
inline net::id_index_table lab_registry() {
  const std::array<uint64_t, 3> ids{
    UINT64_C(0x6c61625f6d6f7665), // "lab_move"
    UINT64_C(0x6c61625f73746f70), // "lab_stop"
    UINT64_C(0x6c61625f77616974), // "lab_wait"
  };
  net::id_index_table table;
  if (net::id_index_table::try_build(ids, table) != net::id_index_table::build_status::built)
    utils::error{}("NET-LAB-01 registry refused its own declaration");
  return table;
}

// The absolute quanta of a decoded intent. The receiver knows the base cell,
// so the wire only carried a delta from it.
inline int64_t lab_intent_quanta(const net::intent& value, const int32_t base_key) noexcept {
  const int32_t key = net::absolute_cell(base_key, value.cell_delta[0]);
  return (int64_t(key) << 16) | int64_t(value.code[0]);
}

inline net::intent lab_make_intent(const int64_t quanta, const int32_t base_key,
                                   const uint8_t tick_delta, bool& out_of_range) {
  const int32_t key = int32_t(quanta >> 16);
  const auto delta = net::relative_cell(base_key, key);
  out_of_range = delta.out_of_range;
  net::intent value;
  value.kind = lab_kind_move_to;
  value.tick_delta = tick_delta;
  value.cell_delta[0] = delta.value;
  value.code[0] = uint16_t(quanta & 0xffff);
  return value;
}

// ---------------------------------------------------------------- lab protocol
//
// A laboratory protocol, not an engine wire format. The HANDSHAKE is the
// engine's frozen SESSION-02 format and the intent batch is HOT-01's; only the
// classes below are the stand's own, and they occupy the authority and bulk
// ranges HOT-01 reserved so a type byte still decides everything.

enum class lab_message : uint8_t {
  // Authority classes, 64..191.
  bundle = 64,
  reconnect_grant = 65,
  recovery_plan = 66,
  recovery_unavailable = 67,
  // Bulk, 192..255.
  checkpoint_chunk = 192,
};

inline constexpr uint16_t lab_lane_control = 0; // reliable ordered
inline constexpr uint16_t lab_lane_bulk = 1;    // reliable, lower priority
inline constexpr uint16_t lab_lane_intent = 2;  // unreliable sequenced

inline constexpr size_t lab_max_message_bytes = 1024;
// Deliberately small. The point is not to model a megabyte: it is that the
// transfer takes several messages and is PACED against its lane budget, so the
// current bundles can overtake it and the follower's deferral path is a
// measured property rather than a claim.
inline constexpr size_t lab_checkpoint_chunk_bytes = 16;
// One chunk per owner pass. A sender which blasts the whole checkpoint into the
// lane has not used the budget the lane declares.
inline constexpr size_t lab_chunks_per_pass = 1;
inline constexpr size_t lab_max_intents_per_tick = 4;

// How many recent ticks an intent batch repeats. Redundancy, not retransmission:
// a lost packet costs nothing because the next two carry the same intents.
inline constexpr uint8_t lab_intent_window = 3;

struct lab_grant {
  net::reconnect_credential credential;
  net::credential_mac session_secret{};
  // The authority's own instant at issue. A client anchors its clock to this,
  // because `expires_at` is in the authority's units and two machines' monotonic
  // clocks share no origin.
  uint64_t issued_at = 0;
  // Where this run ends, as the AUTHORITY currently intends it. A follower
  // cannot hold this as a constant: a peer which rejoins late needs the run
  // extended so that it plays again rather than merely catching up, and a
  // session's length was never a client-side fact anyway.
  uint64_t final_tick = 0;
};

struct lab_recovery_plan {
  uint64_t checkpoint_tick = 0;
  uint64_t target_tick = 0;
  uint64_t checkpoint_root = 0;
  uint64_t target_root = 0;
  uint32_t checkpoint_bytes = 0;
};

// --------------------------------------------------------------- lab MAC policy

// HMAC-SHA256 over the engine's own SHA-256. A concrete primitive belongs in a
// consumer, never in `credential.h`.
inline net::credential_mac hmac_sha256(const std::span<const std::byte> key,
                                       const std::span<const std::byte> message) {
  constexpr size_t block_bytes = 64;
  std::array<std::byte, block_bytes> padded{};
  if (key.size() > block_bytes) {
    utils::SHA256 shrink;
    shrink.update(key.data(), key.size());
    const auto digest = shrink.finalize();
    for (size_t i = 0; i < digest.size(); ++i) padded[i] = std::byte(digest[i]);
  } else {
    for (size_t i = 0; i < key.size(); ++i) padded[i] = key[i];
  }

  std::array<std::byte, block_bytes> inner_pad{}, outer_pad{};
  for (size_t i = 0; i < block_bytes; ++i) {
    const auto value = std::to_integer<unsigned char>(padded[i]);
    inner_pad[i] = std::byte(static_cast<unsigned char>(value ^ 0x36u));
    outer_pad[i] = std::byte(static_cast<unsigned char>(value ^ 0x5cu));
  }

  utils::SHA256 inner;
  inner.update(inner_pad.data(), inner_pad.size());
  if (!message.empty()) inner.update(message.data(), message.size());
  const auto inner_digest = inner.finalize();

  utils::SHA256 outer;
  outer.update(outer_pad.data(), outer_pad.size());
  outer.update(inner_digest.data(), inner_digest.size());
  const auto outer_digest = outer.finalize();

  net::credential_mac result{};
  for (size_t i = 0; i < result.size(); ++i) result[i] = std::byte(outer_digest[i]);
  return result;
}

inline constexpr uint32_t lab_issuer = UINT32_C(0x4c414231); // "LAB1"

// The authority key. In a real deployment it is loaded from a keystore and must
// not live beside the tickets it signs; a fixed laboratory key is a laboratory
// key, and the follower process deliberately does not hold it.
struct lab_authority_mac {
  std::array<std::byte, 32> authority_key{};

  lab_authority_mac() {
    for (size_t i = 0; i < authority_key.size(); ++i)
      authority_key[i] = std::byte(uint8_t(0x51u + 3u * i));
  }

  bool authority_mac(const uint32_t issuer, const std::span<const std::byte> message,
                     net::credential_mac& out) {
    if (issuer != lab_issuer) return false;
    out = hmac_sha256(authority_key, message);
    return true;
  }

  bool keyed_mac(const net::credential_mac& key, const std::span<const std::byte> message,
                 net::credential_mac& out) {
    out = hmac_sha256(key, message);
    return true;
  }
};

// The follower holds no authority key at all: it can only key a MAC with the
// derived session secret it was handed. `authority_mac` returning false is the
// truth about a client, not a stub.
struct lab_follower_mac {
  bool authority_mac(const uint32_t, const std::span<const std::byte>, net::credential_mac&) {
    return false;
  }

  bool keyed_mac(const net::credential_mac& key, const std::span<const std::byte> message,
                 net::credential_mac& out) {
    out = hmac_sha256(key, message);
    return true;
  }
};

static_assert(net::credential_mac_policy<lab_authority_mac>);
static_assert(net::credential_mac_policy<lab_follower_mac>);

// The join credential stays an injected policy, per SESSION-01/03. This stand's
// policy is a declared roster of laboratory tokens: enough to prove the seam
// exists and to give each follower a distinct identity, and deliberately not a
// design for real identity.
//
// Identity comes from the CREDENTIAL, not from a field a client fills in. A
// follower which could name its own principal could name someone else's, which
// is the same forgery as naming its own actor.
inline constexpr size_t lab_max_followers = 3;

inline constexpr std::array<std::string_view, lab_max_followers> lab_join_tokens{
  "netlab01-join-token-0",
  "netlab01-join-token-1",
  "netlab01-join-token-2",
};

// Principals are the authority's own naming of who holds a session. They are
// derived from the roster position, so a token maps to exactly one principal.
inline constexpr uint64_t lab_principal_of(const size_t index) noexcept {
  return UINT64_C(0x70726e63'6c616230) + index; // "prnclab0" + i
}

inline std::optional<size_t> lab_roster_index(const std::span<const std::byte> credential) {
  for (size_t index = 0; index < lab_join_tokens.size(); ++index) {
    const auto& token = lab_join_tokens[index];
    const std::span<const std::byte> expected{
      reinterpret_cast<const std::byte*>(token.data()), token.size()};
    if (credential.size() != expected.size()) continue;
    if (net::equal_in_constant_time(credential, expected)) return index;
  }
  return std::nullopt;
}

// The declared numeric profile: the axis split, the code width and the causal
// step. Every value here changes what a code MEANS, so every value belongs in
// the fingerprint which refuses a differing peer before the first tick.
inline uint32_t lab_numeric_profile() {
  uint32_t value = UINT32_C(0x4c414231); // "LAB1": integer causal state
  value = value * UINT32_C(16777619) ^ uint32_t(lab_axis.cell_shift);
  value = value * UINT32_C(16777619) ^ uint32_t(lab_axis.fraction_bits);
  value = value * UINT32_C(16777619) ^ uint32_t(net::fixed_axis<uint16_t>::code_bits);
  value = value * UINT32_C(16777619) ^ uint32_t(lab_step_quanta);
  return value;
}

// ------------------------------------------------------------------- verifier

struct verifier {
  size_t checks = 0;

  void require(const bool condition, const std::string_view message) {
    if (!condition) utils::error{}("NET-LAB-01: {}", message);
    ++checks;
  }
};

// ------------------------------------------------------------- compatibility

// Every field a real project would fill from its own content and schema. The
// content root is a fixed laboratory value rather than a file scan: what this
// stand proves is that a mismatch is refused before tick one, and it proves
// that better with a value a test can perturb on purpose.
inline net::session_compatibility lab_compatibility(const uint32_t content_salt = 0) {
  net::session_compatibility value;
  value.handshake_format = 1;
  value.protocol_version = 1;
  value.state_schema_fingerprint = lab_schema::schema_fingerprint();
  value.intent_schema_fingerprint = lab_registry().fingerprint();
  // The quantum is SESSION IDENTITY. Two peers with different quanta decode the
  // same codes into different world values -- a systematic, silent
  // disagreement no digest can localize, because it appears in the state and
  // not in the codec. A literal profile number would have let such a peer
  // through the handshake, so the profile is DERIVED from what is declared.
  value.numeric_profile = lab_numeric_profile();
  utils::SHA256 hash;
  const std::string_view label = "netlab01-content-root";
  hash.update(label.data(), label.size());
  hash.update(&content_salt, sizeof(content_salt));
  value.content_root = hash.finalize();
  return value;
}

} // namespace netlab01

#endif
