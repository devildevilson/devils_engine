#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace net = devils_engine::network;
namespace utils = devils_engine::utils;

namespace {

// A concrete primitive lives in the test, not in the library: the library owns
// message construction, check order and comparison, and must not choose an
// algorithm. HMAC-SHA256 over the engine's own SHA-256.
net::credential_mac hmac_sha256(const std::span<const std::byte> key,
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

constexpr uint32_t held_issuer = 1;

struct mac_policy {
  std::array<std::byte, 32> authority_key{};
  unsigned authority_calls = 0;
  unsigned keyed_calls = 0;

  mac_policy() {
    for (size_t i = 0; i < authority_key.size(); ++i)
      authority_key[i] = std::byte(uint8_t(0xa0u + i));
  }

  bool authority_mac(const uint32_t issuer, const std::span<const std::byte> message,
                     net::credential_mac& out) {
    ++authority_calls;
    if (issuer != held_issuer) return false;
    out = hmac_sha256(authority_key, message);
    return true;
  }

  bool keyed_mac(const net::credential_mac& key, const std::span<const std::byte> message,
                 net::credential_mac& out) {
    ++keyed_calls;
    out = hmac_sha256(key, message);
    return true;
  }
};

static_assert(net::credential_mac_policy<mac_policy>);

utils::digest transcript_of(const uint8_t seed) {
  utils::digest value{};
  for (size_t i = 0; i < value.size(); ++i) value[i] = uint8_t(seed * 7u + i);
  return value;
}

net::reconnect_ticket sample_ticket() {
  net::reconnect_ticket ticket;
  ticket.issuer = held_issuer;
  ticket.session = 9001;
  ticket.principal = 4242;
  ticket.authority_epoch = 3;
  ticket.issued_at = 1'000'000;
  ticket.expires_at = 1'000'000 + 60'000'000; // one minute of grace
  return ticket;
}

net::reconnect_expectation expectation_of(const net::reconnect_ticket& ticket) {
  return net::reconnect_expectation{ticket.session, ticket.principal, ticket.authority_epoch};
}

// The whole admission-then-reconnect flow, as the authority and client would
// run it: mint a ticket, hand the client its derived secret, then present the
// credential in a new exchange with a new transcript.
struct flow {
  mac_policy policy;
  net::credential_scratch scratch;
  net::reconnect_ticket ticket = sample_ticket();
  net::reconnect_credential credential;
  net::credential_mac secret{};

  flow() {
    REQUIRE(net::issue_reconnect_ticket(ticket, policy, scratch, credential) ==
            net::credential_status::accepted);
    REQUIRE(net::derive_session_secret(ticket.issuer, ticket.session, ticket.principal, policy,
                                       scratch, secret) == net::credential_status::accepted);
  }

  net::credential_status present(const utils::digest& transcript) {
    return net::seal_reconnect_presentation(secret, transcript, policy, scratch, credential);
  }

  net::credential_status verify(const utils::digest& transcript, const uint64_t now) {
    return net::verify_reconnect_credential(credential, expectation_of(ticket), now, transcript,
                                            policy, scratch);
  }
};

} // namespace

TEST_CASE("network reconnect credential round-trips through the handshake field") {
  flow session;
  const auto transcript = transcript_of(1);
  REQUIRE(session.present(transcript) == net::credential_status::accepted);

  std::vector<std::byte> bytes;
  bytes.reserve(net::reconnect_credential_bytes);
  REQUIRE(net::try_encode_credential(session.credential, bytes) ==
          net::credential_status::accepted);
  CHECK(bytes.size() == net::reconnect_credential_bytes);
  CHECK(bytes.size() <= net::session_wire_max_credential_bytes);

  net::reconnect_credential decoded;
  REQUIRE(net::try_decode_credential(bytes, decoded) == net::credential_status::accepted);
  CHECK(decoded == session.credential);

  SUBCASE("a wrong size or format byte is refused, never partially read") {
    auto shorter = bytes;
    shorter.pop_back();
    CHECK(net::try_decode_credential(shorter, decoded) == net::credential_status::malformed);
    auto longer = bytes;
    longer.push_back(std::byte(0));
    CHECK(net::try_decode_credential(longer, decoded) == net::credential_status::malformed);
    auto other_format = bytes;
    other_format[0] = std::byte(2);
    CHECK(net::try_decode_credential(other_format, decoded) == net::credential_status::malformed);
  }

  SUBCASE("an unprepared buffer is refused instead of growing") {
    std::vector<std::byte> unprepared;
    CHECK(net::try_encode_credential(session.credential, unprepared) ==
          net::credential_status::malformed);
    CHECK(unprepared.capacity() == 0);
  }
}

TEST_CASE("network reconnect credential is accepted only by its own authority state") {
  flow session;
  const auto transcript = transcript_of(2);
  REQUIRE(session.present(transcript) == net::credential_status::accepted);
  const uint64_t now = session.ticket.issued_at + 1'000'000;
  CHECK(session.verify(transcript, now) == net::credential_status::accepted);

  SUBCASE("a session or principal the authority did not expect is refused") {
    auto other = expectation_of(session.ticket);
    other.session += 1;
    CHECK(net::verify_reconnect_credential(session.credential, other, now, transcript,
                                           session.policy, session.scratch) ==
          net::credential_status::wrong_session);
    other = expectation_of(session.ticket);
    other.principal += 1;
    CHECK(net::verify_reconnect_credential(session.credential, other, now, transcript,
                                           session.policy, session.scratch) ==
          net::credential_status::wrong_principal);
  }

  SUBCASE("an authority which migrated during the gap refuses the old epoch") {
    auto migrated = expectation_of(session.ticket);
    migrated.authority_epoch += 1;
    CHECK(net::verify_reconnect_credential(session.credential, migrated, now, transcript,
                                           session.policy, session.scratch) ==
          net::credential_status::stale_epoch);
    migrated.authority_epoch = session.ticket.authority_epoch - 1;
    CHECK(net::verify_reconnect_credential(session.credential, migrated, now, transcript,
                                           session.policy, session.scratch) ==
          net::credential_status::future_epoch);
  }

  SUBCASE("expiry and a clock which moved backwards are distinct answers") {
    CHECK(session.verify(transcript, session.ticket.expires_at) ==
          net::credential_status::expired);
    CHECK(session.verify(transcript, session.ticket.expires_at + 1) ==
          net::credential_status::expired);
    CHECK(session.verify(transcript, session.ticket.issued_at - 1) ==
          net::credential_status::not_yet_valid);
    // The boundaries themselves: valid from the issue instant, invalid at expiry.
    CHECK(session.verify(transcript, session.ticket.issued_at) ==
          net::credential_status::accepted);
  }

  SUBCASE("an issuer the authority does not hold is refused") {
    session.credential.ticket.issuer = held_issuer + 1;
    session.ticket.issuer = session.credential.ticket.issuer;
    CHECK(session.verify(transcript, now) == net::credential_status::unknown_issuer);
  }

  SUBCASE("a window which does not open is malformed, not merely expired") {
    session.credential.ticket.expires_at = session.credential.ticket.issued_at;
    session.ticket = session.credential.ticket;
    CHECK(session.verify(transcript, now) == net::credential_status::malformed);
    net::reconnect_credential unused;
    CHECK(net::issue_reconnect_ticket(session.ticket, session.policy, session.scratch, unused) ==
          net::credential_status::malformed);
  }

  SUBCASE("every tampered ticket field invalidates the authority's own tag") {
    for (int field = 0; field < 4; ++field) {
      CAPTURE(field);
      flow tampered;
      REQUIRE(tampered.present(transcript) == net::credential_status::accepted);
      switch (field) {
        case 0: tampered.credential.ticket.session += 1; break;
        case 1: tampered.credential.ticket.principal += 1; break;
        case 2: tampered.credential.ticket.authority_epoch += 1; break;
        default: tampered.credential.ticket.expires_at += 1'000'000; break;
      }
      // The authority's own state is deliberately made to AGREE with the forged
      // fields, which is the worst case for it: the declared comparisons all
      // pass and only the tag stands between the bearer and admission. That is
      // the point — the field comparison is a diagnostic, the tag is the defence.
      const auto status = net::verify_reconnect_credential(
        tampered.credential, expectation_of(tampered.credential.ticket), now, transcript,
        tampered.policy, tampered.scratch);
      CHECK(status == net::credential_status::ticket_mac_invalid);
    }
  }
}

TEST_CASE("network reconnect credential cannot be replayed by whoever captured it") {
  flow session;
  const auto captured_exchange = transcript_of(3);
  REQUIRE(session.present(captured_exchange) == net::credential_status::accepted);
  const uint64_t now = session.ticket.issued_at + 1'000'000;
  REQUIRE(session.verify(captured_exchange, now) == net::credential_status::accepted);

  SUBCASE("the whole credential presented in another exchange is refused") {
    // A passive observer holds every byte, including both tags. The new
    // exchange has different nonces, so its transcript differs and the proof
    // of possession no longer matches.
    const auto other_exchange = transcript_of(4);
    CHECK(session.verify(other_exchange, now) ==
          net::credential_status::presentation_mac_invalid);
  }

  SUBCASE("holding the ticket without the derived secret is not enough") {
    flow bearer;
    REQUIRE(bearer.present(captured_exchange) == net::credential_status::accepted);
    // The bearer copies the entitlement but keys its proof with a secret it
    // guessed rather than the one the authority derived.
    net::credential_mac wrong_secret{};
    for (size_t i = 0; i < wrong_secret.size(); ++i) wrong_secret[i] = std::byte(uint8_t(i));
    REQUIRE(net::seal_reconnect_presentation(wrong_secret, captured_exchange, bearer.policy,
                                             bearer.scratch, bearer.credential) ==
            net::credential_status::accepted);
    CHECK(net::verify_reconnect_credential(bearer.credential, expectation_of(bearer.ticket), now,
                                           captured_exchange, bearer.policy, bearer.scratch) ==
          net::credential_status::presentation_mac_invalid);
  }

  SUBCASE("a proof made for one entitlement does not carry another ticket") {
    flow second;
    second.ticket.expires_at += 30'000'000;
    net::reconnect_credential reissued;
    REQUIRE(net::issue_reconnect_ticket(second.ticket, second.policy, second.scratch, reissued) ==
            net::credential_status::accepted);
    // Pair the newer entitlement with the older presentation.
    reissued.presentation_mac = session.credential.presentation_mac;
    CHECK(net::verify_reconnect_credential(reissued, expectation_of(second.ticket), now,
                                           captured_exchange, second.policy, second.scratch) ==
          net::credential_status::presentation_mac_invalid);
  }
}

TEST_CASE("network credential checks are ordered so a stranger costs no work") {
  flow session;
  const auto transcript = transcript_of(5);
  REQUIRE(session.present(transcript) == net::credential_status::accepted);
  const uint64_t now = session.ticket.issued_at + 1'000'000;

  auto other = expectation_of(session.ticket);
  other.session += 1;
  session.policy.authority_calls = 0;
  session.policy.keyed_calls = 0;
  CHECK(net::verify_reconnect_credential(session.credential, other, now, transcript,
                                         session.policy, session.scratch) ==
        net::credential_status::wrong_session);
  // No tag was computed and no secret was derived for a session this authority
  // does not own.
  CHECK(session.policy.authority_calls == 0);
  CHECK(session.policy.keyed_calls == 0);

  SUBCASE("an expired ticket also costs nothing") {
    session.policy.authority_calls = 0;
    CHECK(session.verify(transcript, session.ticket.expires_at) ==
          net::credential_status::expired);
    CHECK(session.policy.authority_calls == 0);
  }

  SUBCASE("a bad ticket tag stops before the secret is derived") {
    session.credential.ticket_mac[0] =
      std::byte(std::to_integer<unsigned char>(session.credential.ticket_mac[0]) ^ 1u);
    session.policy.keyed_calls = 0;
    CHECK(session.verify(transcript, now) == net::credential_status::ticket_mac_invalid);
    CHECK(session.policy.keyed_calls == 0);
  }
}

TEST_CASE("network credential secrets are derived, separated by domain and per session") {
  mac_policy policy;
  net::credential_scratch scratch;
  net::credential_mac first{}, again{}, other_session{}, other_principal{};

  REQUIRE(net::derive_session_secret(held_issuer, 9001, 4242, policy, scratch, first) ==
          net::credential_status::accepted);
  REQUIRE(net::derive_session_secret(held_issuer, 9001, 4242, policy, scratch, again) ==
          net::credential_status::accepted);
  REQUIRE(net::derive_session_secret(held_issuer, 9002, 4242, policy, scratch, other_session) ==
          net::credential_status::accepted);
  REQUIRE(net::derive_session_secret(held_issuer, 9001, 4243, policy, scratch, other_principal) ==
          net::credential_status::accepted);

  CHECK(first == again);
  CHECK(first != other_session);
  CHECK(first != other_principal);

  SUBCASE("an issuer the authority does not hold derives nothing") {
    net::credential_mac unused{};
    CHECK(net::derive_session_secret(held_issuer + 1, 9001, 4242, policy, scratch, unused) ==
          net::credential_status::unknown_issuer);
  }

  SUBCASE("a secret and a ticket tag under the same key are different strings") {
    // Domain tags are why. Without them both are bytes under one key and an
    // attacker chooses which one they are.
    flow session;
    net::credential_mac secret{};
    REQUIRE(net::derive_session_secret(session.ticket.issuer, session.ticket.session,
                                       session.ticket.principal, session.policy, session.scratch,
                                       secret) == net::credential_status::accepted);
    CHECK(secret != session.credential.ticket_mac);
  }
}

TEST_CASE("network credential comparison does not stop at the first difference") {
  const std::array<std::byte, 4> base{std::byte(1), std::byte(2), std::byte(3), std::byte(4)};
  CHECK(net::equal_in_constant_time(base, base));
  for (size_t i = 0; i < base.size(); ++i) {
    CAPTURE(i);
    auto altered = base;
    altered[i] = std::byte(0xff);
    CHECK_FALSE(net::equal_in_constant_time(base, altered));
  }
  const std::array<std::byte, 3> shorter{std::byte(1), std::byte(2), std::byte(3)};
  CHECK_FALSE(net::equal_in_constant_time(base, shorter));
  CHECK(net::equal_in_constant_time(std::span<const std::byte>{}, std::span<const std::byte>{}));
}

TEST_CASE("network credential reissue does not revoke the previous ticket") {
  // Written down because it is a limitation, not an oversight. Without keeping
  // per-session state an authority cannot revoke; expiry is the only
  // revocation it has, which is exactly why the window is short and a ticket is
  // reissued at every admission.
  flow session;
  const auto transcript = transcript_of(6);
  REQUIRE(session.present(transcript) == net::credential_status::accepted);
  const auto older = session.credential;
  const uint64_t now = session.ticket.issued_at + 1'000'000;

  auto renewed = session.ticket;
  renewed.issued_at = now;
  renewed.expires_at = now + 60'000'000;
  net::reconnect_credential reissued;
  REQUIRE(net::issue_reconnect_ticket(renewed, session.policy, session.scratch, reissued) ==
          net::credential_status::accepted);
  REQUIRE(net::seal_reconnect_presentation(session.secret, transcript, session.policy,
                                           session.scratch, reissued) ==
          net::credential_status::accepted);
  CHECK(net::verify_reconnect_credential(reissued, expectation_of(renewed), now, transcript,
                                         session.policy, session.scratch) ==
        net::credential_status::accepted);

  // The older ticket still verifies until its own expiry.
  CHECK(net::verify_reconnect_credential(older, expectation_of(session.ticket), now, transcript,
                                         session.policy, session.scratch) ==
        net::credential_status::accepted);
  CHECK(net::verify_reconnect_credential(older, expectation_of(session.ticket),
                                         session.ticket.expires_at, transcript, session.policy,
                                         session.scratch) == net::credential_status::expired);
}
