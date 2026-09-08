#ifndef DEVILS_ENGINE_NETLAB01_PROTOCOL_H
#define DEVILS_ENGINE_NETLAB01_PROTOCOL_H

#include "lab.h"

// The stand's own message classes. Byte zero is the type, because it decides
// how every following byte is read; the transport delivers whole messages, so
// the hot classes carry no length and the received size is authoritative.
//
// A bundle names its tick ABSOLUTELY, unlike an intent batch. A client
// compresses a tick into sixteen bits because it always has a nearby tick to
// reconstruct from; a follower replaying a gap has exactly the opposite
// problem, and reconstructing "nearest to my own tick" is what it must not do.

namespace netlab01 {

inline constexpr size_t lab_bundle_intent_bytes = 4;

[[nodiscard]] inline bool encode_bundle(const lab_bundle& bundle, std::vector<std::byte>& out) {
  out.clear();
  if (bundle.intents.size() > lab_max_intents_per_tick) return false;
  const size_t required = 1 + 8 + 1 + bundle.intents.size() * lab_bundle_intent_bytes;
  if (out.capacity() < required) return false;
  net::state_writer w(out, false);
  w.u8(uint8_t(lab_message::bundle));
  w.u64(bundle.tick);
  w.u8(uint8_t(bundle.intents.size()));
  for (const auto& value : bundle.intents) {
    w.u8(value.kind);
    w.u8(uint8_t(value.cell_delta[0]));
    w.u16(value.code[0]);
  }
  return w.good();
}

[[nodiscard]] inline bool decode_bundle(const std::span<const std::byte> bytes,
                                        lab_bundle& out) {
  net::state_reader r(bytes);
  if (r.u8() != uint8_t(lab_message::bundle)) return false;
  const auto tick = r.u64();
  const auto count = r.u8();
  if (!r.good() || count > lab_max_intents_per_tick) return false;
  out.tick = tick;
  out.intents.clear();
  if (out.intents.capacity() < count) return false;
  for (unsigned i = 0; i < count; ++i) {
    net::intent value;
    value.kind = r.u8();
    value.cell_delta[0] = int8_t(r.u8());
    value.code[0] = r.u16();
    if (!r.good()) return false;
    // The kind is checked here, at the boundary: an unknown kind inside a
    // canonical bundle is a protocol fault, never a project lookup miss.
    if (value.kind != lab_kind_move_to) return false;
    out.intents.push_back(value);
  }
  return r.position() == r.size();
}

[[nodiscard]] inline bool encode_grant(const lab_grant& grant, std::vector<std::byte>& out) {
  std::vector<std::byte> credential;
  credential.reserve(net::reconnect_credential_bytes);
  if (net::try_encode_credential(grant.credential, credential) != net::credential_status::accepted)
    return false;
  out.clear();
  const size_t required = 1 + 48 + net::reconnect_credential_bytes + net::credential_mac_bytes;
  if (out.capacity() < required) return false;
  net::state_writer w(out, false);
  w.u8(uint8_t(lab_message::reconnect_grant));
  w.u64(grant.issued_at);
  w.u64(grant.final_tick);
  w.u64(grant.tick_period_ms);
  w.u64(grant.suspect_after_ms);
  w.u64(grant.lost_after_ms);
  w.u64(grant.intent_lead_ticks);
  w.bytes(credential);
  w.bytes(grant.session_secret);
  return w.good();
}

[[nodiscard]] inline bool decode_grant(const std::span<const std::byte> bytes, lab_grant& out) {
  net::state_reader r(bytes);
  if (r.u8() != uint8_t(lab_message::reconnect_grant)) return false;
  const auto issued_at = r.u64();
  const auto final_tick = r.u64();
  const auto tick_period_ms = r.u64();
  const auto suspect_after_ms = r.u64();
  const auto lost_after_ms = r.u64();
  const auto intent_lead_ticks = r.u64();
  const auto credential = r.take(net::reconnect_credential_bytes);
  const auto secret = r.take(net::credential_mac_bytes);
  if (!r.good() || r.position() != r.size()) return false;
  net::reconnect_credential decoded;
  if (net::try_decode_credential(credential, decoded) != net::credential_status::accepted)
    return false;
  // An incoherent policy is refused at the boundary rather than handed to
  // `reconnect_policy::valid()` to discover later.
  if (tick_period_ms == 0 || suspect_after_ms == 0 || lost_after_ms <= suspect_after_ms ||
      intent_lead_ticks == 0)
    return false;
  out.issued_at = issued_at;
  out.final_tick = final_tick;
  out.tick_period_ms = tick_period_ms;
  out.suspect_after_ms = suspect_after_ms;
  out.lost_after_ms = lost_after_ms;
  out.intent_lead_ticks = intent_lead_ticks;
  out.credential = decoded;
  std::copy(secret.begin(), secret.end(), out.session_secret.begin());
  return true;
}

[[nodiscard]] inline bool encode_recovery_plan(const lab_recovery_plan& plan,
                                               std::vector<std::byte>& out) {
  out.clear();
  if (out.capacity() < 1 + 8 * 4 + 4) return false;
  net::state_writer w(out, false);
  w.u8(uint8_t(lab_message::recovery_plan));
  w.u64(plan.checkpoint_tick);
  w.u64(plan.target_tick);
  w.u64(plan.checkpoint_root);
  w.u64(plan.target_root);
  w.u32(plan.checkpoint_bytes);
  return w.good();
}

[[nodiscard]] inline bool decode_recovery_plan(const std::span<const std::byte> bytes,
                                               lab_recovery_plan& out) {
  net::state_reader r(bytes);
  if (r.u8() != uint8_t(lab_message::recovery_plan)) return false;
  lab_recovery_plan value;
  value.checkpoint_tick = r.u64();
  value.target_tick = r.u64();
  value.checkpoint_root = r.u64();
  value.target_root = r.u64();
  value.checkpoint_bytes = r.u32();
  if (!r.good() || r.position() != r.size()) return false;
  if (value.target_tick < value.checkpoint_tick) return false;
  out = value;
  return true;
}

[[nodiscard]] inline bool decode_recovery_unavailable(const std::span<const std::byte> bytes,
                                                      uint8_t& reason) {
  net::state_reader r(bytes);
  if (r.u8() != uint8_t(lab_message::recovery_unavailable)) return false;
  reason = r.u8();
  return r.good() && r.position() == r.size();
}

[[nodiscard]] inline bool encode_recovery_unavailable(const uint8_t reason,
                                                      std::vector<std::byte>& out) {
  out.clear();
  if (out.capacity() < 2) return false;
  net::state_writer w(out, false);
  w.u8(uint8_t(lab_message::recovery_unavailable));
  w.u8(reason);
  return w.good();
}

// The one bulk class, and the only one which carries its own 32-bit length:
// it slices something whose size is not a message size.
[[nodiscard]] inline bool encode_chunk(const uint32_t offset,
                                       const std::span<const std::byte> payload,
                                       std::vector<std::byte>& out) {
  out.clear();
  if (payload.size() > lab_checkpoint_chunk_bytes) return false;
  if (out.capacity() < 1 + 4 + 4 + payload.size()) return false;
  net::state_writer w(out, false);
  w.u8(uint8_t(lab_message::checkpoint_chunk));
  w.u32(offset);
  w.u32(uint32_t(payload.size()));
  w.bytes(payload);
  return w.good();
}

struct lab_chunk_view {
  uint32_t offset = 0;
  std::span<const std::byte> payload;
};

[[nodiscard]] inline bool decode_chunk(const std::span<const std::byte> bytes,
                                       lab_chunk_view& out) {
  net::state_reader r(bytes);
  if (r.u8() != uint8_t(lab_message::checkpoint_chunk)) return false;
  const auto offset = r.u32();
  const auto length = r.u32();
  if (!r.good() || length > lab_checkpoint_chunk_bytes) return false;
  const auto payload = r.take(length);
  if (!r.good() || r.position() != r.size()) return false;
  out.offset = offset;
  out.payload = payload;
  return true;
}

// The murmur64 root over canonical uncompressed state. NET-05's measurement is
// why this is the frequent diagnostic and SHA-256 is not.
inline uint64_t lab_root(const lab_host& host) {
  return net::make_state_digest<lab_schema, net::buffered_murmur64_state_hasher>(host).root;
}

} // namespace netlab01

#endif
