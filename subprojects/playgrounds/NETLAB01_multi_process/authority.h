#ifndef DEVILS_ENGINE_NETLAB01_AUTHORITY_H
#define DEVILS_ENGINE_NETLAB01_AUTHORITY_H

#include "link.h"
#include "protocol.h"

#include <random>

// The authority process. It owns the tick clock, seals canonical bundles,
// retains checkpoints and bundle history, mints reconnect tickets for itself
// and answers a resume. It never trusts a peer for its own identity: session,
// principal and epoch come from its own record, and the acting entity is
// implied by the connection rather than asserted by the client.

namespace netlab01 {

// Every injected failure names the roster position it hits, because with
// several followers "the follower" is not a thing. One follower per failure and
// one left alone: a stand where every peer is disturbed cannot tell whether the
// undisturbed path survives a neighbour's recovery.
struct lab_schedule {
  size_t followers = lab_max_followers;
  // Silence budgets. Derived from the tick period by default rather than fixed
  // in the follower: 60/220 ms is right for a 4 ms laboratory tick and much too
  // tight for a slower one, where a single missed tick would look like a loss.
  // 0 means "derive".
  uint64_t suspect_after_ms = 0;
  uint64_t lost_after_ms = 0;
  uint64_t final_tick = 70;
  // Authority closes the connection explicitly. That follower then learns of
  // the loss from the transport itself and skips its silence budget entirely.
  uint64_t explicit_close_tick = 20;
  size_t explicit_close_follower = 0;
  // Authority stops sending to one follower WITHOUT closing. Nothing reports
  // anything, so only that follower's silence budget can notice.
  uint64_t quiet_from_tick = 45;
  size_t quiet_follower = 1;
  // Follower leaves without a goodbye. 0 disables it.
  uint64_t self_exit_tick = 0;
  size_t self_exit_follower = 0;
  uint64_t checkpoint_every = 8;
  uint64_t tick_period_ms = 4;
  uint64_t ticket_window_ms = 20000;
  uint64_t hold_window_ms = 20000;
  // One batch deliberately proposing ticks the authority has already sealed.
  // Nothing about loopback produces a late intent on its own, and an ingress
  // branch nothing ever reaches is not a verified refusal. 0 disables it.
  uint64_t stale_batch_tick = 30;
  size_t stale_batch_follower = 2;
  // Ticks the run is EXTENDED by once the last expected reconnect is admitted.
  // A peer which rejoins exactly at the end recovers and then has nothing to
  // do, which proves it caught up but not that it participates again. 0 keeps
  // the run's length fixed.
  uint64_t resume_tail_ticks = 0;
  // One membership publication deliberately delayed by a tick, so the frames
  // which reference its generation arrive BEFORE the message that gives their
  // slots meaning. Nothing about loopback produces that race on its own — the
  // reliable lane is the higher-priority one and there is no loss to reorder
  // anything — and a refusal branch nothing ever reaches is not a verified
  // refusal. 0 disables it.
  // The tick has to be one where a publication AND a frame send both fall due,
  // or there is nothing to race: membership publishes every
  // `lab_relevance_every_ticks` and the near class every three ticks, so the
  // race can only be staged where those coincide.
  uint64_t membership_delay_tick = 60;
  size_t membership_delay_follower = 2;
  // The other half of the same race: one frame built against the PREVIOUS
  // generation and sent after the current membership has already landed.
  uint64_t stale_generation_tick = 64;
  size_t stale_generation_follower = 2;
  // Ticks ahead the follower proposes. 4 was chosen for a laboratory loopback;
  // on a real link it is the whole margin budget, so it has to be adjustable
  // without a rebuild.
  uint64_t intent_lead_ticks = 4;

  // A scheduled failure names a roster position, and a reduced roster may not
  // have it. Asking that question in one place is the fix for a whole class of
  // bug: with `--followers 2` the stand demanded a stale batch from a follower
  // which was never launched, and with `--followers 1` the authority waited
  // forever for a reconnect that could not happen.
  [[nodiscard]] bool hits_roster(const uint64_t tick, const size_t which) const noexcept {
    return tick != 0 && which < followers;
  }
  [[nodiscard]] bool closes_someone() const noexcept {
    return hits_roster(explicit_close_tick, explicit_close_follower);
  }
  [[nodiscard]] bool quiets_someone() const noexcept {
    return hits_roster(quiet_from_tick, quiet_follower);
  }
  [[nodiscard]] bool kills_someone() const noexcept {
    return hits_roster(self_exit_tick, self_exit_follower);
  }
  [[nodiscard]] bool staleness_tested() const noexcept {
    return hits_roster(stale_batch_tick, stale_batch_follower);
  }
  [[nodiscard]] bool delays_membership() const noexcept {
    return hits_roster(membership_delay_tick, membership_delay_follower);
  }
  [[nodiscard]] bool sends_stale_generation() const noexcept {
    return hits_roster(stale_generation_tick, stale_generation_follower);
  }

  // Joins plus exactly the reconnects the roster can actually produce.
  [[nodiscard]] uint64_t admissions_expected() const noexcept {
    return uint64_t(followers) + (closes_someone() ? 1 : 0) + (quiets_someone() ? 1 : 0) +
           (kills_someone() ? 1 : 0);
  }

  [[nodiscard]] uint64_t suspect_ms() const noexcept {
    if (suspect_after_ms != 0) return suspect_after_ms;
    const uint64_t derived = tick_period_ms * 15;
    return derived < 60 ? 60 : derived;
  }
  [[nodiscard]] uint64_t lost_ms() const noexcept {
    if (lost_after_ms != 0) return lost_after_ms;
    const uint64_t derived = tick_period_ms * 55;
    return derived < 220 ? 220 : derived;
  }
};

inline lab_schedule lab_continuous_schedule() {
  return {};
}

inline lab_schedule lab_killed_schedule() {
  lab_schedule value;
  // Short on purpose: the authority reaches this tick and WAITS for the
  // scheduled rejoin, so the extension below always has to act. A longer run
  // would let the rejoin land early enough that the tail was already satisfied,
  // and the extension would only sometimes be exercised.
  value.final_tick = 30;
  value.explicit_close_tick = 0;
  value.quiet_from_tick = 0;
  value.self_exit_tick = 20;
  value.self_exit_follower = 0;
  value.stale_batch_tick = 0;
  // This scenario ends at tick 30, long before the injections below would fire.
  // A schedule naming a tick the run never reaches is the same class of bug as
  // one naming a roster position the run never launched.
  value.membership_delay_tick = 0;
  value.stale_generation_tick = 0;
  value.resume_tail_ticks = 12;
  return value;
}

inline constexpr uint64_t lab_max_intent_lead = 16;
// How long a process keeps pumping after its last tick, so accepted messages
// leave the transport instead of dying with the process.
inline constexpr uint64_t lab_linger_ms = 400;
// How long a refused connection is kept open after the refusal is handed to
// the transport. Closing immediately discards it, and a refusal the peer never
// receives is one it cannot act on: SESSION-02 makes a refusal TERMINAL
// precisely so a client stops instead of spending its reconnect deadline on
// retries, and that only works if the client is told.
inline constexpr uint64_t lab_refusal_grace_ms = 250;

// How many ticks of margin each arriving intent copy had: the target tick minus
// the tick the authority had already committed. A positive margin means the
// copy beat the seal; zero or negative means it was late. This is the number
// which sizes the proposal lead, and without it "orders were lost" cannot be
// told apart from "orders arrived too late", which the first real network run
// showed are different failures with the same symptom.
inline constexpr size_t lab_margin_buckets = 9;
inline constexpr int lab_margin_lowest = -3; // bucket 0 is "-3 or worse"

inline size_t lab_margin_bucket(const int64_t margin) noexcept {
  if (margin <= lab_margin_lowest) return 0;
  const int64_t offset = margin - lab_margin_lowest;
  return offset >= int64_t(lab_margin_buckets) ? lab_margin_buckets - 1 : size_t(offset);
}

struct authority_counters {
  uint64_t bundles_sent = 0;
  std::array<uint64_t, lab_margin_buckets> margin{};
  uint64_t joins_refused_no_capacity = 0;
  uint64_t multi_principal_ticks = 0;
  uint64_t intents_accepted = 0;
  uint64_t intents_duplicate = 0;
  uint64_t intents_late = 0;
  uint64_t intents_refused = 0;
  uint64_t admissions = 0;
  uint64_t resumes_from_hold = 0;
  uint64_t resumes_by_migration = 0;
  uint64_t recoveries_planned = 0;
  uint64_t chunks_sent = 0;
  uint64_t replay_bundles_sent = 0;
  uint64_t refusals = 0;
  // HOT-02 on the wire. Bytes are counted as PAYLOAD handed to the transport:
  // the per-packet overhead is the transport's and the link's own statistics
  // report it, so adding a guess here would double-count it.
  uint64_t membership_updates = 0;
  uint64_t membership_full_sets = 0;
  uint64_t membership_bytes = 0;
  uint64_t membership_changes = 0;
  uint64_t relevance_enters = 0;
  uint64_t relevance_leaves = 0;
  uint64_t relevance_exhausted = 0;
  uint64_t frames_sent = 0;
  uint64_t frame_bytes = 0;
  uint64_t frame_records = 0;
  // Consecutive runs of occupied slots a full pass had to be split into. One
  // run means the set is still dense; many means churn has holed it, and the
  // dense mode's whole saving is the index it can then no longer omit.
  uint64_t dense_runs = 0;
  uint64_t dense_run_records = 0;
  uint64_t frames_over_budget = 0;
  // Sampled where it matters — at publication — so the mean is over
  // publications and not over ticks that published nothing.
  uint64_t relevance_size_sum = 0;
  uint64_t relevance_publications = 0;
};

class authority_run {
public:
  // One session per follower. Everything a session owns beyond its identity is
  // per-peer, including its recovery transfer: one follower's recovery must not
  // stall another follower's bundles.
  struct session_slot {
    uint64_t session = 0;
    uint64_t principal = 0;
    size_t roster = 0;
    net::gns_peer peer;
    std::vector<std::byte> transfer_bytes;
    // Who this client is allowed to know about. Per session, because that is
    // what relevance means: the slot indices in its frames are meaningless to
    // anyone else.
    net::relevant_set relevance{lab_relevant_capacity};
    size_t transfer_offset = 0;
    bool used = false;
    bool peer_live = false;
    bool quiet = false;
    bool transfer_active = false;
    // A peer which came back is a peer whose mirror may be empty, so the next
    // membership message it gets has to be the whole set rather than a diff.
    bool owes_full_set = false;
    // The scheduled one-tick delay, held here rather than dropped: dropping a
    // reliable ordered message would break the generation chain, which is a
    // different fault from the race being staged.
    std::vector<std::byte> delayed_membership;
    bool membership_delayed = false;
    bool stale_generation_sent = false;
  };

  explicit authority_run(const lab_schedule& schedule)
    : schedule_(schedule),
      history_(96, 96 * 1024),
      checkpoints_(4, 256 * 1024) {
    final_tick_ = schedule_.final_tick;
    std::random_device source;
    nonce_seed_ = (uint64_t(source()) << 32) | uint64_t(source());
    scratch_.reserve(lab_max_message_bytes);
    bundle_scratch_.reserve(lab_max_message_bytes);
    decoded_intents_.reserve(lab_max_intents_per_tick * (lab_intent_window + 1));
    pending_.reserve(lab_max_intents_per_tick * lab_max_intent_lead * lab_max_followers);
    membership_scratch_.reserve(lab_entity_count + lab_relevant_capacity);
    samples_.reserve(lab_relevant_capacity);
    ordered_slots_.reserve(lab_relevant_capacity);
    frame_scratch_.reserve(lab_max_message_bytes);
    journal_.recycle(std::vector<lab_intent_record>(lab_max_intents_per_tick * lab_max_followers));
  }

  lab_link::listen_outcome listen(const uint32_t host_address, const uint16_t port) {
    return port == 0 ? link_.listen_any(host_address) : link_.listen_on(host_address, port);
  }

  // Real conditions per live session, measured from the backend rather than
  // assumed from the fact that this is loopback.
  uint64_t superseded() const noexcept {
    return link_.superseded();
  }

  lab_link::conditions measure(const size_t roster) {
    if (auto* session = session_by_roster(roster); session != nullptr && session->peer_live)
      return link_.measure(session->peer);
    return {};
  }

  verifier& checks() noexcept {
    return verify_;
  }
  const authority_counters& counters() const noexcept {
    return counters_;
  }
  const lab_host& host() const noexcept {
    return host_;
  }
  uint64_t final_tick() const noexcept {
    return final_tick_;
  }
  bool started() const noexcept {
    return started_;
  }
  size_t admitted() const noexcept {
    return admitted_count_;
  }

  // One pass of the authority's owner loop. Returns false once the scheduled
  // final tick is committed.
  bool step() {
    const uint64_t now = monotonic_ms();
    poll_transport(now);
    drain_messages(now);
    // The clock starts once EVERY expected follower is admitted. A follower
    // which joins fresh has no state at all, so a session which had already
    // advanced would owe it a baseline transfer -- a real requirement, and the
    // next slice's. With one follower this was "the first admission"; with
    // three it is the last, and that difference is the whole reason a
    // mid-session fresh join needs its own answer.
    if (!started_) return true;
    pump_transfers();
    if (host_.state.tick >= final_tick_) {
      if (transfers_active()) return true;
      if (counters_.admissions < schedule_.admissions_expected()) return true;
      // The last bundles are accepted by the transport, not yet delivered.
      // Exiting here would make the follower's final tick depend on scheduling.
      if (linger_until_ == 0) linger_until_ = now + lab_linger_ms;
      return now < linger_until_;
    }
    if (now >= next_tick_at_) {
      advance_tick(now);
      next_tick_at_ = now + schedule_.tick_period_ms;
    }
    return true;
  }

private:
  // -------------------------------------------------------------- connections

  struct handshake_slot {
    net::gns_peer peer;
    // Nonzero once refused: the connection is held open until this instant so
    // the refusal can actually leave.
    uint64_t close_after = 0;
    bool used = false;
    std::optional<net::authority_handshake> shake;
  };

  void reap_refused(const uint64_t now) {
    for (auto& value : handshakes_) {
      if (!value.used || value.close_after == 0 || now < value.close_after) continue;
      (void)link_.close(value.peer);
      value.used = false;
      value.close_after = 0;
      value.shake.reset();
    }
  }

  void poll_transport(const uint64_t now) {
    reap_refused(now);
    std::array<lab_link::observation, 4> events;
    const size_t count = link_.poll(events);
    for (size_t i = 0; i < count; ++i) {
      const auto& event = events[i];
      if (event.needs_accept) {
        // Admission is explicit. A connection is accepted at the transport so
        // a handshake can run on it; it is not a session and not an identity.
        if (link_.accept(event.peer) != net::gns_status::ok) continue;
        open_handshake(event.peer);
        continue;
      }
      if (event.terminal) on_peer_lost(event.peer, now);
    }
  }

  void open_handshake(const net::gns_peer peer) {
    handshake_slot* slot = nullptr;
    for (auto& value : handshakes_) {
      if (!value.used && value.close_after == 0) {
        slot = &value;
        break;
      }
    }
    if (slot == nullptr) {
      (void)link_.close(peer);
      ++counters_.refusals;
      return;
    }
    net::session_nonce nonce{};
    uint64_t word = splitmix64(nonce_seed_ ^ ++nonce_counter_);
    for (size_t i = 0; i < nonce.size(); ++i) {
      if (i % 8 == 0) word = splitmix64(word);
      nonce[i] = std::byte(uint8_t(word >> (8 * (i % 8))));
    }
    slot->peer = peer;
    slot->used = true;
    slot->close_after = 0;
    slot->shake.emplace(local_, nonce);
  }

  void close_handshake(const net::gns_peer peer) {
    for (auto& value : handshakes_) {
      if (value.used && value.peer == peer) {
        value.used = false;
        value.shake.reset();
      }
    }
  }

  void on_peer_lost(const net::gns_peer peer, const uint64_t now) {
    close_handshake(peer);
    (void)link_.close(peer);
    if (auto* session = session_for(peer); session != nullptr) {
      session->peer_live = false;
      session->transfer_active = false;
      hold_session(*session, now);
    }
  }

  session_slot* session_for(const net::gns_peer peer) {
    for (auto& value : sessions_) {
      if (value.used && value.peer_live && value.peer == peer) return &value;
    }
    return nullptr;
  }

  session_slot* session_by_id(const uint64_t session) {
    for (auto& value : sessions_) {
      if (value.used && value.session == session) return &value;
    }
    return nullptr;
  }

  // A session whose peer is gone is retained for a declared window so its owner
  // can come back. Capacity is declared, so this cannot become an allocation a
  // peer controls -- and with several followers that capacity is now a real
  // number rather than a formality.
  void hold_session(const session_slot& session, const uint64_t now) {
    const auto status = holds_.hold(session.session, session.principal, epoch_, now,
                                    now + schedule_.hold_window_ms);
    verify_.require(status == net::session_hold_table::hold_status::held ||
                      status == net::session_hold_table::hold_status::already_held,
                    "authority could not retain a session whose peer disappeared");
  }

  // ---------------------------------------------------------------- messages

  void drain_messages(const uint64_t now) {
    link_.receive([&](const net::gns_peer peer, const uint16_t lane,
                      const std::span<const std::byte> bytes) {
      if (bytes.empty()) {
        ++counters_.refusals;
        return;
      }
      if (lane == lab_lane_intent) {
        on_intent_batch(peer, bytes);
        return;
      }
      // Everything else on the control lane is either the handshake or noise.
      if (handshake_for(peer) != nullptr) {
        on_handshake_message(peer, bytes, now);
        return;
      }
      ++counters_.refusals;
    });
  }

  handshake_slot* handshake_for(const net::gns_peer peer) {
    for (auto& value : handshakes_) {
      if (value.used && value.shake.has_value() && value.peer == peer) return &value;
    }
    return nullptr;
  }

  void on_handshake_message(const net::gns_peer peer, const std::span<const std::byte> bytes,
                            const uint64_t now) {
    auto* slot = handshake_for(peer);
    admission_now_ = now;
    admission_peer_ = peer;
    admitted_ = nullptr;
    resumed_ = false;
    reply_.clear();
    reply_.reserve(net::session_wire_max_message_bytes);
    const auto status = slot->shake->consume(bytes, reply_, *this);
    if (!reply_.empty()) (void)link_.send(peer, lab_lane_control, reply_);

    if (slot->shake->phase() == net::handshake_phase::refused ||
        status != net::session_wire_status::ok) {
      ++counters_.refusals;
      // The refusal was handed to the transport a moment ago. Closing now
      // would throw it away and the peer would learn only that the connection
      // died, which is exactly the retry a terminal refusal exists to prevent.
      slot->close_after = now + lab_refusal_grace_ms;
      slot->shake.reset();
      return;
    }
    if (!slot->shake->established()) return;

    // Established. From here the connection carries the session, and the
    // session's identity is the authority's record rather than the claim.
    verify_.require(admitted_ != nullptr, "an established handshake named no session");
    attach_peer(*admitted_, peer, now);
    close_handshake(peer);
    linger_until_ = 0;
    send_grant(*admitted_, now);
    if (resumed_) send_recovery(*admitted_, now);
  }

  void attach_peer(session_slot& session, const net::gns_peer peer, const uint64_t now) {
    if (session.peer_live && !(session.peer == peer)) {
      // A resume arrived while the previous connection was still nominally
      // alive. Refusing it because the session is "not held" would strand a
      // client whose path died in one direction only, so the session migrates
      // and the stale connection is closed.
      (void)link_.close(session.peer);
      ++counters_.resumes_by_migration;
    }
    session.peer = peer;
    session.peer_live = true;
    session.quiet = false;
    session.transfer_active = false;
    // Any newly attached connection gets the whole set once. For a fresh join
    // the diff and the full set are the same thing; for a rejoin they are not,
    // and the authority cannot tell whether the peer across a new connection
    // still holds the mirror it had.
    session.owes_full_set = true;
    holds_.release(session.session);
    if (!started_ && admitted_count_ >= schedule_.followers) {
      started_ = true;
      next_tick_at_ = now;
    }
    // The run is extended once the last expected reconnect lands, and the new
    // end is ANNOUNCED: a follower which kept its first grant's value would
    // stop while the session is still running.
    if (schedule_.resume_tail_ticks != 0 && started_ &&
        counters_.admissions >= schedule_.admissions_expected()) {
      // The extension is a FIXED end, not "wherever we are plus a tail". The
      // authority cannot pass its announced final tick before the rejoin lands,
      // so a fixed end still guarantees at least the declared tail -- and it
      // makes the run's length, and therefore its state root, the same in every
      // run instead of a function of how fast a process happened to start.
      const uint64_t extended = schedule_.final_tick + schedule_.resume_tail_ticks;
      if (extended > final_tick_) {
        final_tick_ = extended;
        for (auto& other : sessions_) {
          if (other.used && other.peer_live && !(other.peer == peer)) send_grant(other, now);
        }
      }
    }
  }

  void send_grant(const session_slot& session, const uint64_t now) {
    net::reconnect_ticket ticket;
    ticket.session = session.session;
    ticket.principal = session.principal;
    ticket.authority_epoch = epoch_;
    ticket.issued_at = now;
    ticket.expires_at = now + schedule_.ticket_window_ms;
    ticket.issuer = lab_issuer;

    lab_grant grant;
    grant.issued_at = now;
    grant.final_tick = final_tick_;
    grant.tick_period_ms = schedule_.tick_period_ms;
    grant.suspect_after_ms = schedule_.suspect_ms();
    grant.lost_after_ms = schedule_.lost_ms();
    grant.intent_lead_ticks = schedule_.intent_lead_ticks;
    verify_.require(net::issue_reconnect_ticket(ticket, mac_, credential_scratch_,
                                                grant.credential) ==
                      net::credential_status::accepted,
                    "authority could not mint its own reconnect ticket");
    verify_.require(net::derive_session_secret(lab_issuer, session.session, session.principal,
                                               mac_, credential_scratch_, grant.session_secret) ==
                      net::credential_status::accepted,
                    "authority could not derive the session secret");
    scratch_.reserve(lab_max_message_bytes);
    verify_.require(encode_grant(grant, scratch_), "grant did not encode");
    verify_.require(link_.send(session.peer, lab_lane_control, scratch_) ==
                      lab_send_result::sent,
                    "grant could not be sent on the control lane");
  }

  // ---------------------------------------------------------------- recovery

  void send_recovery(session_slot& session, const uint64_t now) {
    const uint64_t target = host_.state.tick;
    const auto* entry = checkpoints_.latest_at_or_before(target);
    std::optional<uint64_t> checkpoint_tick;
    if (entry != nullptr) checkpoint_tick = entry->tick;

    net::recovery_range<uint64_t> range;
    const auto feasibility =
      net::assess_recovery<uint64_t>(checkpoint_tick, history_.oldest_tick(), target, range);
    if (feasibility != net::recovery_feasibility::recoverable) {
      scratch_.reserve(lab_max_message_bytes);
      verify_.require(encode_recovery_unavailable(uint8_t(feasibility), scratch_),
                      "unavailability did not encode");
      (void)link_.send(session.peer, lab_lane_control, scratch_);
      return;
    }

    const auto& blob = *checkpoints_.find(range.checkpoint_tick);
    lab_recovery_plan plan;
    plan.checkpoint_tick = range.checkpoint_tick;
    plan.target_tick = range.target_tick;
    plan.checkpoint_bytes = uint32_t(blob.size());
    net::state_digest_report<uint64_t> report;
    report.sections.reserve(lab_schema::section_count);
    verify_.require(net::try_murmur64_digest<lab_schema>(blob, report) ==
                      net::state_digest_build_status::built,
                    "retained checkpoint bytes are not a valid canonical document");
    plan.checkpoint_root = report.root;
    plan.target_root = lab_root(host_);

    scratch_.reserve(lab_max_message_bytes);
    verify_.require(encode_recovery_plan(plan, scratch_), "recovery plan did not encode");
    verify_.require(link_.send(session.peer, lab_lane_control, scratch_) ==
                      lab_send_result::sent,
                    "recovery plan could not be sent");
    ++counters_.recoveries_planned;

    // The transfer owns its bytes and belongs to THIS session: the ring is free
    // to evict that checkpoint while the transfer is in flight, and one
    // follower's recovery must not touch another's.
    session.transfer_bytes = blob;
    session.transfer_offset = 0;
    session.transfer_active = true;

    // The replay range goes out on the control lane immediately. It will
    // OVERTAKE the paced bulk transfer, which is the whole reason the follower
    // has to be able to hold a bundle back until the checkpoint has landed.
    for (uint64_t tick = range.checkpoint_tick + 1; tick <= range.target_tick; ++tick) {
      const auto* bundle = history_.find(tick);
      verify_.require(bundle != nullptr, "a bundle the feasibility check promised is missing");
      send_bundle_to(session, *bundle);
      ++counters_.replay_bundles_sent;
    }
    (void)now;
  }

  bool transfers_active() const noexcept {
    for (const auto& session : sessions_) {
      if (session.used && session.transfer_active) return true;
    }
    return false;
  }

  void pump_transfers() {
    for (auto& session : sessions_) {
      if (!session.used || !session.transfer_active || !session.peer_live) continue;
      for (size_t sent = 0; sent < lab_chunks_per_pass && session.transfer_active; ++sent) {
        const auto length = std::min(lab_checkpoint_chunk_bytes,
                                     session.transfer_bytes.size() - session.transfer_offset);
        verify_.require(
          encode_chunk(uint32_t(session.transfer_offset),
                       std::span<const std::byte>(session.transfer_bytes)
                         .subspan(session.transfer_offset, length),
                       scratch_),
          "checkpoint chunk did not encode");
        const auto result = link_.send(session.peer, lab_lane_bulk, scratch_);
        // Bulk backpressure is a status, not a fault: the transfer waits.
        if (result != lab_send_result::sent) break;
        ++counters_.chunks_sent;
        session.transfer_offset += length;
        if (session.transfer_offset == session.transfer_bytes.size())
          session.transfer_active = false;
      }
    }
  }

  // ----------------------------------------------------------------- intents

  void on_intent_batch(const net::gns_peer peer, const std::span<const std::byte> bytes) {
    // The PRINCIPAL comes from the connection's session, never from the batch.
    // This is the same rule as the implied actor: a peer which could name its
    // principal could name someone else's.
    auto* session = session_for(peer);
    if (session == nullptr) {
      ++counters_.intents_refused;
      return;
    }
    net::intent_batch_view view;
    if (net::try_decode_intent_batch(bytes, layouts_, registry_, view, decoded_intents_) !=
        net::intent_wire_status::ok) {
      ++counters_.intents_refused;
      return;
    }
    for (const auto& value : decoded_intents_) {
      const auto absolute =
        net::intent_absolute_tick(host_.state.tick, view.base_tick_low, value.tick_delta);
      if (!absolute) {
        ++counters_.intents_refused;
        continue;
      }
      admit_intent(*absolute, session->principal, value);
    }
  }

  // The untrusted ingress: ownership is already established by the connection,
  // so what is left is range, rate and provenance. A redundant copy is dropped
  // HERE rather than in the journal, whose duplicate rejection is a fault -- and
  // "redundant" now means same principal, same kind, same tick. The same kind
  // from a different principal is a different order.
  void admit_intent(const uint64_t tick, const uint64_t principal, const net::intent& value) {
    // Recorded for every arriving copy, accepted or late, because the useful
    // question is the SHAPE of the margin distribution and not just its sign.
    ++counters_.margin[lab_margin_bucket(int64_t(tick) - int64_t(host_.state.tick))];
    if (tick <= host_.state.tick) {
      ++counters_.intents_late;
      return;
    }
    if (tick > host_.state.tick + lab_max_intent_lead) {
      ++counters_.intents_refused;
      return;
    }
    for (const auto& held : pending_) {
      if (held.tick == tick && held.principal == principal &&
          held.value.kind == value.kind) {
        ++counters_.intents_duplicate;
        return;
      }
    }
    if (pending_.size() == pending_.capacity()) {
      ++counters_.intents_refused;
      return;
    }
    pending_.push_back({tick, principal, value});
    ++counters_.intents_accepted;
  }

  // ------------------------------------------------------------------- ticks

  void advance_tick(const uint64_t now) {
    const uint64_t tick = host_.state.tick + 1;
    const size_t capacity = lab_max_intents_per_tick * lab_max_followers;
    const auto tag = journal_.begin(tick, capacity);
    for (const auto& record : pending_) {
      if (record.tick != tick) continue;
      const auto result = journal_.try_record(record);
      verify_.require(result == net::tick_record_result::recorded,
                      "ingress admitted more intents for one tick than the journal declared");
    }
    verify_.require(journal_.seal() == net::tick_seal_result::sealed,
                    "canonical seal refused a set the ingress had already validated");
    auto batch = journal_.consume(tag);

    lab_bundle bundle;
    bundle.tick = tick;
    bundle.intents.reserve(capacity);
    uint64_t previous_principal = 0;
    bool first = true, several = false;
    for (const auto& record : batch.records()) {
      // The seal promised a total order on the principal. Checking it here is
      // what turns that promise into a measured property of the run.
      if (!first) {
        verify_.require(previous_principal <= record.principal,
                        "a sealed bundle was not ordered by principal");
        if (previous_principal != record.principal) several = true;
      }
      previous_principal = record.principal;
      first = false;
      bundle.intents.push_back(record.value);
    }
    if (several) ++counters_.multi_principal_ticks;
    // Ownership goes back to the journal explicitly, so the next tick allocates
    // nothing at all.
    journal_.retire(tag);
    journal_.recycle(std::move(batch).release_storage());
    std::erase_if(pending_, [tick](const lab_intent_record& record) {
      return record.tick <= tick;
    });

    apply_bundle(host_, bundle);
    lab_step(host_, tick);

    // The byte budget is the logical cost the project supplies, never a guess
    // from allocator capacity.
    verify_.require(history_.try_store(tick, bundle, lab_bundle_size{}(bundle)).stored(),
                    "bundle history refused a bundle it must retain");
    if (tick % schedule_.checkpoint_every == 0) {
      auto bytes = lab_schema::write(host_);
      verify_.require(!bytes.empty(), "checkpoint serialization produced nothing");
      verify_.require(checkpoints_.try_store(tick, std::move(bytes)).stored(),
                      "checkpoint ring refused a checkpoint it must retain");
    }

    apply_schedule(tick, now);
    broadcast_bundle(bundle);
    replicate_transforms(tick);
  }

  // ------------------------------------------------- relevant set and frames

  void replicate_transforms(const uint64_t tick) {
    for (auto& session : sessions_) {
      if (!session.used || !session.peer_live || session.quiet) continue;
      // Membership first, and on the reliable lane: a frame refers to slots by
      // the generation this publication produces. The frame may still overtake
      // it — that is a different lane — which is exactly what the follower's
      // mismatch counters measure.
      // A delayed publication goes out first thing on the following tick, so
      // the frames that raced it have already left.
      if (!session.delayed_membership.empty()) {
        (void)link_.send(session.peer, lab_lane_control, session.delayed_membership);
        session.delayed_membership.clear();
      }
      if (tick % lab_relevance_every_ticks == 0 || session.owes_full_set)
        publish_relevance(session, tick);
      send_frames(session, tick);
      if (schedule_.sends_stale_generation() && tick == schedule_.stale_generation_tick &&
          session.roster == schedule_.stale_generation_follower &&
          !session.stale_generation_sent)
        send_stale_generation_frame(session, tick);
    }
  }

  void publish_relevance(session_slot& session, const uint64_t tick) {
    const double center = lab_view_center(host_.state);
    // Hysteresis: an entity enters nearer than it has to leave. Without it the
    // set dithers at the boundary and every dither is a membership message and
    // a pop on the client.
    for (size_t index = 0; index < lab_entity_count; ++index) {
      const uint64_t handle = lab_entity_handle(index);
      const double distance = lab_entity_distance(index, tick, center);
      const bool present = session.relevance.slot_of(handle).has_value();
      if (!present && distance <= lab_relevance_enter_units) {
        const auto result = session.relevance.enter(handle);
        if (result.status == net::slot_admission::assigned) ++counters_.relevance_enters;
        else if (result.status == net::slot_admission::capacity_exhausted)
          ++counters_.relevance_exhausted;
        continue;
      }
      if (present && distance > lab_relevance_leave_units) {
        if (session.relevance.leave(handle)) ++counters_.relevance_leaves;
      }
    }

    // The delay below can only race something if there IS a publication at that
    // tick, and a short run's set may not have changed at all. So the schedule
    // forces one: the occupant of the highest slot leaves, which the next
    // publication will put back on its own.
    if (schedule_.delays_membership() && tick == schedule_.membership_delay_tick &&
        session.roster == schedule_.membership_delay_follower && !session.membership_delayed) {
      for (uint16_t slot = session.relevance.capacity(); slot-- > 0;) {
        const uint64_t handle = session.relevance.handle_at(slot);
        if (handle == 0) continue;
        if (session.relevance.leave(handle)) ++counters_.relevance_leaves;
        break;
      }
    }

    membership_scratch_.clear();
    uint16_t generation = session.relevance.generation();
    auto scope = net::relevant_set_scope::diff;
    if (session.owes_full_set) {
      // The generation still advances through publish(), so the full set is the
      // CURRENT set at the current generation rather than a message with a
      // generation of its own.
      generation = session.relevance.publish(membership_scratch_);
      session.relevance.publish_full(membership_scratch_);
      scope = net::relevant_set_scope::full;
      session.owes_full_set = false;
    } else {
      generation = session.relevance.publish(membership_scratch_);
      if (membership_scratch_.empty()) return;
    }

    scratch_.reserve(net::relevant_set_update_max_bytes);
    const auto status =
      net::try_encode_relevant_set_update(generation, membership_scratch_, scope, scratch_);
    verify_.require(status == net::transform_wire_status::ok,
                    "the relevant set did not encode");
    if (schedule_.delays_membership() && tick == schedule_.membership_delay_tick &&
        session.roster == schedule_.membership_delay_follower && !session.membership_delayed) {
      session.membership_delayed = true;
      session.delayed_membership.assign(scratch_.begin(), scratch_.end());
    } else {
      verify_.require(link_.send(session.peer, lab_lane_control, scratch_) ==
                        lab_send_result::sent,
                      "membership could not be sent on the reliable lane");
    }
    if (scope == net::relevant_set_scope::full) ++counters_.membership_full_sets;
    else ++counters_.membership_updates;
    counters_.membership_bytes += scratch_.size();
    counters_.membership_changes += membership_scratch_.size();
    counters_.relevance_size_sum += session.relevance.size();
    ++counters_.relevance_publications;
  }

  // Occupied slots, nearest first. `near` takes the head of that order and
  // `far` the tail, so the two classes partition the set instead of overlapping.
  void collect_relevant(const session_slot& session, const uint64_t tick) {
    const double center = lab_view_center(host_.state);
    ordered_slots_.clear();
    for (uint16_t slot = 0; slot < session.relevance.capacity(); ++slot) {
      const uint64_t handle = session.relevance.handle_at(slot);
      if (handle == 0) continue;
      const auto index = lab_entity_index(handle);
      verify_.require(index.has_value(), "a relevant set held a handle no entity owns");
      ordered_slots_.push_back({lab_entity_distance(*index, tick, center), *index, slot});
    }
    std::sort(ordered_slots_.begin(), ordered_slots_.end(),
              [](const ranked_slot& l, const ranked_slot& r) {
                if (l.distance != r.distance) return l.distance < r.distance;
                return l.slot < r.slot;
              });
  }

  void send_frames(session_slot& session, const uint64_t tick) {
    std::array<uint8_t, 4> due{};
    const size_t count = transforms_.due_classes(tick, due);
    if (count == 0) return;
    collect_relevant(session, tick);
    if (ordered_slots_.empty()) return;

    for (size_t i = 0; i < count; ++i) {
      const uint8_t class_id = due[i];
      if (class_id == lab_class_refresh) {
        send_dense_runs(session, tick, class_id);
        continue;
      }
      const size_t head = class_id == lab_class_near
                            ? 0
                            : (ordered_slots_.size() < lab_near_slots ? ordered_slots_.size()
                                                                      : lab_near_slots);
      const size_t tail = class_id == lab_class_near
                            ? (ordered_slots_.size() < lab_near_slots ? ordered_slots_.size()
                                                                      : lab_near_slots)
                            : ordered_slots_.size();
      if (head >= tail) continue;

      // The budget decides how much of the class travels this opportunity, and
      // the class is ordered by distance, so what gets cut is the far end.
      const auto plan = net::plan_transform_send(lab_transform_fields,
                                                 net::transform_frame_mode::sparse, tail - head,
                                                 transforms_.cadence(class_id));
      if (!plan.complete) ++counters_.frames_over_budget;
      if (plan.records == 0) continue;

      samples_.clear();
      for (size_t k = head; k < head + plan.records; ++k)
        samples_.push_back(lab_entity_sample(ordered_slots_[k].index, tick,
                                             ordered_slots_[k].slot));
      // Sparse frames carry their slots and must ascend; distance order does
      // not, so the frame is sorted by slot after the budget chose the set.
      std::sort(samples_.begin(), samples_.end(),
                [](const net::transform_sample& l, const net::transform_sample& r) {
                  return l.slot < r.slot;
                });
      send_frame(session, class_id, tick, net::transform_frame_mode::sparse, samples_);
    }
  }

  // A full pass in dense mode, which carries no slot indices at all — and can
  // therefore only cover CONSECUTIVE slots. Counting the runs measures how
  // holed churn has left the set, which is the same thing as measuring what the
  // dense mode is still worth.
  void send_dense_runs(session_slot& session, const uint64_t tick, const uint8_t class_id) {
    const uint16_t capacity = session.relevance.capacity();
    uint16_t slot = 0;
    while (slot < capacity) {
      if (session.relevance.handle_at(slot) == 0) {
        ++slot;
        continue;
      }
      samples_.clear();
      while (slot < capacity && session.relevance.handle_at(slot) != 0 &&
             samples_.size() < size_t(lab_relevant_capacity)) {
        const auto index = lab_entity_index(session.relevance.handle_at(slot));
        verify_.require(index.has_value(), "a relevant set held a handle no entity owns");
        samples_.push_back(lab_entity_sample(*index, tick, slot));
        ++slot;
      }
      ++counters_.dense_runs;
      counters_.dense_run_records += samples_.size();
      send_frame(session, class_id, tick, net::transform_frame_mode::dense, samples_);
    }
  }

  // One frame built against the generation before the current one, after the
  // current membership has landed. A client must drop it: its slots mean what
  // they meant under a set which has already been replaced.
  void send_stale_generation_frame(session_slot& session, const uint64_t tick) {
    collect_relevant(session, tick);
    if (ordered_slots_.empty() || session.relevance.generation() == 0) return;
    samples_.clear();
    samples_.push_back(lab_entity_sample(ordered_slots_.front().index, tick,
                                         ordered_slots_.front().slot));
    frame_scratch_.reserve(lab_max_message_bytes);
    const auto status = net::try_encode_transform_frame(
      lab_class_near, tick, uint16_t(session.relevance.generation() - 1),
      net::transform_frame_mode::sparse, lab_frame_origin, samples_, transforms_,
      frame_scratch_);
    verify_.require(status == net::transform_wire_status::ok,
                    "the deliberately stale frame did not encode");
    if (link_.send(session.peer, lab_lane_transform, frame_scratch_) == lab_send_result::sent)
      session.stale_generation_sent = true;
  }

  void send_frame(const session_slot& session, const uint8_t class_id, const uint64_t tick,
                  const net::transform_frame_mode mode,
                  const std::span<const net::transform_sample> samples) {
    frame_scratch_.reserve(lab_max_message_bytes);
    const auto status = net::try_encode_transform_frame(
      class_id, tick, session.relevance.generation(), mode, lab_frame_origin, samples,
      transforms_, frame_scratch_);
    verify_.require(status == net::transform_wire_status::ok, "a transform frame did not encode");
    const auto result = link_.send(session.peer, lab_lane_transform, frame_scratch_);
    if (result != lab_send_result::sent) {
      // Latest-value traffic may be dropped by its own lane's budget; that is
      // the class working as declared, not a fault.
      verify_.require(result == lab_send_result::backpressure || result == lab_send_result::no_peer,
                      "the transform lane refused a frame for a live peer");
      return;
    }
    ++counters_.frames_sent;
    counters_.frame_bytes += frame_scratch_.size();
    counters_.frame_records += samples.size();
  }

  void broadcast_bundle(const lab_bundle& bundle) {
    for (auto& session : sessions_) {
      if (!session.used || !session.peer_live || session.quiet) continue;
      send_bundle_to(session, bundle);
    }
  }

  void send_bundle_to(const session_slot& session, const lab_bundle& bundle) {
    bundle_scratch_.reserve(lab_max_message_bytes);
    verify_.require(encode_bundle(bundle, bundle_scratch_), "bundle did not encode");
    const auto result = link_.send(session.peer, lab_lane_control, bundle_scratch_);
    if (result == lab_send_result::sent) {
      ++counters_.bundles_sent;
      return;
    }
    // Canonical bundles cannot be silently dropped: overflow is a session
    // fault, not a lost frame.
    verify_.require(result == lab_send_result::no_peer,
                    "control lane refused a canonical bundle while the peer was live");
  }

  void apply_schedule(const uint64_t tick, const uint64_t now) {
    if (schedule_.explicit_close_tick != 0 && tick == schedule_.explicit_close_tick) {
      if (auto* session = session_by_roster(schedule_.explicit_close_follower);
          session != nullptr && session->peer_live) {
        (void)link_.close(session->peer);
        session->peer_live = false;
        session->transfer_active = false;
        hold_session(*session, now);
      }
    }
    if (schedule_.quiet_from_tick != 0 && tick == schedule_.quiet_from_tick) {
      if (auto* session = session_by_roster(schedule_.quiet_follower);
          session != nullptr && session->peer_live) {
        // No close, no notification: the connection stays nominally up and
        // simply carries nothing to THIS follower while the others keep going.
        session->quiet = true;
        hold_session(*session, now);
      }
    }
  }

  session_slot* session_by_roster(const size_t roster) {
    for (auto& value : sessions_) {
      if (value.used && value.roster == roster) return &value;
    }
    return nullptr;
  }

public:
  // ------------------------------------------------- authority_handshake_policy

  bool issue_challenge(const net::client_hello& hello, std::vector<std::byte>& out) {
    // The library refuses to guess what a challenge contains. This one binds
    // the client's own nonce under the authority key, so it cannot be produced
    // by anyone else and cannot be replayed into a different hello.
    net::credential_mac tag{};
    challenge_message_.clear();
    challenge_message_.reserve(hello.client_nonce.size());
    challenge_message_.assign(hello.client_nonce.begin(), hello.client_nonce.end());
    if (!mac_.authority_mac(lab_issuer, challenge_message_, tag)) return false;
    out.assign(tag.begin(), tag.end());
    return true;
  }

  net::session_refusal_reason admit(const net::client_response& response,
                                    const utils::digest& transcript,
                                    net::session_accepted& accepted) {
    if (response.resumed_session) return admit_resume(response, transcript, accepted);
    return admit_join(response, accepted);
  }

private:
  net::session_refusal_reason admit_join(const net::client_response& response,
                                         net::session_accepted& accepted) {
    // The join credential is an injected policy, and a declared roster of
    // laboratory tokens is a laboratory policy. What matters is where identity
    // comes from: the credential names the roster position, and the authority
    // derives the principal. A client cannot name its own.
    const auto roster = lab_roster_index(response.credential);
    if (!roster) return net::session_refusal_reason::identity_rejected;
    if (auto* existing = session_by_roster(*roster); existing != nullptr) {
      // That identity already holds a session, live or merely retained.
      // Minting a second one would discard a hold its owner is on the way back
      // to claim.
      ++counters_.joins_refused_no_capacity;
      return net::session_refusal_reason::no_capacity;
    }
    session_slot* free_slot = nullptr;
    for (auto& value : sessions_) {
      if (!value.used) {
        free_slot = &value;
        break;
      }
    }
    if (free_slot == nullptr) {
      ++counters_.joins_refused_no_capacity;
      return net::session_refusal_reason::no_capacity;
    }
    free_slot->used = true;
    free_slot->session = ++next_session_;
    free_slot->principal = lab_principal_of(*roster);
    free_slot->roster = *roster;
    ++admitted_count_;
    admitted_ = free_slot;
    fill_accepted(*free_slot, accepted);
    ++counters_.admissions;
    return net::session_refusal_reason::none;
  }

  net::session_refusal_reason admit_resume(const net::client_response& response,
                                           const utils::digest& transcript,
                                           net::session_accepted& accepted) {
    net::reconnect_credential credential;
    if (net::try_decode_credential(response.credential, credential) !=
        net::credential_status::accepted)
      return net::session_refusal_reason::identity_rejected;
    auto* session = session_by_id(*response.resumed_session);
    if (session == nullptr) return net::session_refusal_reason::unknown_session;

    // Session, principal and epoch come from the authority's own record. Taking
    // any of them from the credential would make the credential its own
    // authority.
    const net::reconnect_expectation expectation{session->session, session->principal, epoch_};
    const auto status = net::verify_reconnect_credential(credential, expectation, admission_now_,
                                                         transcript, mac_, credential_scratch_);
    if (status != net::credential_status::accepted)
      return net::session_refusal_reason::identity_rejected;

    // Only now, with the principal proved, is the retention table consulted --
    // and only when the session is not already attached to a live connection.
    if (!session->peer_live) {
      const auto resolved =
        holds_.resolve(session->session, session->principal, epoch_, admission_now_);
      if (resolved != net::session_hold_table::resolve_status::held)
        return net::session_refusal_reason::unknown_session;
      ++counters_.resumes_from_hold;
    }
    resumed_ = true;
    admitted_ = session;
    fill_accepted(*session, accepted);
    ++counters_.admissions;
    return net::session_refusal_reason::none;
  }

  void fill_accepted(const session_slot& session, net::session_accepted& accepted) const {
    accepted.session = session.session;
    accepted.local_peer = session.principal;
    accepted.authority_peer = lab_authority_peer;
    accepted.authority_epoch = epoch_;
    accepted.start_tick = host_.state.tick;
  }

public:
  static void apply_bundle(lab_host& host, const lab_bundle& bundle) {
    // The base cell is the state BEFORE the bundle, which both sides have
    // committed identically, so a relative cell needs nothing on the wire.
    // Several intents in one tick are applied in the sealed canonical order,
    // so "the last one wins" is a rule about the ORDER, not about arrival.
    const int32_t base_key = int32_t(host.state.position >> 16);
    for (const auto& value : bundle.intents)
      host.state.target = lab_intent_quanta(value, base_key);
  }

  static constexpr uint64_t lab_authority_peer = 1;

private:
  lab_schedule schedule_;
  verifier verify_;
  authority_counters counters_;
  gns_runtime runtime_;
  lab_link link_;
  lab_authority_mac mac_;
  net::credential_scratch credential_scratch_;
  net::session_hold_table holds_{lab_max_followers};
  net::session_compatibility local_ = lab_compatibility();
  net::intent_layout_table layouts_ = lab_layouts();
  net::id_index_table registry_ = lab_registry();
  lab_journal journal_;
  net::bounded_history<uint64_t, lab_bundle> history_;
  net::checkpoint_ring<uint64_t, std::vector<std::byte>, lab_blob_size> checkpoints_;
  lab_host host_;
  // One more handshake than followers: a reconnect overlaps the connection it
  // replaces.
  // Two more than the roster: a reconnect overlaps the connection it replaces,
  // and a refused connection occupies its slot for the grace period above.
  std::array<handshake_slot, lab_max_followers + 2> handshakes_;
  std::array<session_slot, lab_max_followers> sessions_;
  std::vector<lab_intent_record> pending_;
  std::vector<net::intent> decoded_intents_;
  std::vector<std::byte> scratch_, bundle_scratch_, reply_, challenge_message_, frame_scratch_;
  net::transform_layout_table transforms_ = lab_transform_layouts();
  std::vector<net::slot_change> membership_scratch_;
  std::vector<net::transform_sample> samples_;
  struct ranked_slot {
    double distance = 0;
    size_t index = 0;
    uint16_t slot = 0;
  };
  std::vector<ranked_slot> ordered_slots_;
  net::gns_peer admission_peer_;
  session_slot* admitted_ = nullptr;
  uint64_t nonce_seed_ = 0, nonce_counter_ = 0;
  uint64_t epoch_ = 1, next_session_ = 0;
  uint64_t admission_now_ = 0, next_tick_at_ = 0, linger_until_ = 0;
  uint64_t final_tick_ = schedule_.final_tick;
  size_t admitted_count_ = 0;
  bool started_ = false, resumed_ = false;
};

static_assert(net::authority_handshake_policy<authority_run>);

} // namespace netlab01

#endif
