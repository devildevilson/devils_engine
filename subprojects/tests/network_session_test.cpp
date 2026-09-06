#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace net = devils_engine::network;

namespace {

template <std::size_t Size>
std::span<const std::byte> bytes(const std::array<std::byte, Size>& value) {
  return value;
}

struct authenticated_peer {
  std::uint64_t principal = 0;
  bool reconnect = false;

  bool operator==(const authenticated_peer&) const = default;
};

struct causal_state {
  unsigned tick = 0;
  std::uint64_t random_state = 1;
  std::uint64_t next_entity = 10;
  std::uint64_t timeline_phase = 0;
  std::int64_t value = 0;

  bool operator==(const causal_state&) const = default;
};

struct host {
  causal_state causal;
  unsigned presentation_events = 0;
};

struct checkpoint {
  causal_state causal;
};

struct incomplete_checkpoint {
  unsigned tick = 0;
  std::uint64_t next_entity = 0;
  std::uint64_t timeline_phase = 0;
  std::int64_t value = 0;
};

struct bundle_entry {
  unsigned tick = 0;
  std::int64_t intent = 0;
};

std::uint64_t root(const host& value) noexcept {
  std::uint64_t result = UINT64_C(0xcbf29ce484222325);
  const auto mix = [&result](const std::uint64_t part) {
    result ^= part;
    result *= UINT64_C(0x100000001b3);
  };
  mix(value.causal.tick);
  mix(value.causal.random_state);
  mix(value.causal.next_entity);
  mix(value.causal.timeline_phase);
  mix(std::uint64_t(value.causal.value));
  return result;
}

void apply_tick(host& value, const bundle_entry& bundle,
                const bool presentation) {
  value.causal.random_state =
    value.causal.random_state * UINT64_C(6364136223846793005) + 1;
  value.causal.value += bundle.intent;
  value.causal.value += std::int64_t(value.causal.random_state >> 61);
  if ((value.causal.value & 1) != 0) ++value.causal.next_entity;
  value.causal.timeline_phase = (value.causal.timeline_phase + 17) % 60;
  value.causal.tick = bundle.tick;
  if (presentation) ++value.presentation_events;
}

using membership = net::session_membership<unsigned, unsigned, unsigned, unsigned>;
using recovery_plan =
  net::session_recovery_plan<unsigned, unsigned, unsigned, unsigned,
                             unsigned, std::uint64_t>;

constexpr membership member{7, 2, 1, 42, 3};

template <class Checkpoint>
auto run_recovery(host& candidate, host& live, const Checkpoint& saved,
                  const std::span<const bundle_entry> bundles,
                  recovery_plan plan, unsigned& restore_calls,
                  unsigned& publish_calls) {
  return net::recover_session(
    member, plan, candidate, saved, bundles,
    [&restore_calls](host& value, const Checkpoint& source) {
      ++restore_calls;
      if constexpr (std::same_as<Checkpoint, checkpoint>) {
        value.causal = source.causal;
      } else {
        // Deliberately leaves the PRNG cursor untouched: this is the hidden
        // mutable-state bug which a complete checkpoint must prevent.
        value.causal.tick = source.tick;
        value.causal.next_entity = source.next_entity;
        value.causal.timeline_phase = source.timeline_phase;
        value.causal.value = source.value;
      }
      return true;
    },
    [](host& value, const bundle_entry& bundle,
       const net::replay_context& context) {
      CHECK(context.presentation_suppressed());
      value.causal.value += bundle.intent;
      return true;
    },
    [](host& value, const unsigned tick, const net::replay_context& context) {
      CHECK(context.presentation_suppressed());
      value.causal.random_state =
        value.causal.random_state * UINT64_C(6364136223846793005) + 1;
      value.causal.value += std::int64_t(value.causal.random_state >> 61);
      if ((value.causal.value & 1) != 0) ++value.causal.next_entity;
      value.causal.timeline_phase = (value.causal.timeline_phase + 17) % 60;
      value.causal.tick = tick;
      return true;
    },
    root,
    [&live, &publish_calls](host& value) noexcept {
      ++publish_calls;
      const unsigned presentation = live.presentation_events;
      live = value;
      live.presentation_events = presentation;
    },
    net::checked_tick_successor<unsigned>{},
    [](const bundle_entry& value) {
      return value.tick;
    },
    [](const bundle_entry& value) {
      return value;
    });
}

} // namespace

TEST_CASE("network content manifest hashes exact canonical core project and mod bytes") {
  const std::array core{std::byte{1}, std::byte{2}};
  const std::array project{std::byte{3}};
  const std::array mod_a{std::byte{4}, std::byte{5}};
  const std::array mod_b{std::byte{6}};
  const std::array entries{
    net::session_content_entry{net::session_content_domain::core, 0,
                               "engine", "rules/core.tavl", bytes(core)},
    net::session_content_entry{net::session_content_domain::project, 0,
                               "frontier", "rules/world.tavl", bytes(project)},
    net::session_content_entry{net::session_content_domain::mod, 0,
                               "weather", "data/rain.tavl", bytes(mod_a)},
    net::session_content_entry{net::session_content_domain::mod, 1,
                               "units", "data/actors.tavl", bytes(mod_b)},
  };
  devils_engine::utils::digest expected{};
  REQUIRE(net::try_make_session_content_root(
            {"tile_frontier", "0.1.0", entries}, expected) ==
          net::session_content_status::built);
  constexpr devils_engine::utils::digest canonical_root{
    0x02,
    0x06,
    0x4d,
    0x28,
    0x80,
    0xd5,
    0x85,
    0x87,
    0xed,
    0xdc,
    0xf9,
    0xc9,
    0x46,
    0x9d,
    0x96,
    0xe0,
    0xae,
    0x3a,
    0x88,
    0xb5,
    0xc3,
    0xa1,
    0xaa,
    0x90,
    0xd2,
    0xe0,
    0x3f,
    0xd7,
    0x76,
    0x30,
    0x1e,
    0x55,
  };
  CHECK(expected == canonical_root);

  auto changed_entries = entries;
  const std::array changed{std::byte{4}, std::byte{7}};
  changed_entries[2].bytes = bytes(changed);
  devils_engine::utils::digest changed_root{};
  REQUIRE(net::try_make_session_content_root(
            {"tile_frontier", "0.1.0", changed_entries}, changed_root) ==
          net::session_content_status::built);
  CHECK(changed_root != expected);

  REQUIRE(net::try_make_session_content_root(
            {"tile_frontier", "0.1.1", entries}, changed_root) ==
          net::session_content_status::built);
  CHECK(changed_root != expected);

  changed_entries = entries;
  changed_entries[2].load_order = 2;
  changed_entries[3].load_order = 1;
  changed_root.fill(0x5a);
  const auto unchanged = changed_root;
  CHECK(net::try_make_session_content_root(
          {"tile_frontier", "0.1.0", changed_entries}, changed_root) ==
        net::session_content_status::out_of_order);
  CHECK(changed_root == unchanged);

  changed_entries = entries;
  changed_entries[3].load_order = 0;
  CHECK(net::try_make_session_content_root(
          {"tile_frontier", "0.1.0", changed_entries}, changed_root) ==
        net::session_content_status::ambiguous_mod_order);

  changed_entries = entries;
  changed_entries[1].path = "../outside";
  CHECK(net::try_make_session_content_root(
          {"tile_frontier", "0.1.0", changed_entries}, changed_root) ==
        net::session_content_status::invalid_name);

  changed_entries = entries;
  changed_entries[0].domain = net::session_content_domain(99);
  CHECK(net::try_make_session_content_root(
          {"tile_frontier", "0.1.0", changed_entries}, changed_root) ==
        net::session_content_status::invalid_domain);
}

TEST_CASE("network handshake refuses compatibility before authenticating identity") {
  net::session_compatibility local;
  local.protocol_version = 9;
  local.state_schema_fingerprint = 10;
  local.intent_schema_fingerprint = 11;
  local.numeric_profile = 12;
  local.content_root[0] = 13;

  auto incompatible = local;
  ++incompatible.handshake_format;
  CHECK(net::compare_session_compatibility(local, incompatible) ==
        net::session_compatibility_status::handshake_format_mismatch);
  incompatible = local;
  ++incompatible.protocol_version;
  CHECK(net::compare_session_compatibility(local, incompatible) ==
        net::session_compatibility_status::protocol_version_mismatch);
  incompatible = local;
  ++incompatible.state_schema_fingerprint;
  CHECK(net::compare_session_compatibility(local, incompatible) ==
        net::session_compatibility_status::state_schema_mismatch);
  incompatible = local;
  ++incompatible.intent_schema_fingerprint;
  CHECK(net::compare_session_compatibility(local, incompatible) ==
        net::session_compatibility_status::intent_schema_mismatch);
  incompatible = local;
  ++incompatible.numeric_profile;
  CHECK(net::compare_session_compatibility(local, incompatible) ==
        net::session_compatibility_status::numeric_profile_mismatch);

  unsigned authentications = 0;
  const auto authenticate = [&authentications](const std::uint64_t token)
    -> std::optional<authenticated_peer> {
    ++authentications;
    if (token != 99) return std::nullopt;
    return authenticated_peer{42, true};
  };

  auto remote = local;
  remote.content_root[0] ^= 1;
  auto result = net::evaluate_session_handshake(local, remote, 99u, authenticate);
  CHECK(result.status == net::session_handshake_status::content_mismatch);
  CHECK(authentications == 0);

  result = net::evaluate_session_handshake(local, local, 1u, authenticate);
  CHECK(result.status == net::session_handshake_status::identity_rejected);
  CHECK(authentications == 1);

  result = net::evaluate_session_handshake(local, local, 99u, authenticate);
  REQUIRE(result.accepted());
  REQUIRE(result.identity.has_value());
  CHECK(result.identity->principal == 42);
  CHECK(result.identity->reconnect);
  CHECK(authentications == 2);
}

TEST_CASE("network authority gate separates logical membership from transport reconnect") {
  const net::authority_message_stamp<unsigned, unsigned, unsigned> current{7, 1, 3};
  CHECK(net::classify_authority_message(member, current) ==
        net::authority_message_status::accepted);
  CHECK(net::classify_authority_message(
          member, net::authority_message_stamp<unsigned, unsigned, unsigned>{8, 1, 3}) ==
        net::authority_message_status::wrong_session);
  CHECK(net::classify_authority_message(
          member, net::authority_message_stamp<unsigned, unsigned, unsigned>{7, 9, 3}) ==
        net::authority_message_status::wrong_authority);
  CHECK(net::classify_authority_message(
          member, net::authority_message_stamp<unsigned, unsigned, unsigned>{7, 1, 2}) ==
        net::authority_message_status::stale_epoch);
  CHECK(net::classify_authority_message(
          member, net::authority_message_stamp<unsigned, unsigned, unsigned>{7, 1, 4}) ==
        net::authority_message_status::future_epoch);
}

TEST_CASE("network reconnect publishes checkpoint plus every later intent transactionally") {
  const std::array all{
    bundle_entry{1, 2},
    bundle_entry{2, 3},
    bundle_entry{3, 5},
    bundle_entry{4, 0},
    bundle_entry{5, 7},
    bundle_entry{6, 4},
  };
  host uninterrupted;
  for (const auto& bundle : all)
    apply_tick(uninterrupted, bundle, true);

  host at_checkpoint;
  apply_tick(at_checkpoint, all[0], true);
  apply_tick(at_checkpoint, all[1], true);
  const checkpoint saved{at_checkpoint.causal};

  host live;
  live.presentation_events = 91;
  host candidate{{99, 123, 456, 17, -1}, 800};
  recovery_plan plan{7, 1, 42, 3, 2, 6, root(at_checkpoint), root(uninterrupted)};
  unsigned restores = 0, publishes = 0;
  const auto result = run_recovery(
    candidate, live, saved, std::span(all).subspan(2), plan, restores, publishes);

  REQUIRE(result.recovered());
  REQUIRE(result.replay.has_value());
  CHECK(result.replay->replayed_ticks == 4);
  CHECK(restores == 1);
  CHECK(publishes == 1);
  CHECK(live.causal == uninterrupted.causal);
  CHECK(live.presentation_events == 91);

  host rejected_live;
  const host original_rejected = rejected_live;
  host rejected_candidate;
  plan.target_root ^= 1;
  restores = 0;
  publishes = 0;
  const auto rejected = run_recovery(rejected_candidate, rejected_live, saved,
                                     std::span(all).subspan(2), plan, restores,
                                     publishes);
  CHECK(rejected.status == net::session_recovery_status::target_root_mismatch);
  CHECK(restores == 1);
  CHECK(publishes == 0);
  CHECK(rejected_live.causal == original_rejected.causal);
}

TEST_CASE("network reconnect refuses identity epoch and incomplete history before restore") {
  const checkpoint saved{{2, 5, 11, 34, 9}};
  const std::array history{bundle_entry{3, 1}, bundle_entry{5, 2}};
  host candidate, live;
  unsigned restores = 0, publishes = 0;
  recovery_plan plan{7, 1, 42, 3, 2, 5, root(host{saved.causal}), 0};

  plan.session = 8;
  auto result =
    run_recovery(candidate, live, saved, history, plan, restores, publishes);
  CHECK(result.status == net::session_recovery_status::wrong_session);
  CHECK(restores == 0);

  plan.session = 7;
  plan.authority_peer = 9;
  result = run_recovery(candidate, live, saved, history, plan, restores, publishes);
  CHECK(result.status == net::session_recovery_status::wrong_authority);
  CHECK(restores == 0);

  plan.authority_peer = 1;
  plan.authority_epoch = 2;
  result = run_recovery(candidate, live, saved, history, plan, restores, publishes);
  CHECK(result.status == net::session_recovery_status::stale_epoch);
  CHECK(restores == 0);

  plan.authority_epoch = 4;
  result = run_recovery(candidate, live, saved, history, plan, restores, publishes);
  CHECK(result.status == net::session_recovery_status::future_epoch);
  CHECK(restores == 0);

  plan.authority_epoch = 3;
  plan.principal = 99;
  result = run_recovery(candidate, live, saved, history, plan, restores, publishes);
  CHECK(result.status == net::session_recovery_status::wrong_principal);
  CHECK(restores == 0);

  plan.principal = 42;
  result = run_recovery(candidate, live, saved, history, plan, restores, publishes);
  REQUIRE(result.status == net::session_recovery_status::replay_refused);
  REQUIRE(result.replay.has_value());
  CHECK(result.replay->status == net::replay_status::missing_bundle);
  CHECK(restores == 0);
  CHECK(publishes == 0);
}

TEST_CASE("network reconnect detects causal state omitted from checkpoint") {
  const std::array all{bundle_entry{1, 2}, bundle_entry{2, 3},
                       bundle_entry{3, 5}, bundle_entry{4, 7}};
  host authoritative;
  apply_tick(authoritative, all[0], false);
  apply_tick(authoritative, all[1], false);
  const host at_checkpoint = authoritative;
  apply_tick(authoritative, all[2], false);
  apply_tick(authoritative, all[3], false);

  const incomplete_checkpoint saved{
    at_checkpoint.causal.tick,
    at_checkpoint.causal.next_entity,
    at_checkpoint.causal.timeline_phase,
    at_checkpoint.causal.value};
  host candidate;
  candidate.causal.random_state = 999; // hidden cursor not restored
  host live;
  const host original_live = live;
  recovery_plan plan{7, 1, 42, 3, 2, 4,
                     root(at_checkpoint), root(authoritative)};
  unsigned restores = 0, publishes = 0;
  const auto result = run_recovery(
    candidate, live, saved, std::span(all).subspan(2), plan, restores, publishes);

  CHECK(result.status == net::session_recovery_status::checkpoint_root_mismatch);
  CHECK(restores == 1);
  CHECK(publishes == 0);
  CHECK(live.causal == original_live.causal);
}
