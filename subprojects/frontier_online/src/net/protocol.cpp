#include "net/protocol.h"

#include <algorithm>
#include <cmath>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/hmac.h>
#include <devils_engine/utils/serialization.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/type_traits.h> // murmur_hash64A

#include "core/actor_checkpoint.h"
#include "net/actor_component_wire.h"
#include "net/link.h"
#include "net/player_intent_wire.h"

namespace frontier_online {
namespace net {

namespace serial = devils_engine::utils::serial;
namespace utils = devils_engine::utils;

network::session_compatibility local_compatibility(const core::causal_content& content) {
  network::component_wire_table components;
  if (const auto status = declare_actor_components(components);
      status != network::component_wire_status::ok)
    utils::error{}("frontier_online protocol: actor component wire declarations refused (status {})",
                   unsigned(status));
  components.freeze();
  network::session_compatibility value;
  value.protocol_version = protocol_version;
  value.state_schema_fingerprint = core::actor_checkpoint_schema_fingerprint();
  value.intent_schema_fingerprint = player_intent_schema_fingerprint;
  value.component_schema_fingerprint = components.fingerprint();
  value.numeric_profile = numeric_profile;
  value.content_root = content.manifest().root;
  return value;
}

uint8_t peek_message_type(const std::span<const std::byte> bytes) noexcept {
  return bytes.empty() ? uint8_t(0) : uint8_t(bytes[0]);
}

namespace {

// Общий вход всех кодировщиков: буфер очищается, ёмкость обеспечивается один раз, дальше writer
// РАСТИТЬ не имеет права. Сообщение, не влезшее в объявленный бюджет, — это ошибка, а не повод
// перевыделить память посреди отправки.
serial::state_writer begin(std::vector<std::byte>& out, const size_t required,
                           const message type) {
  out.clear();
  out.reserve(required);
  serial::state_writer w(out, false);
  w.u8(uint8_t(type));
  return w;
}

bool finish(const serial::state_writer& w, const std::vector<std::byte>& out,
            const size_t required) {
  return w.good() && out.size() <= required;
}

} // namespace

uint64_t terrain_probe_root(core::terrain_source& source) {
  const auto chunk = source.generate(core::chunk_coord{0, 0});
  std::string bytes;
  bytes.reserve(chunk.tiles.size());
  for (const auto& tile : chunk.tiles) bytes.push_back(char(tile.terrain));
  return devils_engine::utils::murmur_hash64A(bytes);
}

world_declaration declare_world(core::terrain_source& source, const std::string& generator) {
  world_declaration value;
  value.world_seed = source.world_seed();
  value.chunk_size = source.chunk_size();
  value.generator_fingerprint = source.fingerprint();
  value.probe_root = terrain_probe_root(source);
  value.generator = generator;
  return value;
}

bool encode(const world_declaration& value, std::vector<std::byte>& out) {
  if (value.generator.size() > 255 || value.generator.empty()) return false;
  const size_t required = 1 + 8 + 4 + 8 + 8 + 1 + value.generator.size();
  if (required > control_message_bytes) return false;
  auto w = begin(out, required, message::world_declaration);
  w.u64(value.world_seed);
  w.u32(value.chunk_size);
  w.u64(value.generator_fingerprint);
  w.u64(value.probe_root);
  w.u8(uint8_t(value.generator.size()));
  w.raw(value.generator.data(), value.generator.size());
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, world_declaration& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::world_declaration)) return false;
  world_declaration value;
  value.world_seed = r.u64();
  value.chunk_size = r.u32();
  value.generator_fingerprint = r.u64();
  value.probe_root = r.u64();
  const auto length = r.u8();
  if (!r.good() || length == 0) return false;
  const auto text = r.take(length);
  if (!r.good() || r.position() != r.size()) return false;
  if (value.chunk_size == 0 || value.chunk_size > 256) return false;
  value.generator.assign(reinterpret_cast<const char*>(text.data()), text.size());
  out = std::move(value);
  return true;
}

bool encode(const checkpoint_begin& value, std::vector<std::byte>& out) {
  if (value.total_bytes == 0 || value.total_bytes > max_checkpoint_bytes) return false;
  const size_t required = 1 + 8 + 8 + 4 + 4;
  auto w = begin(out, required, message::checkpoint_begin);
  w.u64(value.tick);
  w.u64(value.root);
  w.u32(value.total_bytes);
  w.u32(value.schema_fingerprint);
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, checkpoint_begin& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::checkpoint_begin)) return false;
  checkpoint_begin value;
  value.tick = r.u64();
  value.root = r.u64();
  value.total_bytes = r.u32();
  value.schema_fingerprint = r.u32();
  if (!r.good() || r.position() != r.size()) return false;
  if (value.total_bytes == 0 || value.total_bytes > max_checkpoint_bytes) return false;
  out = value;
  return true;
}

bool encode_chunk(const uint32_t offset, const std::span<const std::byte> payload,
                  std::vector<std::byte>& out) {
  const size_t required = 1 + devils_engine::network::document_chunk_header_bytes + payload.size();
  auto w = begin(out, required, message::checkpoint_chunk);
  return devils_engine::network::write_document_chunk(w, offset, payload, chunk_payload_bytes) && finish(w, out, required);
}

bool decode_chunk(const std::span<const std::byte> bytes, chunk_view& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::checkpoint_chunk)) return false;
  return devils_engine::network::read_document_chunk(r, chunk_payload_bytes, out);
}

bool encode(const join_report& value, std::vector<std::byte>& out) {
  const size_t required = 1 + 1 + 8 * 4;
  auto w = begin(out, required, message::join_report);
  w.u8(uint8_t(value.status));
  w.u64(value.tick);
  w.u64(value.root);
  w.u64(value.world_fingerprint);
  w.u64(value.probe_root);
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, join_report& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::join_report)) return false;
  join_report value;
  const auto status = r.u8();
  value.tick = r.u64();
  value.root = r.u64();
  value.world_fingerprint = r.u64();
  value.probe_root = r.u64();
  if (!r.good() || r.position() != r.size()) return false;
  if (status > uint8_t(join_status::world_declaration_malformed)) return false;
  value.status = join_status(status);
  out = value;
  return true;
}

bool encode(const join_confirmation& value, std::vector<std::byte>& out) {
  out.clear();
  if (value.session == 0 || value.logical_peer == 0) return false;
  const size_t required = 1 + 8 * 4;
  auto w = begin(out, required, message::join_confirmed);
  w.u64(value.session);
  w.u64(value.logical_peer);
  w.u64(value.tick);
  w.u64(value.root);
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, join_confirmation& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::join_confirmed)) return false;
  join_confirmation value;
  value.session = r.u64();
  value.logical_peer = r.u64();
  value.tick = r.u64();
  value.root = r.u64();
  if (!r.good() || r.position() != r.size() || value.session == 0 ||
      value.logical_peer == 0) return false;
  out = value;
  return true;
}

bool encode(const catchup_marker& value, std::vector<std::byte>& out) {
  out.clear();
  if (value.session == 0 || value.logical_peer == 0 || value.tick == 0) return false;
  const size_t required = 1 + 8 * 3;
  auto w = begin(out, required, message::live_caught_up);
  w.u64(value.session);
  w.u64(value.logical_peer);
  w.u64(value.tick);
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, catchup_marker& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::live_caught_up)) return false;
  catchup_marker value;
  value.session = r.u64();
  value.logical_peer = r.u64();
  value.tick = r.u64();
  if (!r.good() || r.position() != r.size() || value.session == 0 ||
      value.logical_peer == 0 || value.tick == 0) return false;
  out = value;
  return true;
}

bool encode(const session_terminated& value, std::vector<std::byte>& out) {
  out.clear();
  if (uint8_t(value.reason) == 0 || uint8_t(value.reason) > termination_reason_max) return false;
  const size_t required = 1 + 1;
  auto w = begin(out, required, message::session_terminated);
  w.u8(uint8_t(value.reason));
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, session_terminated& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::session_terminated)) return false;
  const uint8_t reason = r.u8();
  if (!r.good() || r.position() != r.size() || reason == 0 || reason > termination_reason_max)
    return false;
  out.reason = termination_reason(reason);
  return true;
}

bool credential_policy::authority_mac(const uint32_t issuer, const std::span<const std::byte> message,
                                      network::credential_mac& out) const {
  if (authority_key == nullptr || issuer != credential_issuer) return false;
  out = utils::hmac_sha256(*authority_key, message);
  return true;
}

bool credential_policy::keyed_mac(const network::credential_mac& key,
                                  const std::span<const std::byte> message,
                                  network::credential_mac& out) const {
  out = utils::hmac_sha256(key, message);
  return true;
}

bool encode(const reconnect_grant& value, std::vector<std::byte>& out) {
  out.clear();
  if (value.session == 0 || value.logical_peer == 0 || value.hold_ms == 0) return false;
  std::vector<std::byte> credential;
  credential.reserve(network::reconnect_credential_bytes);
  if (network::try_encode_credential(value.credential, credential) !=
      network::credential_status::accepted)
    return false;
  const size_t required = 1 + 8 * 2 + credential.size() + value.secret.size() + 4;
  auto w = begin(out, required, message::reconnect_grant);
  w.u64(value.session);
  w.u64(value.logical_peer);
  w.bytes(credential);
  w.bytes(value.secret);
  w.u32(value.hold_ms);
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, reconnect_grant& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::reconnect_grant)) return false;
  reconnect_grant value;
  value.session = r.u64();
  value.logical_peer = r.u64();
  const auto credential = r.take(network::reconnect_credential_bytes);
  const auto secret = r.take(value.secret.size());
  value.hold_ms = r.u32();
  if (!r.good() || r.position() != r.size() || value.session == 0 || value.logical_peer == 0 ||
      value.hold_ms == 0 ||
      network::try_decode_credential(credential, value.credential) !=
        network::credential_status::accepted ||
      value.credential.ticket.session != value.session ||
      value.credential.ticket.principal != value.logical_peer)
    return false;
  std::copy(secret.begin(), secret.end(), value.secret.begin());
  out = value;
  return true;
}

bool encode(const resume_plan& value, std::vector<std::byte>& out) {
  out.clear();
  if (value.session == 0 || value.logical_peer == 0 || value.tick == 0 ||
      (value.mode != resume_mode::journal && value.mode != resume_mode::checkpoint))
    return false;
  const size_t required = 1 + 8 * 2 + 1 + 8 * 2;
  auto w = begin(out, required, message::resume_plan);
  w.u64(value.session);
  w.u64(value.logical_peer);
  w.u8(uint8_t(value.mode));
  w.u64(value.tick);
  w.u64(value.root);
  return finish(w, out, required);
}

bool decode(const std::span<const std::byte> bytes, resume_plan& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::resume_plan)) return false;
  resume_plan value;
  value.session = r.u64();
  value.logical_peer = r.u64();
  const uint8_t mode = r.u8();
  value.tick = r.u64();
  value.root = r.u64();
  if (!r.good() || r.position() != r.size() || value.session == 0 || value.logical_peer == 0 ||
      value.tick == 0 ||
      (mode != uint8_t(resume_mode::journal) && mode != uint8_t(resume_mode::checkpoint)))
    return false;
  value.mode = resume_mode(mode);
  out = value;
  return true;
}

bool encode_leave(std::vector<std::byte>& out) {
  out.clear();
  auto w = begin(out, 1, message::leave);
  return finish(w, out, 1);
}

bool is_leave(const std::span<const std::byte> bytes) noexcept {
  return bytes.size() == 1 && uint8_t(bytes[0]) == uint8_t(message::leave);
}

utils::digest anchor_digest(const uint64_t root) noexcept {
  utils::digest value{};
  for (size_t i = 0; i < 8; ++i) value[i] = uint8_t(root >> (8 * i));
  return value;
}

bool anchor_root(const utils::digest& digest, uint64_t& root) noexcept {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= uint64_t(digest[i]) << (8 * i);
  for (size_t i = 8; i < digest.size(); ++i)
    if (digest[i] != 0) return false;
  root = value;
  return true;
}

const char* describe(const join_status status) noexcept {
  switch (status) {
    case join_status::loaded: return "loaded";
    case join_status::checkpoint_refused: return "checkpoint refused by the loader";
    case join_status::root_mismatch: return "loaded, but the causal root differs";
    case join_status::world_fingerprint_mismatch: return "generator fingerprint differs";
    case join_status::world_probe_mismatch: return "same fingerprint, different land";
    case join_status::generator_missing: return "declared generator is not present";
    case join_status::handshake_refused: return "refused during the handshake";
    case join_status::world_declaration_malformed: return "world declaration is malformed";
  }
  return "unknown";
}

const char* describe(const termination_reason reason) noexcept {
  switch (reason) {
    case termination_reason::violations: return "protocol violations";
    case termination_reason::timeout: return "phase timeout";
    case termination_reason::history_exhausted: return "fell behind the retained history";
    case termination_reason::shutdown: return "authority shutdown";
    case termination_reason::disagreement: return "join report disagrees with the sent state";
  }
  return "unknown";
}

const char* describe(const network::session_refusal_reason reason) noexcept {
  using reason_t = network::session_refusal_reason;
  switch (reason) {
    case reason_t::none: return "none";
    case reason_t::handshake_format_mismatch: return "handshake format mismatch";
    case reason_t::protocol_version_mismatch: return "protocol version mismatch";
    case reason_t::content_mismatch: return "causal content mismatch";
    case reason_t::state_schema_mismatch: return "state schema mismatch";
    case reason_t::intent_schema_mismatch: return "intent schema mismatch";
    case reason_t::component_schema_mismatch: return "component schema mismatch";
    case reason_t::numeric_profile_mismatch: return "numeric profile mismatch";
    case reason_t::identity_rejected: return "identity rejected";
    case reason_t::malformed_message: return "malformed message";
    case reason_t::unexpected_message: return "unexpected message";
    case reason_t::unknown_session: return "unknown session";
    case reason_t::no_capacity: return "no capacity";
    case reason_t::unsupported_wire_version: return "unsupported wire version";
  }
  return "unknown";
}

} // namespace net
} // namespace frontier_online
