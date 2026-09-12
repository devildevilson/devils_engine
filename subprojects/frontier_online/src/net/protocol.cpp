#include "net/protocol.h"

#include <devils_engine/utils/serialization.h>
#include <devils_engine/utils/type_traits.h> // murmur_hash64A

#include "core/actor_checkpoint.h"
#include "net/link.h"

namespace frontier_online {
namespace net {

namespace serial = devils_engine::utils::serial;

network::session_compatibility local_compatibility(const core::causal_content& content) {
  network::session_compatibility value;
  value.handshake_format = 1;
  value.protocol_version = protocol_version;
  value.state_schema_fingerprint = core::actor_checkpoint_schema_fingerprint();
  value.intent_schema_fingerprint = intent_schema_fingerprint;
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
  if (value.total_bytes == 0) return false;
  out = value;
  return true;
}

bool encode_chunk(const uint32_t offset, const std::span<const std::byte> payload,
                  std::vector<std::byte>& out) {
  if (payload.empty() || payload.size() > chunk_payload_bytes) return false;
  const size_t required = 1 + 4 + 4 + payload.size();
  auto w = begin(out, required, message::checkpoint_chunk);
  w.u32(offset);
  w.u32(uint32_t(payload.size()));
  w.bytes(payload);
  return finish(w, out, required);
}

bool decode_chunk(const std::span<const std::byte> bytes, chunk_view& out) {
  serial::state_reader r(bytes);
  if (r.u8() != uint8_t(message::checkpoint_chunk)) return false;
  const auto offset = r.u32();
  const auto length = r.u32();
  if (!r.good() || length == 0 || length > chunk_payload_bytes) return false;
  const auto payload = r.take(length);
  if (!r.good() || r.position() != r.size()) return false;
  out.offset = offset;
  out.payload = payload;
  return true;
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
  if (status > uint8_t(join_status::handshake_refused)) return false;
  value.status = join_status(status);
  out = value;
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
