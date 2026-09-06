#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace net = devils_engine::network;
namespace utils = devils_engine::utils;

namespace {

net::session_nonce nonce_of(const std::uint8_t seed) {
  net::session_nonce value{};
  for (std::size_t i = 0; i < value.size(); ++i)
    value[i] = std::byte(std::uint8_t(seed * 31u + i));
  return value;
}

utils::digest digest_of(const std::uint8_t seed) {
  utils::digest value{};
  for (std::size_t i = 0; i < value.size(); ++i) value[i] = std::uint8_t(seed + i);
  return value;
}

net::session_compatibility compatibility() {
  return net::session_compatibility{1, 7, 0xaaaa, 0xbbbb, 0xcccc, digest_of(3)};
}

std::vector<std::byte> prepared() {
  std::vector<std::byte> buffer;
  buffer.reserve(net::session_wire_max_message_bytes);
  return buffer;
}

std::vector<std::byte> blob(const std::size_t size, const std::uint8_t seed) {
  std::vector<std::byte> value(size);
  for (std::size_t i = 0; i < size; ++i) value[i] = std::byte(std::uint8_t(seed + i * 7u));
  return value;
}

// The authority policy answers with a fixed challenge and admits a credential
// only when it equals the transcript the library derived. This is deliberately
// the whole point of the transcript: a credential recorded from any other
// exchange cannot satisfy it.
struct authority_policy {
  std::vector<std::byte> challenge = blob(48, 5);
  net::session_accepted grant{9001, 42, 1, 3, 120};
  net::session_refusal_reason admit_reason = net::session_refusal_reason::none;
  bool allow_challenge = true;
  unsigned challenges = 0;
  unsigned admits = 0;
  utils::digest seen_transcript{};
  std::optional<std::uint64_t> seen_resumed_session;
  std::optional<net::recovery_anchor<std::uint64_t, utils::digest>> seen_anchor;

  bool issue_challenge(const net::client_hello&, std::vector<std::byte>& out) {
    ++challenges;
    if (!allow_challenge) return false;
    out.assign(challenge.begin(), challenge.end());
    return true;
  }

  net::session_refusal_reason admit(const net::client_response& response,
                                    const utils::digest& transcript,
                                    net::session_accepted& accepted) {
    ++admits;
    seen_transcript = transcript;
    seen_resumed_session = response.resumed_session;
    seen_anchor = response.confirmed;
    if (admit_reason != net::session_refusal_reason::none) return admit_reason;
    if (response.credential.size() != transcript.size())
      return net::session_refusal_reason::identity_rejected;
    for (std::size_t i = 0; i < transcript.size(); ++i) {
      if (response.credential[i] != std::byte(transcript[i]))
        return net::session_refusal_reason::identity_rejected;
    }
    accepted = grant;
    return net::session_refusal_reason::none;
  }
};

struct client_policy {
  bool answer_ok = true;
  unsigned answers = 0;
  utils::digest seen_transcript{};
  std::vector<std::byte> forced_credential;

  bool answer(const net::authority_challenge&, const utils::digest& transcript,
              std::vector<std::byte>& credential) {
    ++answers;
    seen_transcript = transcript;
    if (!answer_ok) return false;
    if (!forced_credential.empty()) {
      credential.assign(forced_credential.begin(), forced_credential.end());
      return true;
    }
    credential.reserve(transcript.size());
    for (const auto byte : transcript) credential.push_back(std::byte(byte));
    return true;
  }
};

} // namespace

TEST_CASE("network session wire round-trips every handshake message") {
  auto buffer = prepared();

  SUBCASE("client hello") {
    const net::client_hello sent{compatibility(), nonce_of(2)};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    REQUIRE(envelope.type == net::session_message_type::client_hello);
    net::client_hello decoded;
    REQUIRE(net::try_decode(envelope.payload, decoded) == net::session_wire_status::ok);
    CHECK(decoded.compatibility == sent.compatibility);
    CHECK(decoded.client_nonce == sent.client_nonce);
  }

  SUBCASE("authority challenge with the largest legal challenge") {
    const auto challenge = blob(net::session_wire_max_challenge_bytes, 11);
    const net::authority_challenge sent{compatibility(), nonce_of(4), challenge};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    REQUIRE(envelope.type == net::session_message_type::authority_challenge);
    net::authority_challenge decoded;
    REQUIRE(net::try_decode(envelope.payload, decoded) == net::session_wire_status::ok);
    CHECK(decoded.compatibility == sent.compatibility);
    CHECK(decoded.authority_nonce == sent.authority_nonce);
    REQUIRE(decoded.challenge.size() == challenge.size());
    CHECK(std::equal(decoded.challenge.begin(), decoded.challenge.end(), challenge.begin()));
  }

  SUBCASE("client response without a reconnect claim") {
    const auto credential = blob(64, 21);
    const net::client_response sent{credential, std::nullopt, std::nullopt};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    net::client_response decoded;
    REQUIRE(net::try_decode(envelope.payload, decoded) == net::session_wire_status::ok);
    REQUIRE(decoded.credential.size() == credential.size());
    CHECK(std::equal(decoded.credential.begin(), decoded.credential.end(), credential.begin()));
    CHECK_FALSE(decoded.resumed_session.has_value());
    CHECK_FALSE(decoded.confirmed.has_value());
  }

  SUBCASE("client response carrying a resumed session and proven anchor") {
    const auto credential = blob(net::session_wire_max_credential_bytes, 33);
    const net::recovery_anchor<std::uint64_t, utils::digest> anchor{77, digest_of(9)};
    const net::client_response sent{credential, std::optional<std::uint64_t>(555), anchor};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    net::client_response decoded;
    REQUIRE(net::try_decode(envelope.payload, decoded) == net::session_wire_status::ok);
    CHECK(decoded.resumed_session == std::optional<std::uint64_t>(555));
    REQUIRE(decoded.confirmed.has_value());
    CHECK(*decoded.confirmed == anchor);
  }

  SUBCASE("session accepted") {
    const net::session_accepted sent{1, 2, 3, 4, 5};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    net::session_accepted decoded;
    REQUIRE(net::try_decode(envelope.payload, decoded) == net::session_wire_status::ok);
    CHECK(decoded.session == 1);
    CHECK(decoded.local_peer == 2);
    CHECK(decoded.authority_peer == 3);
    CHECK(decoded.authority_epoch == 4);
    CHECK(decoded.start_tick == 5);
  }

  SUBCASE("session refused") {
    const net::session_refused sent{net::session_refusal_reason::content_mismatch};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    net::session_refused decoded;
    REQUIRE(net::try_decode(envelope.payload, decoded) == net::session_wire_status::ok);
    CHECK(decoded.reason == net::session_refusal_reason::content_mismatch);
  }
}

TEST_CASE("network session wire refuses every malformed envelope") {
  auto buffer = prepared();
  const net::client_hello hello{compatibility(), nonce_of(2)};
  REQUIRE(net::try_encode(hello, buffer) == net::session_wire_status::ok);
  const std::vector<std::byte> good = buffer;
  net::session_wire_message envelope;

  SUBCASE("every truncated prefix") {
    for (std::size_t size = 0; size < good.size(); ++size) {
      const auto status = net::try_peek_session_message(std::span(good).first(size), envelope);
      CHECK(status != net::session_wire_status::ok);
    }
  }

  SUBCASE("trailing bytes are never ignored") {
    auto extended = good;
    extended.push_back(std::byte(0));
    CHECK(net::try_peek_session_message(extended, envelope) ==
          net::session_wire_status::trailing_bytes);
  }

  SUBCASE("bad magic") {
    auto broken = good;
    broken[0] = std::byte(0);
    CHECK(net::try_peek_session_message(broken, envelope) == net::session_wire_status::bad_magic);
  }

  SUBCASE("unsupported envelope version") {
    auto broken = good;
    broken[4] = std::byte(2);
    CHECK(net::try_peek_session_message(broken, envelope) ==
          net::session_wire_status::unsupported_envelope);
  }

  SUBCASE("unknown and zero message type") {
    for (const std::uint8_t type : {std::uint8_t(0), std::uint8_t(6), std::uint8_t(255)}) {
      auto broken = good;
      broken[6] = std::byte(type);
      CHECK(net::try_peek_session_message(broken, envelope) ==
            net::session_wire_status::unknown_message_type);
    }
  }

  SUBCASE("reserved byte must be zero") {
    auto broken = good;
    broken[7] = std::byte(1);
    CHECK(net::try_peek_session_message(broken, envelope) ==
          net::session_wire_status::invalid_field);
  }

  SUBCASE("declared length must match the buffer exactly") {
    auto shorter = good;
    shorter[8] = std::byte(std::uint8_t(good.size() - net::session_wire_header_bytes - 1));
    CHECK(net::try_peek_session_message(shorter, envelope) ==
          net::session_wire_status::trailing_bytes);
    auto longer = good;
    longer[8] = std::byte(std::uint8_t(good.size() - net::session_wire_header_bytes + 1));
    CHECK(net::try_peek_session_message(longer, envelope) == net::session_wire_status::truncated);
    auto absurd = good;
    absurd[9] = std::byte(0xff);
    CHECK(net::try_peek_session_message(absurd, envelope) == net::session_wire_status::too_large);
  }

  SUBCASE("a message larger than the declared budget is refused before parsing") {
    std::vector<std::byte> oversized(net::session_wire_max_message_bytes + 1);
    CHECK(net::try_peek_session_message(oversized, envelope) ==
          net::session_wire_status::too_large);
  }
}

TEST_CASE("network session wire refuses noncanonical and oversized payload fields") {
  auto buffer = prepared();

  SUBCASE("an absent optional must be encoded as zero") {
    const auto credential = blob(4, 1);
    const net::client_response sent{credential, std::nullopt, std::nullopt};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    const std::size_t session_at = net::session_wire_header_bytes + 4 + credential.size() + 1;
    net::client_response decoded;

    auto forged = buffer;
    forged[session_at] = std::byte(1); // Session bytes present while the flag says absent.
    net::session_wire_message forged_envelope;
    REQUIRE(net::try_peek_session_message(forged, forged_envelope) ==
            net::session_wire_status::ok);
    CHECK(net::try_decode(forged_envelope.payload, decoded) ==
          net::session_wire_status::invalid_field);

    auto flagged = buffer;
    flagged[session_at - 1] = std::byte(2); // Presence flags accept only zero or one.
    REQUIRE(net::try_peek_session_message(flagged, forged_envelope) ==
            net::session_wire_status::ok);
    CHECK(net::try_decode(forged_envelope.payload, decoded) ==
          net::session_wire_status::invalid_field);
  }

  SUBCASE("an anchor without a session is not a legal claim") {
    const auto credential = blob(4, 1);
    const net::recovery_anchor<std::uint64_t, utils::digest> anchor{5, digest_of(1)};
    const net::client_response sent{credential, std::nullopt, anchor};
    CHECK(net::try_encode(sent, buffer) == net::session_wire_status::invalid_field);
  }

  SUBCASE("refusal reason must be a named nonzero value") {
    CHECK(net::try_encode(net::session_refused{net::session_refusal_reason::none}, buffer) ==
          net::session_wire_status::invalid_field);
    const net::session_refused sent{net::session_refusal_reason::no_capacity};
    REQUIRE(net::try_encode(sent, buffer) == net::session_wire_status::ok);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(buffer, envelope) == net::session_wire_status::ok);
    net::session_refused decoded;
    for (const std::uint8_t reason : {std::uint8_t(0), std::uint8_t(12), std::uint8_t(255)}) {
      auto forged = buffer;
      forged[net::session_wire_header_bytes] = std::byte(reason);
      net::session_wire_message forged_envelope;
      REQUIRE(net::try_peek_session_message(forged, forged_envelope) ==
              net::session_wire_status::ok);
      CHECK(net::try_decode(forged_envelope.payload, decoded) ==
            net::session_wire_status::invalid_field);
    }
  }

  SUBCASE("challenge and credential budgets are refused at encode") {
    const auto challenge = blob(net::session_wire_max_challenge_bytes + 1, 3);
    CHECK(net::try_encode(net::authority_challenge{compatibility(), nonce_of(1), challenge},
                          buffer) == net::session_wire_status::too_large);
    const auto credential = blob(net::session_wire_max_credential_bytes + 1, 4);
    CHECK(net::try_encode(net::client_response{credential, std::nullopt, std::nullopt}, buffer) ==
          net::session_wire_status::too_large);
  }

  SUBCASE("an unprepared buffer is refused instead of growing") {
    std::vector<std::byte> unprepared;
    CHECK(net::try_encode(net::client_hello{compatibility(), nonce_of(2)}, unprepared) ==
          net::session_wire_status::buffer_too_small);
    CHECK(unprepared.empty());
    CHECK(unprepared.capacity() == 0);
  }
}

TEST_CASE("network session handshake completes as an ordered multi-message exchange") {
  net::authority_handshake authority(compatibility(), nonce_of(1));
  net::client_handshake client(compatibility(), nonce_of(2));
  authority_policy authority_side;
  client_policy client_side;
  auto to_authority = prepared(), to_client = prepared();

  REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
  CHECK(client.phase() == net::handshake_phase::awaiting_challenge);

  REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
          net::session_wire_status::ok);
  CHECK(authority.phase() == net::handshake_phase::awaiting_response);
  CHECK(authority_side.challenges == 1);

  REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
  CHECK(client.phase() == net::handshake_phase::awaiting_result);
  CHECK(client_side.answers == 1);

  REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
          net::session_wire_status::ok);
  REQUIRE(authority.established());
  CHECK(authority_side.admits == 1);

  REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
  REQUIRE(client.established());

  // Both roles derived the same transcript from the same two messages, and the
  // credential was accepted only because of that agreement.
  CHECK(client_side.seen_transcript == authority_side.seen_transcript);
  CHECK(client.accepted().session == authority.accepted().session);
  CHECK(client.accepted().local_peer == 42);
  CHECK(client.accepted().authority_epoch == 3);
  CHECK(client.accepted().start_tick == 120);
  CHECK(client.refusal() == net::session_refusal_reason::none);
}

TEST_CASE("network session handshake refuses each incompatibility before any credential") {
  const auto local = compatibility();
  struct field {
    const char* name;
    net::session_compatibility remote;
    net::session_refusal_reason reason;
  };
  auto altered = [local](void (*apply)(net::session_compatibility&)) {
    auto copy = local;
    apply(copy);
    return copy;
  };
  const std::array<field, 6> cases{
    field{"handshake format",
          altered([](net::session_compatibility& c) { c.handshake_format = 2; }),
          net::session_refusal_reason::handshake_format_mismatch},
    field{"protocol version",
          altered([](net::session_compatibility& c) { c.protocol_version = 8; }),
          net::session_refusal_reason::protocol_version_mismatch},
    field{"content root", altered([](net::session_compatibility& c) { c.content_root[0] ^= 1; }),
          net::session_refusal_reason::content_mismatch},
    field{"state schema",
          altered([](net::session_compatibility& c) { c.state_schema_fingerprint = 1; }),
          net::session_refusal_reason::state_schema_mismatch},
    field{"intent schema",
          altered([](net::session_compatibility& c) { c.intent_schema_fingerprint = 1; }),
          net::session_refusal_reason::intent_schema_mismatch},
    field{"numeric profile",
          altered([](net::session_compatibility& c) { c.numeric_profile = 1; }),
          net::session_refusal_reason::numeric_profile_mismatch}};

  for (const auto& item : cases) {
    CAPTURE(item.name);
    net::authority_handshake authority(local, nonce_of(1));
    net::client_handshake client(item.remote, nonce_of(2));
    authority_policy authority_side;
    client_policy client_side;
    auto to_authority = prepared(), to_client = prepared();

    REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    CHECK(authority.phase() == net::handshake_phase::refused);
    CHECK(authority.refusal() == item.reason);
    // No challenge was issued and no credential was examined.
    CHECK(authority_side.challenges == 0);
    CHECK(authority_side.admits == 0);

    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    CHECK(client.phase() == net::handshake_phase::refused);
    CHECK(client.refusal() == item.reason);
    CHECK(client_side.answers == 0);
  }
}

TEST_CASE("network session handshake refuses identity and stale credentials") {
  auto to_authority = prepared(), to_client = prepared();

  SUBCASE("a rejected credential names identity and grants no session") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    net::client_handshake client(compatibility(), nonce_of(2));
    authority_policy authority_side;
    authority_side.admit_reason = net::session_refusal_reason::identity_rejected;
    client_policy client_side;

    REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    CHECK(authority.phase() == net::handshake_phase::refused);
    CHECK(authority.refusal() == net::session_refusal_reason::identity_rejected);
    CHECK(authority.accepted().session == 0);

    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    CHECK(client.phase() == net::handshake_phase::refused);
    CHECK(client.refusal() == net::session_refusal_reason::identity_rejected);
    CHECK(client.accepted().session == 0);
  }

  SUBCASE("a credential recorded from another exchange does not verify") {
    // Record a complete first exchange, then replay its credential against an
    // authority whose nonce differs. The transcript differs, so admit refuses.
    net::authority_handshake first(compatibility(), nonce_of(1));
    net::client_handshake first_client(compatibility(), nonce_of(2));
    authority_policy first_policy;
    client_policy first_client_policy;
    REQUIRE(first_client.start(to_authority) == net::session_wire_status::ok);
    REQUIRE(first.consume(to_authority, to_client, first_policy) == net::session_wire_status::ok);
    REQUIRE(first_client.consume(to_client, to_authority, first_client_policy) ==
            net::session_wire_status::ok);
    const utils::digest recorded = first_client_policy.seen_transcript;

    net::authority_handshake second(compatibility(), nonce_of(77));
    net::client_handshake second_client(compatibility(), nonce_of(2));
    authority_policy second_policy;
    client_policy replaying;
    replaying.forced_credential.reserve(recorded.size());
    for (const auto byte : recorded) replaying.forced_credential.push_back(std::byte(byte));

    auto second_to_authority = prepared(), second_to_client = prepared();
    REQUIRE(second_client.start(second_to_authority) == net::session_wire_status::ok);
    REQUIRE(second.consume(second_to_authority, second_to_client, second_policy) ==
            net::session_wire_status::ok);
    REQUIRE(second_client.consume(second_to_client, second_to_authority, replaying) ==
            net::session_wire_status::ok);
    REQUIRE(second.consume(second_to_authority, second_to_client, second_policy) ==
            net::session_wire_status::ok);
    CHECK(second.phase() == net::handshake_phase::refused);
    CHECK(second.refusal() == net::session_refusal_reason::identity_rejected);
    CHECK(second_policy.seen_transcript != recorded);
  }

  SUBCASE("a policy which cannot issue a challenge refuses without a session") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    net::client_handshake client(compatibility(), nonce_of(2));
    authority_policy authority_side;
    authority_side.allow_challenge = false;
    client_policy client_side;

    REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    CHECK(authority.phase() == net::handshake_phase::refused);
    CHECK(authority_side.admits == 0);
    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    CHECK(client.refusal() == net::session_refusal_reason::identity_rejected);
  }
}

TEST_CASE("network session handshake refuses out-of-order and repeated messages") {
  auto to_authority = prepared(), to_client = prepared();

  SUBCASE("a response before a hello is unexpected") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    authority_policy authority_side;
    const auto credential = blob(8, 1);
    REQUIRE(net::try_encode(net::client_response{credential, std::nullopt, std::nullopt},
                            to_authority) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    CHECK(authority.phase() == net::handshake_phase::refused);
    CHECK(authority.refusal() == net::session_refusal_reason::unexpected_message);
    CHECK(authority_side.challenges == 0);
    net::session_wire_message envelope;
    REQUIRE(net::try_peek_session_message(to_client, envelope) == net::session_wire_status::ok);
    CHECK(envelope.type == net::session_message_type::session_refused);
  }

  SUBCASE("a repeated hello after the challenge is unexpected") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    net::client_handshake client(compatibility(), nonce_of(2));
    authority_policy authority_side;
    client_policy client_side;
    REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
    const std::vector<std::byte> hello = to_authority;
    REQUIRE(authority.consume(hello, to_client, authority_side) == net::session_wire_status::ok);
    REQUIRE(authority.consume(hello, to_client, authority_side) == net::session_wire_status::ok);
    CHECK(authority.phase() == net::handshake_phase::refused);
    CHECK(authority.refusal() == net::session_refusal_reason::unexpected_message);
    CHECK(authority_side.challenges == 1);
  }

  SUBCASE("a refusal is terminal for both roles") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    net::client_handshake client(compatibility(), nonce_of(2));
    authority_policy authority_side;
    client_policy client_side;
    REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
    const std::vector<std::byte> hello = to_authority;
    REQUIRE(authority.consume(hello, to_client, authority_side) == net::session_wire_status::ok);

    // The client is refused by an unexpected accepted message, then must not
    // resume the exchange even when the correct challenge arrives afterwards.
    auto premature = prepared();
    REQUIRE(net::try_encode(net::session_accepted{1, 2, 3, 4, 5}, premature) ==
            net::session_wire_status::ok);
    REQUIRE(client.consume(premature, to_authority, client_side) == net::session_wire_status::ok);
    CHECK(client.refusal() == net::session_refusal_reason::unexpected_message);
    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    CHECK(client.phase() == net::handshake_phase::refused);
    CHECK(client_side.answers == 0);
    CHECK(client.start(to_authority) == net::session_wire_status::invalid_field);
  }

  SUBCASE("a malformed message refuses the exchange rather than being skipped") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    authority_policy authority_side;
    std::vector<std::byte> garbage(net::session_wire_header_bytes, std::byte(0x5a));
    CHECK(authority.consume(garbage, to_client, authority_side) !=
          net::session_wire_status::ok);
    CHECK(authority.phase() == net::handshake_phase::refused);
    CHECK(authority.refusal() == net::session_refusal_reason::malformed_message);
  }
}

TEST_CASE("network session handshake carries a reconnect claim to the authority") {
  const net::recovery_anchor<std::uint64_t, utils::digest> anchor{64, digest_of(7)};
  auto to_authority = prepared(), to_client = prepared();

  SUBCASE("the authority observes the claim and may grant the same session") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    net::client_handshake client(compatibility(), nonce_of(2),
                                 std::optional<std::uint64_t>(9001), anchor);
    authority_policy authority_side;
    client_policy client_side;

    REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    CHECK(authority_side.seen_resumed_session == std::optional<std::uint64_t>(9001));
    REQUIRE(authority_side.seen_anchor.has_value());
    CHECK(*authority_side.seen_anchor == anchor);
    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    CHECK(client.established());
    CHECK(client.accepted().session == 9001);
  }

  SUBCASE("a different granted session refuses instead of silently rejoining") {
    net::authority_handshake authority(compatibility(), nonce_of(1));
    net::client_handshake client(compatibility(), nonce_of(2),
                                 std::optional<std::uint64_t>(4242), anchor);
    authority_policy authority_side;
    client_policy client_side;

    REQUIRE(client.start(to_authority) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    REQUIRE(authority.consume(to_authority, to_client, authority_side) ==
            net::session_wire_status::ok);
    REQUIRE(authority.established());
    REQUIRE(client.consume(to_client, to_authority, client_side) == net::session_wire_status::ok);
    CHECK(client.phase() == net::handshake_phase::refused);
    CHECK(client.refusal() == net::session_refusal_reason::unknown_session);
  }
}

TEST_CASE("network session transcript is order sensitive and sealed once") {
  const auto first = blob(16, 1), second = blob(16, 2);
  net::session_transcript forward, backward, repeated;
  forward.absorb(first);
  forward.absorb(second);
  backward.absorb(second);
  backward.absorb(first);
  CHECK(forward.absorbed() == 2);
  const auto sealed = forward.seal();
  CHECK(forward.sealed());
  CHECK(forward.seal() == sealed);
  CHECK(sealed != backward.seal());

  // Length prefixing keeps concatenation from aliasing two different exchanges.
  repeated.absorb(std::span(first).first(8));
  repeated.absorb(std::span(first).last(8));
  net::session_transcript whole;
  whole.absorb(first);
  CHECK(repeated.seal() != whole.seal());
}
