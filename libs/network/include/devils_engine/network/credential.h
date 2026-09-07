#ifndef DEVILS_ENGINE_NETWORK_CREDENTIAL_H
#define DEVILS_ENGINE_NETWORK_CREDENTIAL_H

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "session_wire.h"

// The reconnect credential, and only that one.
//
// A JOIN credential proves who a stranger is. Which authority vouches for that
// — a platform identity, an offline keystore, a dedicated-server token — is
// policy, and it stays an injected verifier in session.h. The engine must not
// own it.
//
// A RECONNECT credential is different in kind: the authority mints it for
// itself at admission and must be able to verify it ALONE, without the external
// identity service, because that service can be unreachable exactly when a
// reconnect is needed. Nobody but the engine can own that, so it lives here.
//
// The credential carries two independent proofs, and conflating them is the
// classic reconnect hole:
//
//   * `ticket_mac` says WHAT the bearer is entitled to. The authority keyed it
//     with its own key and can check it with no third party. It is replayable
//     on its own, deliberately: it is a statement about entitlement, not about
//     who is speaking now.
//   * `presentation_mac` says the bearer is presenting it in THIS exchange. It
//     is keyed by a secret only the authority and that client know, over the
//     handshake transcript, which contains both nonces. A passive observer who
//     captured a whole credential therefore cannot use it in an exchange of
//     their own.
//
// The library owns the message construction, the order of the checks and the
// comparisons. The MAC primitive is injected: no cryptographic algorithm is
// chosen here.

namespace devils_engine::network {

inline constexpr size_t credential_mac_bytes = 32;
using credential_mac = std::array<std::byte, credential_mac_bytes>;

inline constexpr uint8_t reconnect_credential_format = 1;
inline constexpr size_t reconnect_ticket_field_bytes = 1 + 4 + 8 + 8 + 8 + 8 + 8;
inline constexpr size_t reconnect_credential_bytes =
  reconnect_ticket_field_bytes + 2 * credential_mac_bytes;
static_assert(reconnect_credential_bytes <= session_wire_max_credential_bytes,
              "a reconnect credential must fit the declared handshake credential budget");

// Domain tags keep a MAC computed for one purpose from being accepted as
// another. Without them a derived secret and a ticket signature are two strings
// of bytes under the same key, and an attacker gets to choose which is which.
inline constexpr uint32_t credential_domain_ticket = UINT32_C(0x4b435456);      // "VTCK"
inline constexpr uint32_t credential_domain_secret = UINT32_C(0x43455353);      // "SSEC"
inline constexpr uint32_t credential_domain_presentation = UINT32_C(0x53455250); // "PRES"

struct reconnect_ticket {
  uint64_t session = 0;
  uint64_t principal = 0;
  uint64_t authority_epoch = 0;
  // Caller-declared instants. The library never reads a clock, and only the
  // AUTHORITY's instant decides admission: a bearer's clock is not evidence.
  uint64_t issued_at = 0;
  uint64_t expires_at = 0;
  uint32_t issuer = 0;

  bool operator==(const reconnect_ticket&) const = default;
};

struct reconnect_credential {
  reconnect_ticket ticket;
  credential_mac ticket_mac{};
  credential_mac presentation_mac{};

  bool operator==(const reconnect_credential&) const = default;
};

// What the authority requires of a presented credential. Session, principal and
// epoch come from the authority's own state, never from the credential.
struct reconnect_expectation {
  uint64_t session = 0;
  uint64_t principal = 0;
  uint64_t authority_epoch = 0;
};

enum class credential_status : uint8_t {
  accepted,
  malformed,
  unknown_issuer,
  not_yet_valid,
  expired,
  wrong_session,
  wrong_principal,
  stale_epoch,
  future_epoch,
  ticket_mac_invalid,
  presentation_mac_invalid,
  mac_unavailable
};

// The MAC primitive, injected. `authority_mac` is keyed by the authority key
// named by an issuer identifier and returns false for an issuer it does not
// hold; `keyed_mac` is keyed by a value the library derived. Two operations
// rather than one, because the authority key must never leave the authority
// while the derived secret is handed to a client on purpose.
template <class Policy>
concept credential_mac_policy =
  requires(Policy& policy, const uint32_t issuer, const std::span<const std::byte> message,
           credential_mac& out, const credential_mac& key) {
    { policy.authority_mac(issuer, message, out) } -> std::same_as<bool>;
    { policy.keyed_mac(key, message, out) } -> std::same_as<bool>;
  };

// A MAC comparison must not stop at the first differing byte. A comparison
// whose duration depends on how many leading bytes matched lets a bearer
// discover a valid MAC one byte at a time, which turns an unforgeable tag into
// a few hundred guesses.
[[nodiscard]] inline bool equal_in_constant_time(const std::span<const std::byte> left,
                                                 const std::span<const std::byte> right) noexcept {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (size_t i = 0; i < left.size(); ++i)
    difference = static_cast<unsigned char>(
      difference | (std::to_integer<unsigned char>(left[i]) ^
                    std::to_integer<unsigned char>(right[i])));
  return difference == 0;
}

namespace detail {

inline void write_ticket_fields(state_writer& w, const reconnect_ticket& ticket) {
  w.u8(reconnect_credential_format);
  w.u32(ticket.issuer);
  w.u64(ticket.session);
  w.u64(ticket.principal);
  w.u64(ticket.authority_epoch);
  w.u64(ticket.issued_at);
  w.u64(ticket.expires_at);
}

[[nodiscard]] inline bool build_ticket_message(const reconnect_ticket& ticket,
                                               std::vector<std::byte>& scratch) {
  scratch.clear();
  state_writer w(scratch, false);
  w.u32(credential_domain_ticket);
  write_ticket_fields(w, ticket);
  return w.good();
}

[[nodiscard]] inline bool build_secret_message(const uint64_t session, const uint64_t principal,
                                               std::vector<std::byte>& scratch) {
  scratch.clear();
  state_writer w(scratch, false);
  w.u32(credential_domain_secret);
  w.u64(session);
  w.u64(principal);
  return w.good();
}

// The presentation covers the ticket as well as the transcript: otherwise a
// proof of possession made for one entitlement could be paired with another
// ticket from the same client.
[[nodiscard]] inline bool build_presentation_message(const reconnect_ticket& ticket,
                                                     const utils::digest& transcript,
                                                     std::vector<std::byte>& scratch) {
  scratch.clear();
  state_writer w(scratch, false);
  w.u32(credential_domain_presentation);
  write_ticket_fields(w, ticket);
  for (const auto byte : transcript) w.u8(byte);
  return w.good();
}

inline constexpr size_t credential_scratch_bytes =
  4 + reconnect_ticket_field_bytes + credential_mac_bytes;

} // namespace detail

// Scratch is caller-owned and retains its capacity: building a credential must
// not allocate on a reconnect storm.
struct credential_scratch {
  std::vector<std::byte> message;

  credential_scratch() {
    message.reserve(detail::credential_scratch_bytes);
  }
};

// Derived rather than stored, so the authority keeps no per-session secret
// table and cannot lose one. The client receives this value once, at admission,
// over the already-encrypted transport, and it is the only thing which proves
// the client is present rather than merely holding a captured ticket.
template <credential_mac_policy Policy>
[[nodiscard]] credential_status derive_session_secret(const uint32_t issuer,
                                                      const uint64_t session,
                                                      const uint64_t principal, Policy& policy,
                                                      credential_scratch& scratch,
                                                      credential_mac& output) {
  if (!detail::build_secret_message(session, principal, scratch.message))
    return credential_status::malformed;
  if (!policy.authority_mac(issuer, scratch.message, output))
    return credential_status::unknown_issuer;
  return credential_status::accepted;
}

// Authority side, at admission. Reissuing on every admission is what keeps a
// long session from depending on one long-lived ticket.
template <credential_mac_policy Policy>
[[nodiscard]] credential_status issue_reconnect_ticket(const reconnect_ticket& ticket,
                                                        Policy& policy,
                                                        credential_scratch& scratch,
                                                        reconnect_credential& output) {
  if (ticket.expires_at <= ticket.issued_at) return credential_status::malformed;
  if (!detail::build_ticket_message(ticket, scratch.message)) return credential_status::malformed;
  credential_mac mac{};
  if (!policy.authority_mac(ticket.issuer, scratch.message, mac))
    return credential_status::unknown_issuer;
  output.ticket = ticket;
  output.ticket_mac = mac;
  output.presentation_mac = credential_mac{};
  return credential_status::accepted;
}

// Client side, on reconnect. The transcript is the one from the NEW exchange:
// binding to the old one would be unverifiable, since that transcript is gone
// and its nonces are not the nonces now on the wire.
template <credential_mac_policy Policy>
[[nodiscard]] credential_status seal_reconnect_presentation(const credential_mac& session_secret,
                                                             const utils::digest& transcript,
                                                             Policy& policy,
                                                             credential_scratch& scratch,
                                                             reconnect_credential& credential) {
  if (!detail::build_presentation_message(credential.ticket, transcript, scratch.message))
    return credential_status::malformed;
  if (!policy.keyed_mac(session_secret, scratch.message, credential.presentation_mac))
    return credential_status::mac_unavailable;
  return credential_status::accepted;
}

// Authority side, on reconnect. The order is deliberate: declared fields first,
// then the ticket's own tag, then the proof of possession. An authority learns
// "this is not my ticket" without doing work proportional to a stranger's
// claims, and never derives a secret for a session it does not own.
template <credential_mac_policy Policy>
[[nodiscard]] credential_status verify_reconnect_credential(
  const reconnect_credential& credential, const reconnect_expectation& expectation,
  const uint64_t authority_now, const utils::digest& transcript, Policy& policy,
  credential_scratch& scratch) {
  const auto& ticket = credential.ticket;
  if (ticket.expires_at <= ticket.issued_at) return credential_status::malformed;
  if (ticket.session != expectation.session) return credential_status::wrong_session;
  if (ticket.principal != expectation.principal) return credential_status::wrong_principal;
  if (ticket.authority_epoch < expectation.authority_epoch) return credential_status::stale_epoch;
  if (ticket.authority_epoch > expectation.authority_epoch) return credential_status::future_epoch;
  // A clock which moved backwards must refuse rather than accept: `not_yet_valid`
  // is a distinct answer from `expired` because the operator's fix differs.
  if (authority_now < ticket.issued_at) return credential_status::not_yet_valid;
  if (authority_now >= ticket.expires_at) return credential_status::expired;

  if (!detail::build_ticket_message(ticket, scratch.message)) return credential_status::malformed;
  credential_mac expected{};
  if (!policy.authority_mac(ticket.issuer, scratch.message, expected))
    return credential_status::unknown_issuer;
  if (!equal_in_constant_time(expected, credential.ticket_mac))
    return credential_status::ticket_mac_invalid;

  credential_mac secret{};
  if (const auto status = derive_session_secret(ticket.issuer, ticket.session, ticket.principal,
                                                policy, scratch, secret);
      status != credential_status::accepted)
    return status;
  if (!detail::build_presentation_message(ticket, transcript, scratch.message))
    return credential_status::malformed;
  credential_mac presentation{};
  if (!policy.keyed_mac(secret, scratch.message, presentation))
    return credential_status::mac_unavailable;
  if (!equal_in_constant_time(presentation, credential.presentation_mac))
    return credential_status::presentation_mac_invalid;
  return credential_status::accepted;
}

// Canonical bytes for the opaque credential field of the handshake. The format
// byte leads, so a future credential shape is a refusal rather than a
// misreading of these fields.
[[nodiscard]] inline credential_status try_encode_credential(
  const reconnect_credential& credential, std::vector<std::byte>& out) {
  out.clear();
  if (out.capacity() < reconnect_credential_bytes) return credential_status::malformed;
  state_writer w(out, false);
  detail::write_ticket_fields(w, credential.ticket);
  w.bytes(credential.ticket_mac);
  w.bytes(credential.presentation_mac);
  return w.good() ? credential_status::accepted : credential_status::malformed;
}

[[nodiscard]] inline credential_status try_decode_credential(
  const std::span<const std::byte> bytes, reconnect_credential& out) noexcept {
  if (bytes.size() != reconnect_credential_bytes) return credential_status::malformed;
  state_reader r(bytes);
  if (r.u8() != reconnect_credential_format) return credential_status::malformed;
  reconnect_credential value;
  value.ticket.issuer = r.u32();
  value.ticket.session = r.u64();
  value.ticket.principal = r.u64();
  value.ticket.authority_epoch = r.u64();
  value.ticket.issued_at = r.u64();
  value.ticket.expires_at = r.u64();
  const auto ticket_mac = r.take(credential_mac_bytes);
  const auto presentation_mac = r.take(credential_mac_bytes);
  if (!r.good() || r.position() != r.size()) return credential_status::malformed;
  std::copy(ticket_mac.begin(), ticket_mac.end(), value.ticket_mac.begin());
  std::copy(presentation_mac.begin(), presentation_mac.end(), value.presentation_mac.begin());
  out = value;
  return credential_status::accepted;
}

} // namespace devils_engine::network

#endif
