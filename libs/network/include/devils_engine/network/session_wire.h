#ifndef DEVILS_ENGINE_NETWORK_SESSION_WIRE_H
#define DEVILS_ENGINE_NETWORK_SESSION_WIRE_H

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "session.h"
#include "state_schema.h"

// The frozen session handshake format. Unlike session.h, which stays neutral
// about integer widths, a wire format cannot be templated: two installations
// must agree on exact bytes. Project identifiers are therefore carried as
// fixed 64-bit values and a project maps its own types onto them.
//
// The library owns the budgets below. They are protocol, not machine policy:
// a limit derived from local memory would let two installations disagree about
// what is a legal message, and the larger side would be refused by the smaller
// one with no useful reason. Nothing here knows a socket, a file or a
// credential format; challenge and credential bytes stay opaque.

namespace devils_engine::network {

inline constexpr std::uint32_t session_wire_magic = UINT32_C(0x4853574e); // "NWSH"
inline constexpr std::uint16_t session_wire_envelope_version = 1;
inline constexpr std::size_t session_wire_header_bytes = 12;
inline constexpr std::size_t session_wire_max_payload_bytes = 512;
inline constexpr std::size_t session_wire_max_message_bytes =
  session_wire_header_bytes + session_wire_max_payload_bytes;
inline constexpr std::size_t session_wire_max_credential_bytes = 256;
inline constexpr std::size_t session_wire_max_challenge_bytes = 128;
inline constexpr std::size_t session_nonce_bytes = 32;

using session_nonce = std::array<std::byte, session_nonce_bytes>;

enum class session_message_type : std::uint8_t {
  client_hello = 1,
  authority_challenge = 2,
  client_response = 3,
  session_accepted = 4,
  session_refused = 5
};

inline constexpr std::uint8_t session_message_type_max =
  std::uint8_t(session_message_type::session_refused);

enum class session_wire_status : std::uint8_t {
  ok,
  buffer_too_small,
  too_large,
  truncated,
  trailing_bytes,
  bad_magic,
  unsupported_envelope,
  unknown_message_type,
  invalid_field
};

// A refusal reason travels on the wire, so it is a stable numbering rather than
// a reuse of session_handshake_status. Wire-level reasons say that the exchange
// itself was wrong; the rest name one compatibility field.
enum class session_refusal_reason : std::uint8_t {
  none = 0,
  handshake_format_mismatch = 1,
  protocol_version_mismatch = 2,
  content_mismatch = 3,
  state_schema_mismatch = 4,
  intent_schema_mismatch = 5,
  numeric_profile_mismatch = 6,
  identity_rejected = 7,
  malformed_message = 8,
  unexpected_message = 9,
  unknown_session = 10,
  no_capacity = 11
};

inline constexpr std::uint8_t session_refusal_reason_max =
  std::uint8_t(session_refusal_reason::no_capacity);

// Decoded opaque spans point into the caller's received buffer. They are valid
// only while that buffer is unchanged; a consumer which keeps a credential must
// copy it.
struct client_hello {
  session_compatibility compatibility;
  session_nonce client_nonce{};
};

struct authority_challenge {
  session_compatibility compatibility;
  session_nonce authority_nonce{};
  std::span<const std::byte> challenge;
};

struct client_response {
  std::span<const std::byte> credential;
  // A resuming client names the session it believes it belongs to and, when it
  // has one, the newest checkpoint it can prove. The anchor is a hint: the
  // authority may answer from any retained checkpoint not later than its target.
  std::optional<std::uint64_t> resumed_session;
  std::optional<recovery_anchor<std::uint64_t, utils::digest>> confirmed;
};

struct session_accepted {
  std::uint64_t session = 0;
  std::uint64_t local_peer = 0;
  std::uint64_t authority_peer = 0;
  std::uint64_t authority_epoch = 0;
  std::uint64_t start_tick = 0;
};

struct session_refused {
  session_refusal_reason reason = session_refusal_reason::malformed_message;
};

struct session_wire_message {
  session_message_type type = session_message_type::client_hello;
  std::span<const std::byte> payload;
};

namespace detail {

inline void wire_begin(state_writer& w, const session_message_type type,
                       std::size_t& length_at) {
  w.u32(session_wire_magic);
  w.u16(session_wire_envelope_version);
  w.u8(std::uint8_t(type));
  w.u8(0); // Reserved; a nonzero value is a refusal, never a silent skip.
  length_at = w.position();
  w.u32(0);
}

[[nodiscard]] inline session_wire_status wire_end(state_writer& w,
                                                  const std::size_t length_at) {
  if (!w.good()) return session_wire_status::buffer_too_small;
  const std::size_t payload_end = length_at + 4;
  if (w.position() < payload_end) return session_wire_status::buffer_too_small;
  const std::size_t payload = w.position() - payload_end;
  if (payload > session_wire_max_payload_bytes) return session_wire_status::too_large;
  w.patch_u32(length_at, std::uint32_t(payload));
  return w.good() ? session_wire_status::ok : session_wire_status::buffer_too_small;
}

inline void write_compatibility(state_writer& w, const session_compatibility& value) {
  // handshake_format leads every message which carries compatibility, so a peer
  // reaches a precise refusal before any field whose meaning that format fixes.
  w.u32(value.handshake_format);
  w.u32(value.protocol_version);
  w.u32(value.state_schema_fingerprint);
  w.u32(value.intent_schema_fingerprint);
  w.u32(value.numeric_profile);
  for (const auto byte : value.content_root) w.u8(byte);
}

inline void read_compatibility(state_reader& r, session_compatibility& value) noexcept {
  value.handshake_format = r.u32();
  value.protocol_version = r.u32();
  value.state_schema_fingerprint = r.u32();
  value.intent_schema_fingerprint = r.u32();
  value.numeric_profile = r.u32();
  for (auto& byte : value.content_root) byte = r.u8();
}

inline void write_digest(state_writer& w, const utils::digest& value) {
  for (const auto byte : value) w.u8(byte);
}

inline void read_digest(state_reader& r, utils::digest& value) noexcept {
  for (auto& byte : value) byte = r.u8();
}

[[nodiscard]] inline session_wire_status finish_decode(const state_reader& r) noexcept {
  if (!r.good()) return session_wire_status::truncated;
  if (r.position() != r.size()) return session_wire_status::trailing_bytes;
  return session_wire_status::ok;
}

[[nodiscard]] inline session_wire_status begin_encode(std::vector<std::byte>& out) noexcept {
  out.clear();
  return out.capacity() >= session_wire_header_bytes ? session_wire_status::ok
                                                     : session_wire_status::buffer_too_small;
}

} // namespace detail

// Every encoder writes into prepared capacity and never grows the buffer: a
// session message which does not fit its declared budget is a fault, not a
// reallocation. Reserve session_wire_max_message_bytes once per output buffer.
[[nodiscard]] inline session_wire_status try_encode(const client_hello& message,
                                                    std::vector<std::byte>& out) {
  if (const auto status = detail::begin_encode(out); status != session_wire_status::ok)
    return status;
  state_writer w(out, false);
  std::size_t length_at = 0;
  detail::wire_begin(w, session_message_type::client_hello, length_at);
  detail::write_compatibility(w, message.compatibility);
  w.bytes(message.client_nonce);
  return detail::wire_end(w, length_at);
}

[[nodiscard]] inline session_wire_status try_encode(const authority_challenge& message,
                                                    std::vector<std::byte>& out) {
  if (message.challenge.size() > session_wire_max_challenge_bytes)
    return session_wire_status::too_large;
  if (const auto status = detail::begin_encode(out); status != session_wire_status::ok)
    return status;
  state_writer w(out, false);
  std::size_t length_at = 0;
  detail::wire_begin(w, session_message_type::authority_challenge, length_at);
  detail::write_compatibility(w, message.compatibility);
  w.bytes(message.authority_nonce);
  w.u32(std::uint32_t(message.challenge.size()));
  w.bytes(message.challenge);
  return detail::wire_end(w, length_at);
}

[[nodiscard]] inline session_wire_status try_encode(const client_response& message,
                                                    std::vector<std::byte>& out) {
  if (message.credential.size() > session_wire_max_credential_bytes)
    return session_wire_status::too_large;
  if (message.confirmed.has_value() && !message.resumed_session.has_value())
    return session_wire_status::invalid_field;
  if (const auto status = detail::begin_encode(out); status != session_wire_status::ok)
    return status;
  state_writer w(out, false);
  std::size_t length_at = 0;
  detail::wire_begin(w, session_message_type::client_response, length_at);
  w.u32(std::uint32_t(message.credential.size()));
  w.bytes(message.credential);
  w.u8(std::uint8_t(message.resumed_session.has_value()));
  w.u64(message.resumed_session.value_or(0));
  w.u8(std::uint8_t(message.confirmed.has_value()));
  w.u64(message.confirmed ? message.confirmed->tick : 0);
  detail::write_digest(w, message.confirmed ? message.confirmed->root : utils::digest{});
  return detail::wire_end(w, length_at);
}

[[nodiscard]] inline session_wire_status try_encode(const session_accepted& message,
                                                    std::vector<std::byte>& out) {
  if (const auto status = detail::begin_encode(out); status != session_wire_status::ok)
    return status;
  state_writer w(out, false);
  std::size_t length_at = 0;
  detail::wire_begin(w, session_message_type::session_accepted, length_at);
  w.u64(message.session);
  w.u64(message.local_peer);
  w.u64(message.authority_peer);
  w.u64(message.authority_epoch);
  w.u64(message.start_tick);
  return detail::wire_end(w, length_at);
}

[[nodiscard]] inline session_wire_status try_encode(const session_refused& message,
                                                    std::vector<std::byte>& out) {
  if (std::uint8_t(message.reason) > session_refusal_reason_max ||
      message.reason == session_refusal_reason::none)
    return session_wire_status::invalid_field;
  if (const auto status = detail::begin_encode(out); status != session_wire_status::ok)
    return status;
  state_writer w(out, false);
  std::size_t length_at = 0;
  detail::wire_begin(w, session_message_type::session_refused, length_at);
  w.u8(std::uint8_t(message.reason));
  return detail::wire_end(w, length_at);
}

// The envelope is validated before any payload field is read, and the declared
// payload length must account for the whole message: a shorter or longer buffer
// is a refusal, never a prefix accepted with trailing bytes ignored.
[[nodiscard]] inline session_wire_status try_peek_session_message(
  const std::span<const std::byte> bytes, session_wire_message& out) noexcept {
  if (bytes.size() > session_wire_max_message_bytes) return session_wire_status::too_large;
  if (bytes.size() < session_wire_header_bytes) return session_wire_status::truncated;
  state_reader r(bytes);
  if (r.u32() != session_wire_magic) return session_wire_status::bad_magic;
  if (r.u16() != session_wire_envelope_version) return session_wire_status::unsupported_envelope;
  const auto type = r.u8();
  if (type == 0 || type > session_message_type_max) return session_wire_status::unknown_message_type;
  if (r.u8() != 0) return session_wire_status::invalid_field;
  const auto length = r.u32();
  if (!r.good()) return session_wire_status::truncated;
  const std::size_t available = bytes.size() - session_wire_header_bytes;
  if (length > session_wire_max_payload_bytes) return session_wire_status::too_large;
  if (length > available) return session_wire_status::truncated;
  if (length < available) return session_wire_status::trailing_bytes;
  out.type = session_message_type(type);
  out.payload = bytes.subspan(session_wire_header_bytes, length);
  return session_wire_status::ok;
}

[[nodiscard]] inline session_wire_status try_decode(const std::span<const std::byte> payload,
                                                    client_hello& out) noexcept {
  state_reader r(payload);
  detail::read_compatibility(r, out.compatibility);
  const auto nonce = r.take(session_nonce_bytes);
  if (!r.good()) return session_wire_status::truncated;
  std::copy(nonce.begin(), nonce.end(), out.client_nonce.begin());
  return detail::finish_decode(r);
}

[[nodiscard]] inline session_wire_status try_decode(const std::span<const std::byte> payload,
                                                    authority_challenge& out) noexcept {
  state_reader r(payload);
  detail::read_compatibility(r, out.compatibility);
  const auto nonce = r.take(session_nonce_bytes);
  const auto size = r.u32();
  if (!r.good()) return session_wire_status::truncated;
  if (size > session_wire_max_challenge_bytes) return session_wire_status::too_large;
  const auto challenge = r.take(size);
  if (!r.good()) return session_wire_status::truncated;
  std::copy(nonce.begin(), nonce.end(), out.authority_nonce.begin());
  out.challenge = challenge;
  return detail::finish_decode(r);
}

[[nodiscard]] inline session_wire_status try_decode(const std::span<const std::byte> payload,
                                                    client_response& out) noexcept {
  state_reader r(payload);
  const auto size = r.u32();
  if (!r.good()) return session_wire_status::truncated;
  if (size > session_wire_max_credential_bytes) return session_wire_status::too_large;
  const auto credential = r.take(size);
  const auto has_session = r.u8();
  const auto session = r.u64();
  const auto has_anchor = r.u8();
  const auto tick = r.u64();
  utils::digest root{};
  detail::read_digest(r, root);
  if (!r.good()) return session_wire_status::truncated;
  // Absent optionals must be encoded as zero. Accepting an ignored payload here
  // would let two encoders produce different bytes for one logical message.
  if (has_session > 1 || has_anchor > 1) return session_wire_status::invalid_field;
  if (!has_session && session != 0) return session_wire_status::invalid_field;
  if (!has_anchor && (tick != 0 || root != utils::digest{}))
    return session_wire_status::invalid_field;
  if (has_anchor && !has_session) return session_wire_status::invalid_field;
  out.credential = credential;
  out.resumed_session = has_session ? std::optional(session) : std::nullopt;
  out.confirmed = has_anchor
                    ? std::optional(recovery_anchor<std::uint64_t, utils::digest>{tick, root})
                    : std::nullopt;
  return detail::finish_decode(r);
}

[[nodiscard]] inline session_wire_status try_decode(const std::span<const std::byte> payload,
                                                    session_accepted& out) noexcept {
  state_reader r(payload);
  out.session = r.u64();
  out.local_peer = r.u64();
  out.authority_peer = r.u64();
  out.authority_epoch = r.u64();
  out.start_tick = r.u64();
  if (!r.good()) return session_wire_status::truncated;
  return detail::finish_decode(r);
}

[[nodiscard]] inline session_wire_status try_decode(const std::span<const std::byte> payload,
                                                    session_refused& out) noexcept {
  state_reader r(payload);
  const auto reason = r.u8();
  if (!r.good()) return session_wire_status::truncated;
  if (reason == 0 || reason > session_refusal_reason_max)
    return session_wire_status::invalid_field;
  out.reason = session_refusal_reason(reason);
  return detail::finish_decode(r);
}

// The transcript binds a credential to this exchange. Both roles hash the exact
// bytes of the hello and the challenge, so a credential recorded from another
// session or another peer's nonce cannot be replayed into this one. The library
// does not decide how a credential uses the transcript; it only guarantees that
// both sides derive the same value from the same two messages.
class session_transcript {
public:
  void absorb(const std::span<const std::byte> message) {
    detail::sha_u64(hash, std::uint64_t(message.size()));
    if (!message.empty()) hash.update(message.data(), message.size());
    ++absorbed_;
  }

  [[nodiscard]] std::size_t absorbed() const noexcept {
    return absorbed_;
  }

  // Sealing is once-only: SHA-256 finalization consumes its state.
  [[nodiscard]] const utils::digest& seal() {
    if (!sealed_) {
      value_ = hash.finalize();
      sealed_ = true;
    }
    return value_;
  }

  [[nodiscard]] bool sealed() const noexcept {
    return sealed_;
  }

private:
  utils::SHA256 hash;
  utils::digest value_{};
  std::size_t absorbed_ = 0;
  bool sealed_ = false;
};

enum class handshake_phase : std::uint8_t {
  // Authority phases.
  awaiting_hello,
  awaiting_response,
  // Client phases.
  unsent,
  awaiting_challenge,
  awaiting_result,
  // Terminal for both roles.
  established,
  refused
};

// An authority policy owns the two decisions the library refuses to guess: what
// a challenge contains, and whether a credential names an admitted principal.
// `admit` must verify the credential against the transcript and return
// session_refusal_reason::none only when it also filled `accepted`.
template <class Policy>
concept authority_handshake_policy =
  requires(Policy& policy, const client_hello& hello, const client_response& response,
           const utils::digest& transcript, std::vector<std::byte>& challenge,
           session_accepted& accepted) {
    { policy.issue_challenge(hello, challenge) } -> std::same_as<bool>;
    { policy.admit(response, transcript, accepted) } -> std::same_as<session_refusal_reason>;
  };

template <class Policy>
concept client_handshake_policy =
  requires(Policy& policy, const authority_challenge& challenge,
           const utils::digest& transcript, std::vector<std::byte>& credential) {
    { policy.answer(challenge, transcript, credential) } -> std::same_as<bool>;
  };

namespace detail {

[[nodiscard]] inline session_refusal_reason refusal_of(
  const session_compatibility_status status) noexcept {
  switch (status) {
    case session_compatibility_status::compatible: return session_refusal_reason::none;
    case session_compatibility_status::handshake_format_mismatch:
      return session_refusal_reason::handshake_format_mismatch;
    case session_compatibility_status::protocol_version_mismatch:
      return session_refusal_reason::protocol_version_mismatch;
    case session_compatibility_status::content_mismatch:
      return session_refusal_reason::content_mismatch;
    case session_compatibility_status::state_schema_mismatch:
      return session_refusal_reason::state_schema_mismatch;
    case session_compatibility_status::intent_schema_mismatch:
      return session_refusal_reason::intent_schema_mismatch;
    case session_compatibility_status::numeric_profile_mismatch:
      return session_refusal_reason::numeric_profile_mismatch;
  }
  return session_refusal_reason::malformed_message;
}

} // namespace detail

// Both roles are ordered state machines over decoded messages. A message which
// does not belong to the current phase is refused as unexpected_message instead
// of being applied out of order, and any refusal is terminal: the exchange never
// returns to an earlier phase, so a peer cannot retry a rejected credential or
// renegotiate compatibility on one connection.
//
// Every refusal fills the reply buffer with one session_refused message, so a
// caller always has exactly one thing to send and one place to stop. Neither
// role touches a socket, a simulation or a tick.
//
// A returned status describes the decode, not the decision: malformed input
// returns its wire status, while a well-formed message which is refused on
// compatibility, identity or ordering returns `ok` and reports itself through
// `phase()`/`refusal()`. The received message and the reply must be distinct
// buffers; decoded credential and challenge spans point into the former.
class authority_handshake {
public:
  authority_handshake(const session_compatibility& local, const session_nonce& nonce) noexcept
    : local_(local), nonce_(nonce) {}

  [[nodiscard]] handshake_phase phase() const noexcept {
    return phase_;
  }
  [[nodiscard]] session_refusal_reason refusal() const noexcept {
    return refusal_;
  }
  [[nodiscard]] const session_accepted& accepted() const noexcept {
    return accepted_;
  }
  [[nodiscard]] bool established() const noexcept {
    return phase_ == handshake_phase::established;
  }

  template <authority_handshake_policy Policy>
  [[nodiscard]] session_wire_status consume(const std::span<const std::byte> message,
                                            std::vector<std::byte>& reply,
                                            Policy& policy) {
    session_wire_message envelope;
    if (const auto status = try_peek_session_message(message, envelope);
        status != session_wire_status::ok)
      return refuse(reply, session_refusal_reason::malformed_message, status);

    switch (phase_) {
      case handshake_phase::awaiting_hello:
        if (envelope.type != session_message_type::client_hello)
          return refuse(reply, session_refusal_reason::unexpected_message,
                        session_wire_status::ok);
        return on_hello(message, envelope.payload, reply, policy);
      case handshake_phase::awaiting_response:
        if (envelope.type != session_message_type::client_response)
          return refuse(reply, session_refusal_reason::unexpected_message,
                        session_wire_status::ok);
        return on_response(envelope.payload, reply, policy);
      default:
        return refuse(reply, session_refusal_reason::unexpected_message,
                      session_wire_status::ok);
    }
  }

private:
  template <authority_handshake_policy Policy>
  session_wire_status on_hello(const std::span<const std::byte> message,
                               const std::span<const std::byte> payload,
                               std::vector<std::byte>& reply, Policy& policy) {
    client_hello hello;
    if (const auto status = try_decode(payload, hello); status != session_wire_status::ok)
      return refuse(reply, session_refusal_reason::malformed_message, status);

    // Compatibility is answered before the policy sees anything: an installation
    // mismatch must not consume a challenge or reach a credential check.
    const auto compatibility = compare_session_compatibility(local_, hello.compatibility);
    if (compatibility != session_compatibility_status::compatible)
      return refuse(reply, detail::refusal_of(compatibility), session_wire_status::ok);

    challenge_.clear();
    if (!policy.issue_challenge(hello, challenge_))
      return refuse(reply, session_refusal_reason::identity_rejected, session_wire_status::ok);
    if (challenge_.size() > session_wire_max_challenge_bytes)
      return refuse(reply, session_refusal_reason::malformed_message,
                    session_wire_status::too_large);

    // The received message is absorbed before the reply is written: a caller
    // which passes one buffer for both would otherwise hash the reply twice.
    transcript_.absorb(message);
    const authority_challenge answer{local_, nonce_, challenge_};
    if (const auto status = try_encode(answer, reply); status != session_wire_status::ok) {
      phase_ = handshake_phase::refused;
      refusal_ = session_refusal_reason::malformed_message;
      return status;
    }
    transcript_.absorb(reply);
    phase_ = handshake_phase::awaiting_response;
    return session_wire_status::ok;
  }

  template <authority_handshake_policy Policy>
  session_wire_status on_response(const std::span<const std::byte> payload,
                                  std::vector<std::byte>& reply, Policy& policy) {
    client_response response;
    if (const auto status = try_decode(payload, response); status != session_wire_status::ok)
      return refuse(reply, session_refusal_reason::malformed_message, status);

    session_accepted granted;
    const auto reason = policy.admit(response, transcript_.seal(), granted);
    if (reason != session_refusal_reason::none)
      return refuse(reply, reason, session_wire_status::ok);
    if (const auto status = try_encode(granted, reply); status != session_wire_status::ok) {
      phase_ = handshake_phase::refused;
      refusal_ = session_refusal_reason::malformed_message;
      return status;
    }
    accepted_ = granted;
    phase_ = handshake_phase::established;
    return session_wire_status::ok;
  }

  session_wire_status refuse(std::vector<std::byte>& reply,
                             const session_refusal_reason reason,
                             const session_wire_status status) {
    phase_ = handshake_phase::refused;
    refusal_ = reason;
    const auto encoded = try_encode(session_refused{reason}, reply);
    if (encoded != session_wire_status::ok) reply.clear();
    return status == session_wire_status::ok ? session_wire_status::ok : status;
  }

  session_compatibility local_;
  session_nonce nonce_;
  session_transcript transcript_;
  std::vector<std::byte> challenge_;
  session_accepted accepted_;
  handshake_phase phase_ = handshake_phase::awaiting_hello;
  session_refusal_reason refusal_ = session_refusal_reason::none;
};

class client_handshake {
public:
  client_handshake(const session_compatibility& local, const session_nonce& nonce,
                   const std::optional<std::uint64_t> resumed_session = std::nullopt,
                   const std::optional<recovery_anchor<std::uint64_t, utils::digest>> confirmed =
                     std::nullopt) noexcept
    : local_(local), nonce_(nonce), resumed_session_(resumed_session), confirmed_(confirmed) {}

  [[nodiscard]] handshake_phase phase() const noexcept {
    return phase_;
  }
  [[nodiscard]] session_refusal_reason refusal() const noexcept {
    return refusal_;
  }
  [[nodiscard]] const session_accepted& accepted() const noexcept {
    return accepted_;
  }
  [[nodiscard]] bool established() const noexcept {
    return phase_ == handshake_phase::established;
  }

  // The hello bytes stay in the caller's buffer, but the transcript keeps their
  // hash, so the caller may reuse that buffer for the next message.
  [[nodiscard]] session_wire_status start(std::vector<std::byte>& out) {
    if (phase_ != handshake_phase::unsent) return session_wire_status::invalid_field;
    const client_hello hello{local_, nonce_};
    if (const auto status = try_encode(hello, out); status != session_wire_status::ok)
      return status;
    transcript_.absorb(out);
    phase_ = handshake_phase::awaiting_challenge;
    return session_wire_status::ok;
  }

  template <client_handshake_policy Policy>
  [[nodiscard]] session_wire_status consume(const std::span<const std::byte> message,
                                            std::vector<std::byte>& reply, Policy& policy) {
    session_wire_message envelope;
    if (const auto status = try_peek_session_message(message, envelope);
        status != session_wire_status::ok) {
      stop(session_refusal_reason::malformed_message);
      return status;
    }

    if (envelope.type == session_message_type::session_refused) {
      session_refused refused;
      if (const auto status = try_decode(envelope.payload, refused);
          status != session_wire_status::ok) {
        stop(session_refusal_reason::malformed_message);
        return status;
      }
      stop(refused.reason);
      return session_wire_status::ok;
    }

    switch (phase_) {
      case handshake_phase::awaiting_challenge:
        if (envelope.type != session_message_type::authority_challenge) {
          return refuse(reply, session_refusal_reason::unexpected_message);
        }
        return on_challenge(message, envelope.payload, reply, policy);
      case handshake_phase::awaiting_result:
        if (envelope.type != session_message_type::session_accepted) {
          return refuse(reply, session_refusal_reason::unexpected_message);
        }
        return on_accepted(envelope.payload, reply);
      default:
        return refuse(reply, session_refusal_reason::unexpected_message);
    }
  }

private:
  template <client_handshake_policy Policy>
  session_wire_status on_challenge(const std::span<const std::byte> message,
                                   const std::span<const std::byte> payload,
                                   std::vector<std::byte>& reply, Policy& policy) {
    authority_challenge challenge;
    if (const auto status = try_decode(payload, challenge); status != session_wire_status::ok) {
      stop(session_refusal_reason::malformed_message);
      return status;
    }

    // The client checks compatibility too. Otherwise a follower would answer a
    // challenge from an installation it can never simulate with, and would learn
    // the reason only from the authority's refusal.
    const auto compatibility = compare_session_compatibility(local_, challenge.compatibility);
    if (compatibility != session_compatibility_status::compatible)
      return refuse(reply, detail::refusal_of(compatibility));

    transcript_.absorb(message);
    credential_.clear();
    if (!policy.answer(challenge, transcript_.seal(), credential_))
      return refuse(reply, session_refusal_reason::identity_rejected);
    if (credential_.size() > session_wire_max_credential_bytes) {
      stop(session_refusal_reason::malformed_message);
      return session_wire_status::too_large;
    }

    const client_response response{credential_, resumed_session_, confirmed_};
    if (const auto status = try_encode(response, reply); status != session_wire_status::ok) {
      stop(session_refusal_reason::malformed_message);
      return status;
    }
    phase_ = handshake_phase::awaiting_result;
    return session_wire_status::ok;
  }

  session_wire_status on_accepted(const std::span<const std::byte> payload,
                                  std::vector<std::byte>& reply) {
    session_accepted granted;
    if (const auto status = try_decode(payload, granted); status != session_wire_status::ok) {
      stop(session_refusal_reason::malformed_message);
      return status;
    }
    if (resumed_session_.has_value() && granted.session != *resumed_session_)
      return refuse(reply, session_refusal_reason::unknown_session);
    accepted_ = granted;
    phase_ = handshake_phase::established;
    reply.clear();
    return session_wire_status::ok;
  }

  void stop(const session_refusal_reason reason) noexcept {
    phase_ = handshake_phase::refused;
    refusal_ = reason;
  }

  session_wire_status refuse(std::vector<std::byte>& reply,
                             const session_refusal_reason reason) {
    stop(reason);
    if (try_encode(session_refused{reason}, reply) != session_wire_status::ok) reply.clear();
    return session_wire_status::ok;
  }

  session_compatibility local_;
  session_nonce nonce_;
  std::optional<std::uint64_t> resumed_session_;
  std::optional<recovery_anchor<std::uint64_t, utils::digest>> confirmed_;
  session_transcript transcript_;
  std::vector<std::byte> credential_;
  session_accepted accepted_;
  handshake_phase phase_ = handshake_phase::unsent;
  session_refusal_reason refusal_ = session_refusal_reason::none;
};

} // namespace devils_engine::network

#endif
