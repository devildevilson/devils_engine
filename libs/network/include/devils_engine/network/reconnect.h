#ifndef DEVILS_ENGINE_NETWORK_RECONNECT_H
#define DEVILS_ENGINE_NETWORK_RECONNECT_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "replay.h"

// Automatic reconnect, as declared policy and one ordered machine.
//
// Nothing here opens a socket, sends a byte or replays a tick: the caller
// performs the transport work and drives the handshake and `recover_session`.
// What the library owns is what must not be guessed — when a connection counts
// as lost, how long to keep trying, when to stop, and whether recovery is even
// possible from the history that is still retained.
//
// The library reads no clock. Every instant is caller-declared, in the same
// unit the caller uses for credential validity, so a reconnect deadline and a
// ticket expiry cannot drift apart.

namespace devils_engine::network {

// Silence is not loss. A single slow tick, a stalled frame or a scheduler
// hiccup must not tear down a session, so suspicion and loss are separate
// budgets: the first is worth telling the presentation about, only the second
// is worth reconnecting for.
struct reconnect_policy {
  uint64_t suspect_after = 0;
  uint64_t lost_after = 0;
  uint64_t first_backoff = 0;
  uint64_t max_backoff = 0;
  uint32_t max_attempts = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return suspect_after > 0 && lost_after > suspect_after && first_backoff > 0 &&
           max_backoff >= first_backoff && max_attempts > 0;
  }

  // Deterministic doubling with a cap. Jitter is deliberately absent: the
  // library owns no randomness, and a caller which needs to spread a crowd of
  // reconnecting clients can add its own on top of this value.
  [[nodiscard]] constexpr uint64_t backoff_for(const uint32_t attempt) const noexcept {
    if (attempt <= 1) return first_backoff;
    uint64_t value = first_backoff;
    for (uint32_t i = 1; i < attempt; ++i) {
      if (value > max_backoff / 2) return max_backoff;
      value *= 2;
    }
    return value > max_backoff ? max_backoff : value;
  }
};

enum class reconnect_state : uint8_t {
  live,
  suspect,
  lost,
  attempting,
  handshaking,
  recovering,
  abandoned
};

enum class reconnect_action : uint8_t {
  none,
  // Suspicion began. Worth showing, not worth reconnecting for.
  warn,
  // Open a fresh transport connection. A reconnect is a new connection, never
  // an implicit retry of the old handle.
  connect,
  // Back off; poll again at the returned instant.
  wait,
  // The transport is up: run the handshake carrying the reconnect claim.
  handshake,
  // Admitted: request the checkpoint and the sealed bundle range.
  recover,
  // Recovery is impossible from what is still retained. Joining fresh is the
  // correct outcome here, not a failure.
  rejoin,
  give_up
};

enum class abandon_reason : uint8_t {
  none,
  attempts_exhausted,
  // The reconnect deadline passed. That deadline is the reconnect ticket's own
  // expiry, so the client and the authority cannot disagree about it.
  deadline_passed,
  refused,
  unrecoverable
};

struct reconnect_decision {
  reconnect_action action = reconnect_action::none;
  // Meaningful for `wait`: the instant at which polling is worthwhile again.
  uint64_t next_instant = 0;
};

// The ordered client machine. Observations are what the caller learned;
// `poll` is what the caller should do now.
class reconnect_coordinator {
public:
  // `deadline` should be the reconnect ticket's `expires_at`: attempting after
  // it cannot succeed, because the authority will refuse the credential.
  reconnect_coordinator(const reconnect_policy& policy, const uint64_t deadline,
                        const uint64_t now) noexcept
    : policy_(policy), deadline_(deadline), last_traffic_(now) {}

  [[nodiscard]] reconnect_state state() const noexcept {
    return state_;
  }
  [[nodiscard]] uint32_t attempts() const noexcept {
    return attempts_;
  }
  [[nodiscard]] abandon_reason abandoned_because() const noexcept {
    return abandon_;
  }
  [[nodiscard]] bool abandoned() const noexcept {
    return state_ == reconnect_state::abandoned;
  }

  // Traffic resurrects a session from suspicion or from a declared loss the
  // caller has not acted on yet — the common case of a silence spike which
  // resolves itself, where reconnecting would be pure cost. It does NOT
  // resurrect from `attempting` onward: by then a fresh connection is in
  // flight, and bytes from the old handle are ambiguous rather than reassuring.
  void observe_traffic(const uint64_t now) noexcept {
    if (state_ == reconnect_state::live || state_ == reconnect_state::suspect ||
        state_ == reconnect_state::lost) {
      state_ = reconnect_state::live;
      attempts_ = 0;
      last_traffic_ = now;
      warned_ = false;
    }
  }

  // The transport itself reported a terminal state. This is direct evidence, so
  // it skips the silence budget entirely.
  void observe_transport_lost(const uint64_t now) noexcept {
    if (state_ == reconnect_state::abandoned) return;
    state_ = reconnect_state::lost;
    retry_at_ = now;
  }

  void observe_connected(const uint64_t now) noexcept {
    if (state_ == reconnect_state::attempting) {
      state_ = reconnect_state::handshaking;
      last_traffic_ = now;
    }
  }

  // The transport attempt itself failed. It costs an attempt, exactly like a
  // refused handshake would, so a peer which cannot be reached does not retry
  // forever.
  void observe_connect_failed(const uint64_t now) noexcept {
    if (state_ == reconnect_state::attempting || state_ == reconnect_state::handshaking) {
      state_ = reconnect_state::lost;
      retry_at_ = saturating_add(now, policy_.backoff_for(attempts_));
    }
  }

  void observe_admitted(const uint64_t now) noexcept {
    if (state_ == reconnect_state::handshaking) {
      state_ = reconnect_state::recovering;
      last_traffic_ = now;
    }
  }

  // A terminal refusal from the authority. Retrying an incompatible build or a
  // rejected credential on the same schedule would only spend the deadline.
  void observe_refused(const uint64_t) noexcept {
    state_ = reconnect_state::abandoned;
    abandon_ = abandon_reason::refused;
  }

  void observe_recovered(const uint64_t now) noexcept {
    if (state_ == reconnect_state::recovering) {
      state_ = reconnect_state::live;
      attempts_ = 0;
      warned_ = false;
      last_traffic_ = now;
    }
  }

  // The authority holds no history from which this client can be replayed. The
  // session is over, but the player is not: the caller joins fresh.
  void observe_unrecoverable(const uint64_t) noexcept {
    state_ = reconnect_state::abandoned;
    abandon_ = abandon_reason::unrecoverable;
  }

  [[nodiscard]] reconnect_decision poll(const uint64_t now) noexcept {
    switch (state_) {
      case reconnect_state::abandoned:
        return {abandon_ == abandon_reason::unrecoverable ? reconnect_action::rejoin
                                                          : reconnect_action::give_up,
                0};
      case reconnect_state::live:
      case reconnect_state::suspect:
        return poll_silence(now);
      case reconnect_state::lost:
        return poll_lost(now);
      case reconnect_state::attempting:
        return {reconnect_action::none, 0};
      case reconnect_state::handshaking:
        return {reconnect_action::handshake, 0};
      case reconnect_state::recovering:
        return {reconnect_action::recover, 0};
    }
    return {reconnect_action::none, 0};
  }

private:
  static uint64_t saturating_add(const uint64_t left, const uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
             ? std::numeric_limits<uint64_t>::max()
             : left + right;
  }

  reconnect_decision poll_silence(const uint64_t now) noexcept {
    const uint64_t silence = now > last_traffic_ ? now - last_traffic_ : 0;
    if (silence >= policy_.lost_after) {
      state_ = reconnect_state::lost;
      retry_at_ = now;
      return poll_lost(now);
    }
    if (silence >= policy_.suspect_after) {
      state_ = reconnect_state::suspect;
      if (!warned_) {
        warned_ = true;
        return {reconnect_action::warn, saturating_add(last_traffic_, policy_.lost_after)};
      }
      return {reconnect_action::none, saturating_add(last_traffic_, policy_.lost_after)};
    }
    state_ = reconnect_state::live;
    return {reconnect_action::none, saturating_add(last_traffic_, policy_.suspect_after)};
  }

  reconnect_decision poll_lost(const uint64_t now) noexcept {
    // The deadline is checked before the attempt budget: a client whose ticket
    // has expired must not spend attempts discovering that.
    if (now >= deadline_) {
      state_ = reconnect_state::abandoned;
      abandon_ = abandon_reason::deadline_passed;
      return {reconnect_action::give_up, 0};
    }
    if (attempts_ >= policy_.max_attempts) {
      state_ = reconnect_state::abandoned;
      abandon_ = abandon_reason::attempts_exhausted;
      return {reconnect_action::give_up, 0};
    }
    if (now < retry_at_) return {reconnect_action::wait, retry_at_};
    ++attempts_;
    state_ = reconnect_state::attempting;
    return {reconnect_action::connect, 0};
  }

  reconnect_policy policy_;
  uint64_t deadline_ = 0;
  uint64_t last_traffic_ = 0;
  uint64_t retry_at_ = 0;
  uint32_t attempts_ = 0;
  reconnect_state state_ = reconnect_state::live;
  abandon_reason abandon_ = abandon_reason::none;
  bool warned_ = false;
};

// Authority side. A session whose peer disappeared is retained for a declared
// window so its owner can come back, and reaped afterwards. Capacity is
// declared, because a table which grows with disappearing peers is an
// allocation a peer controls.
//
// Consult this table only AFTER the reconnect credential verified. The order
// matters: the credential proves the principal, so a stranger cannot use
// resolution answers to discover which sessions exist.
class session_hold_table {
public:
  enum class hold_status : uint8_t { held, at_capacity, already_held, invalid_window };
  enum class resolve_status : uint8_t {
    held,
    unknown,
    expired,
    wrong_principal,
    stale_epoch,
    future_epoch
  };

  explicit session_hold_table(const size_t capacity) {
    entries_.reserve(capacity);
    capacity_ = capacity;
  }

  [[nodiscard]] hold_status hold(const uint64_t session, const uint64_t principal,
                                 const uint64_t authority_epoch, const uint64_t now,
                                 const uint64_t until) {
    if (until <= now) return hold_status::invalid_window;
    if (find(session) != nullptr) return hold_status::already_held;
    if (entries_.size() == capacity_) return hold_status::at_capacity;
    entries_.push_back(entry{session, principal, authority_epoch, until});
    return hold_status::held;
  }

  [[nodiscard]] resolve_status resolve(const uint64_t session, const uint64_t principal,
                                      const uint64_t authority_epoch,
                                      const uint64_t now) const noexcept {
    const entry* held = find(session);
    if (held == nullptr) return resolve_status::unknown;
    if (held->principal != principal) return resolve_status::wrong_principal;
    // The epoch presented is compared against the one this hold was recorded
    // at, in the same orientation as `classify_authority_message` and the
    // credential: older than the record is stale. Detecting that the AUTHORITY
    // itself migrated is the credential's job, since only it knows the
    // authority's current epoch.
    if (authority_epoch < held->authority_epoch) return resolve_status::stale_epoch;
    if (authority_epoch > held->authority_epoch) return resolve_status::future_epoch;
    if (now >= held->expires_at) return resolve_status::expired;
    return resolve_status::held;
  }

  void release(const uint64_t session) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [session](const entry& value) {
      return value.session == session;
    });
    if (it != entries_.end()) entries_.erase(it);
  }

  // Expired holds are dropped explicitly rather than lazily, so the capacity a
  // caller declared is the capacity it actually gets.
  size_t reap(const uint64_t now) {
    const auto removed = std::remove_if(entries_.begin(), entries_.end(),
                                        [now](const entry& value) {
                                          return now >= value.expires_at;
                                        });
    const size_t count = size_t(entries_.end() - removed);
    entries_.erase(removed, entries_.end());
    return count;
  }

  [[nodiscard]] size_t size() const noexcept {
    return entries_.size();
  }
  [[nodiscard]] size_t capacity() const noexcept {
    return capacity_;
  }

private:
  struct entry {
    uint64_t session = 0;
    uint64_t principal = 0;
    uint64_t authority_epoch = 0;
    uint64_t expires_at = 0;
  };

  [[nodiscard]] const entry* find(const uint64_t session) const noexcept {
    for (const auto& value : entries_) {
      if (value.session == session) return &value;
    }
    return nullptr;
  }

  std::vector<entry> entries_;
  size_t capacity_ = 0;
};

enum class recovery_feasibility : uint8_t {
  recoverable,
  // No retained checkpoint at or before the target.
  no_checkpoint,
  // A checkpoint exists but the bundles which follow it were already evicted,
  // so the gap cannot be replayed. This is the outcome which measures the
  // retention budget, and answering it honestly is why the client has a
  // `rejoin` action at all.
  history_gap,
  target_before_checkpoint
};

template <class Tick>
struct recovery_range {
  Tick checkpoint_tick{};
  Tick target_tick{};

  bool operator==(const recovery_range&) const = default;
};

// A checkpoint at K is the committed state AFTER tick K, so replay needs a
// sealed bundle for every tick K+1..N, including explicitly empty ones. The
// only history which suffices therefore starts at or before K+1 — a history
// whose oldest retained bundle is K+2 leaves a hole no amount of replay fills.
// A target equal to the checkpoint is recoverable with zero replayed ticks.
template <class Tick, class NextTick = checked_tick_successor<Tick>>
[[nodiscard]] recovery_feasibility assess_recovery(
  const std::optional<Tick>& checkpoint_at_or_before_target,
  const std::optional<Tick>& oldest_retained_bundle, const Tick& target,
  recovery_range<Tick>& output, NextTick next_tick = {}) {
  if (!checkpoint_at_or_before_target) return recovery_feasibility::no_checkpoint;
  const Tick& checkpoint = *checkpoint_at_or_before_target;
  if (target < checkpoint) return recovery_feasibility::target_before_checkpoint;
  if (checkpoint < target) {
    if (!oldest_retained_bundle) return recovery_feasibility::history_gap;
    const auto first_required = next_tick(checkpoint);
    if (!first_required) return recovery_feasibility::history_gap;
    if (*first_required < *oldest_retained_bundle) return recovery_feasibility::history_gap;
  }
  output.checkpoint_tick = checkpoint;
  output.target_tick = target;
  return recovery_feasibility::recoverable;
}

} // namespace devils_engine::network

#endif
