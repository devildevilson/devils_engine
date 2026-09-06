#ifndef DEVILS_ENGINE_NETWORK_SESSION_H
#define DEVILS_ENGINE_NETWORK_SESSION_H

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <devils_engine/utils/sha256cpp.h>

#include "replay.h"

namespace devils_engine::network {

// Session compatibility is deliberately stricter than state-schema
// compatibility. A content root covers the product/version and every resolved
// core, project and mod file in canonical load order. It is computed before a
// connection and sent as fixed-size handshake metadata; peers do not hash or
// transfer their installation during the handshake itself.
enum class session_content_domain : std::uint8_t {
  core,
  project,
  mod
};

struct session_content_entry {
  session_content_domain domain = session_content_domain::core;
  // Core/project use zero. Mod files use the resolved mod load position; all
  // files from one mod have the same position and package ID.
  std::uint32_t load_order = 0;
  std::string_view package;
  std::string_view path;
  std::span<const std::byte> bytes;
};

struct session_content_manifest {
  std::string_view product;
  std::string_view version;
  std::span<const session_content_entry> entries;
};

enum class session_content_status : std::uint8_t {
  built,
  empty_product,
  empty_version,
  empty_manifest,
  too_many_entries,
  invalid_domain,
  invalid_name,
  invalid_load_order,
  ambiguous_mod_order,
  out_of_order
};

namespace detail {

inline bool canonical_manifest_name(const std::string_view value) noexcept {
  if (value.empty() || value.front() == '/' || value.back() == '/') return false;
  std::size_t begin = 0;
  while (begin < value.size()) {
    const std::size_t end = value.find('/', begin);
    const std::string_view part = value.substr(
      begin, end == std::string_view::npos ? value.size() - begin : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    for (const char c : part) {
      if (c == '\\' || c == '\0') return false;
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return true;
}

inline auto manifest_key(const session_content_entry& entry) noexcept {
  return std::tuple{
    std::uint8_t(entry.domain), entry.load_order, entry.package, entry.path};
}

inline void sha_u32(utils::SHA256& hash, const std::uint32_t value) {
  std::array<std::byte, 4> bytes{};
  for (unsigned i = 0; i < bytes.size(); ++i)
    bytes[i] = std::byte(std::uint8_t(value >> (i * 8)));
  hash.update(bytes.data(), bytes.size());
}

inline void sha_u64(utils::SHA256& hash, const std::uint64_t value) {
  std::array<std::byte, 8> bytes{};
  for (unsigned i = 0; i < bytes.size(); ++i)
    bytes[i] = std::byte(std::uint8_t(value >> (i * 8)));
  hash.update(bytes.data(), bytes.size());
}

inline void sha_string(utils::SHA256& hash, const std::string_view value) {
  sha_u64(hash, std::uint64_t(value.size()));
  hash.update(value.data(), value.size());
}

} // namespace detail

// Entries must already be in strict (domain, load_order, package, path) order.
// This makes filesystem enumeration order irrelevant while retaining explicit
// mod precedence. Refusal leaves output unchanged. The canonical framing hashes
// metadata and complete file bytes, so equal roots mean equal resolved content
// under SHA-256's collision assumption; the root is not peer authentication.
[[nodiscard]] inline session_content_status try_make_session_content_root(
  const session_content_manifest manifest,
  utils::digest& output) {
  if (manifest.product.empty()) return session_content_status::empty_product;
  if (manifest.version.empty()) return session_content_status::empty_version;
  if (manifest.entries.empty()) return session_content_status::empty_manifest;
  if (manifest.entries.size() > std::numeric_limits<std::uint32_t>::max())
    return session_content_status::too_many_entries;

  const session_content_entry* previous = nullptr;
  for (const auto& entry : manifest.entries) {
    if (std::uint8_t(entry.domain) > std::uint8_t(session_content_domain::mod))
      return session_content_status::invalid_domain;
    if (!detail::canonical_manifest_name(entry.package) ||
        !detail::canonical_manifest_name(entry.path))
      return session_content_status::invalid_name;
    if (entry.domain != session_content_domain::mod && entry.load_order != 0)
      return session_content_status::invalid_load_order;
    if (previous != nullptr) {
      if (entry.domain == session_content_domain::mod &&
          previous->domain == session_content_domain::mod &&
          entry.load_order == previous->load_order &&
          entry.package != previous->package)
        return session_content_status::ambiguous_mod_order;
      if (!(detail::manifest_key(*previous) < detail::manifest_key(entry)))
        return session_content_status::out_of_order;
    }
    previous = &entry;
  }

  utils::SHA256 hash;
  detail::sha_u32(hash, UINT32_C(0x4e53434d)); // "NSCM"
  detail::sha_u32(hash, 1);
  detail::sha_string(hash, manifest.product);
  detail::sha_string(hash, manifest.version);
  detail::sha_u32(hash, std::uint32_t(manifest.entries.size()));
  for (const auto& entry : manifest.entries) {
    detail::sha_u32(hash, std::uint32_t(entry.domain));
    detail::sha_u32(hash, entry.load_order);
    detail::sha_string(hash, entry.package);
    detail::sha_string(hash, entry.path);
    detail::sha_u64(hash, std::uint64_t(entry.bytes.size()));
    if (!entry.bytes.empty()) hash.update(entry.bytes.data(), entry.bytes.size());
  }
  output = hash.finalize();
  return session_content_status::built;
}

// Wire codecs still own integer encoding and framing. handshake_format must be
// checked before decoding any later-version fields. content_root is the strict
// whole-installation identity above; the other fields give a useful refusal
// instead of reducing every incompatibility to "hash differs".
struct session_compatibility {
  std::uint32_t handshake_format = 1;
  std::uint32_t protocol_version = 0;
  std::uint32_t state_schema_fingerprint = 0;
  std::uint32_t intent_schema_fingerprint = 0;
  std::uint32_t numeric_profile = 0;
  utils::digest content_root{};

  bool operator==(const session_compatibility&) const = default;
};

enum class session_compatibility_status : std::uint8_t {
  compatible,
  handshake_format_mismatch,
  protocol_version_mismatch,
  content_mismatch,
  state_schema_mismatch,
  intent_schema_mismatch,
  numeric_profile_mismatch
};

[[nodiscard]] constexpr session_compatibility_status compare_session_compatibility(
  const session_compatibility& local,
  const session_compatibility& remote) noexcept {
  if (local.handshake_format != remote.handshake_format)
    return session_compatibility_status::handshake_format_mismatch;
  if (local.protocol_version != remote.protocol_version)
    return session_compatibility_status::protocol_version_mismatch;
  if (local.content_root != remote.content_root)
    return session_compatibility_status::content_mismatch;
  if (local.state_schema_fingerprint != remote.state_schema_fingerprint)
    return session_compatibility_status::state_schema_mismatch;
  if (local.intent_schema_fingerprint != remote.intent_schema_fingerprint)
    return session_compatibility_status::intent_schema_mismatch;
  if (local.numeric_profile != remote.numeric_profile)
    return session_compatibility_status::numeric_profile_mismatch;
  return session_compatibility_status::compatible;
}

enum class session_handshake_status : std::uint8_t {
  accepted,
  handshake_format_mismatch,
  protocol_version_mismatch,
  content_mismatch,
  state_schema_mismatch,
  intent_schema_mismatch,
  numeric_profile_mismatch,
  identity_rejected
};

template <class Identity>
struct session_handshake_result {
  session_handshake_status status = session_handshake_status::identity_rejected;
  std::optional<Identity> identity;

  constexpr bool accepted() const noexcept {
    return status == session_handshake_status::accepted;
  }
};

namespace detail {

constexpr session_handshake_status handshake_status(
  const session_compatibility_status status) noexcept {
  switch (status) {
    case session_compatibility_status::compatible:
      return session_handshake_status::accepted;
    case session_compatibility_status::handshake_format_mismatch:
      return session_handshake_status::handshake_format_mismatch;
    case session_compatibility_status::protocol_version_mismatch:
      return session_handshake_status::protocol_version_mismatch;
    case session_compatibility_status::content_mismatch:
      return session_handshake_status::content_mismatch;
    case session_compatibility_status::state_schema_mismatch:
      return session_handshake_status::state_schema_mismatch;
    case session_compatibility_status::intent_schema_mismatch:
      return session_handshake_status::intent_schema_mismatch;
    case session_compatibility_status::numeric_profile_mismatch:
      return session_handshake_status::numeric_profile_mismatch;
  }
  return session_handshake_status::identity_rejected;
}

} // namespace detail

// Authentication is injected because a Steam identity, an offline credential
// and a dedicated-server token are different policies. The verifier returns an
// authenticated logical identity (which may carry reconnect claims), never a
// transport handle. It is not called for an incompatible peer.
template <class Credential, class VerifyIdentity>
  requires requires(VerifyIdentity& verify, const Credential& credential) {
    std::invoke(verify, credential);
    requires requires { typename std::invoke_result_t<VerifyIdentity&, const Credential&>::value_type; };
  }
[[nodiscard]] auto evaluate_session_handshake(
  const session_compatibility& local,
  const session_compatibility& remote,
  const Credential& credential,
  VerifyIdentity verify_identity) {
  using optional_identity = std::invoke_result_t<VerifyIdentity&, const Credential&>;
  using identity = typename optional_identity::value_type;
  static_assert(std::same_as<optional_identity, std::optional<identity>>,
                "network session identity verifier must return std::optional<Identity>");

  const auto compatibility = compare_session_compatibility(local, remote);
  if (compatibility != session_compatibility_status::compatible)
    return session_handshake_result<identity>{detail::handshake_status(compatibility), std::nullopt};

  optional_identity authenticated = std::invoke(verify_identity, credential);
  if (!authenticated)
    return session_handshake_result<identity>{session_handshake_status::identity_rejected, std::nullopt};
  return session_handshake_result<identity>{session_handshake_status::accepted,
                                            std::move(authenticated)};
}

// Peer IDs below are logical session participants, not gns_peer values. A
// reconnect changes its transport handle but retains the authenticated
// principal. Authority changes require a strictly newer epoch.
template <class SessionId, class PeerId, class PrincipalId, class Epoch>
struct session_membership {
  SessionId session;
  PeerId local_peer;
  PeerId authority_peer;
  PrincipalId principal;
  Epoch authority_epoch{};

  bool operator==(const session_membership&) const = default;
};

template <class SessionId, class PeerId, class Epoch>
struct authority_message_stamp {
  SessionId session;
  PeerId authority_peer;
  Epoch authority_epoch{};

  bool operator==(const authority_message_stamp&) const = default;
};

enum class authority_message_status : std::uint8_t {
  accepted,
  wrong_session,
  wrong_authority,
  stale_epoch,
  future_epoch
};

template <class SessionId, class PeerId, class PrincipalId,
          std::totally_ordered Epoch>
[[nodiscard]] constexpr authority_message_status classify_authority_message(
  const session_membership<SessionId, PeerId, PrincipalId, Epoch>& membership,
  const authority_message_stamp<SessionId, PeerId, Epoch>& stamp) noexcept {
  if (stamp.session != membership.session)
    return authority_message_status::wrong_session;
  if (stamp.authority_peer != membership.authority_peer)
    return authority_message_status::wrong_authority;
  if (stamp.authority_epoch < membership.authority_epoch)
    return authority_message_status::stale_epoch;
  if (membership.authority_epoch < stamp.authority_epoch)
    return authority_message_status::future_epoch;
  return authority_message_status::accepted;
}

// A reconnect credential is opaque to the library and must be authenticated by
// evaluate_session_handshake. The confirmed anchor is only a recovery hint: the
// authority may select any retained checkpoint not later than its target.
template <class Tick, class Digest>
struct recovery_anchor {
  Tick tick{};
  Digest root{};

  bool operator==(const recovery_anchor&) const = default;
};

template <class SessionId, class Credential, class Tick, class Digest>
struct reconnect_request {
  SessionId session;
  Credential credential;
  std::optional<recovery_anchor<Tick, Digest>> confirmed;
};

// checkpoint_tick is committed state after K. Recovery must carry a sealed
// bundle for every tick K+1..target_tick, including explicit empty ticks.
// checkpoint_root and target_root prevent a locally valid decode/replay from
// publishing the wrong causal state.
template <class SessionId, class PeerId, class PrincipalId, class Epoch,
          class Tick, class Digest>
struct session_recovery_plan {
  SessionId session;
  PeerId authority_peer;
  PrincipalId principal;
  Epoch authority_epoch{};
  Tick checkpoint_tick{};
  Tick target_tick{};
  Digest checkpoint_root{};
  Digest target_root{};
};

enum class session_recovery_status : std::uint8_t {
  recovered,
  wrong_session,
  wrong_authority,
  wrong_principal,
  stale_epoch,
  future_epoch,
  target_before_checkpoint,
  checkpoint_root_mismatch,
  target_root_mismatch,
  replay_refused
};

template <class Tick>
struct session_recovery_result {
  session_recovery_status status = session_recovery_status::replay_refused;
  std::optional<replay_result<Tick>> replay;

  constexpr bool recovered() const noexcept {
    return status == session_recovery_status::recovered;
  }
};

// Candidate must be detached from the live world. replay_to preflights the
// entire bundle range before Restore, then the roots at K and N are checked.
// Publish is the single noexcept live-state mutation and is called only after
// successful replay. Presentation is suppressed by replay_context throughout.
template <
  class Host,
  class SessionId,
  class PeerId,
  class PrincipalId,
  std::totally_ordered Epoch,
  std::totally_ordered Tick,
  class Digest,
  class Checkpoint,
  std::ranges::forward_range BundleRange,
  class Restore,
  class ApplyBundle,
  class Step,
  class StateRoot,
  class Publish,
  class NextTick,
  class TickOf = replay_entry_tick,
  class BundleOf = replay_entry_bundle>
[[nodiscard]] session_recovery_result<Tick> recover_session(
  const session_membership<SessionId, PeerId, PrincipalId, Epoch>& membership,
  const session_recovery_plan<SessionId, PeerId, PrincipalId, Epoch, Tick, Digest>& plan,
  Host& candidate,
  const Checkpoint& checkpoint,
  BundleRange&& bundles,
  Restore restore,
  ApplyBundle apply_bundle,
  Step step,
  StateRoot state_root,
  Publish publish,
  NextTick next_tick,
  TickOf tick_of = {},
  BundleOf bundle_of = {}) {
  static_assert(std::is_nothrow_invocable_v<Publish&, Host&> &&
                  std::same_as<std::invoke_result_t<Publish&, Host&>, void>,
                "network recovery publish must be noexcept and return void");

  if (plan.session != membership.session)
    return {session_recovery_status::wrong_session, std::nullopt};
  if (plan.authority_peer != membership.authority_peer)
    return {session_recovery_status::wrong_authority, std::nullopt};
  if (plan.principal != membership.principal)
    return {session_recovery_status::wrong_principal, std::nullopt};
  if (plan.authority_epoch < membership.authority_epoch)
    return {session_recovery_status::stale_epoch, std::nullopt};
  if (membership.authority_epoch < plan.authority_epoch)
    return {session_recovery_status::future_epoch, std::nullopt};
  if (plan.target_tick < plan.checkpoint_tick)
    return {session_recovery_status::target_before_checkpoint, std::nullopt};

  std::optional<Tick> root_mismatch;
  replay_result<Tick> replayed = replay_to(
    candidate,
    plan.checkpoint_tick,
    checkpoint,
    plan.target_tick,
    std::forward<BundleRange>(bundles),
    std::move(restore),
    std::move(apply_bundle),
    std::move(step),
    [&](const Host& state, const Tick tick) {
      if (tick != plan.checkpoint_tick && tick != plan.target_tick) return true;
      const Digest actual = std::invoke(state_root, state);
      if (tick == plan.checkpoint_tick && actual != plan.checkpoint_root) {
        root_mismatch = tick;
        return false;
      }
      if (tick == plan.target_tick && actual != plan.target_root) {
        root_mismatch = tick;
        return false;
      }
      return true;
    },
    std::move(next_tick),
    std::move(tick_of),
    std::move(bundle_of));

  if (!replayed.completed()) {
    if (root_mismatch == plan.checkpoint_tick)
      return {session_recovery_status::checkpoint_root_mismatch, replayed};
    if (root_mismatch == plan.target_tick)
      return {session_recovery_status::target_root_mismatch, replayed};
    return {session_recovery_status::replay_refused, replayed};
  }

  std::invoke(publish, candidate);
  return {session_recovery_status::recovered, replayed};
}

} // namespace devils_engine::network

#endif
