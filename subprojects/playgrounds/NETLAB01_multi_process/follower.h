#ifndef DEVILS_ENGINE_NETLAB01_FOLLOWER_H
#define DEVILS_ENGINE_NETLAB01_FOLLOWER_H

#include "authority.h"
#include "link.h"
#include "protocol.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

// The follower process. It proposes intents, consumes the authoritative
// timeline, notices loss two different ways, reconnects with the ticket the
// authority minted, and recovers transactionally: a detached candidate is
// replayed and published once, so a failed recovery leaves the running state
// untouched.

namespace netlab01 {

struct follower_counters {
  uint64_t bundles_applied = 0;
  uint64_t bundles_deferred = 0;
  uint64_t intents_proposed = 0;
  uint64_t batches_sent = 0;
  uint64_t warnings = 0;
  uint64_t transport_losses = 0;
  uint64_t silence_losses = 0;
  uint64_t reconnects = 0;
  uint64_t recoveries = 0;
  uint64_t replayed_ticks = 0;
  uint64_t chunks_received = 0;
  uint64_t stale_batches_sent = 0;
  // The measurement that matters, as opposed to the ones that are easy. A
  // redundant copy which arrives too late costs nothing, and the authority's
  // `late` counter is dominated by exactly those; what a player would notice is
  // an ORDER which never entered a canonical bundle at all.
  uint64_t orders_landed = 0;
  uint64_t orders_lost = 0;
  // Proposals whose tick was crossed by a recovery replay rather than applied
  // live. Their fate is not observed here, and saying so beats folding them
  // into either of the two above.
  uint64_t orders_unobserved = 0;
  // Proposals for ticks the run ended before reaching. Not lost and not
  // landed: the tick never happened, and folding them into either would make
  // the accounting fail to close for a reason that is not a fault.
  uint64_t orders_pending_at_exit = 0;
  // HOT-02 as observed on this link.
  uint64_t membership_updates = 0;
  uint64_t membership_full_sets = 0;
  uint64_t membership_bytes = 0;
  uint64_t membership_changes = 0;
  uint64_t frames_received = 0;
  uint64_t frame_bytes = 0;
  uint64_t frame_records = 0;
  uint64_t samples_verified = 0;
  // The two halves of the two-lane race, counted apart because they mean
  // opposite things. AHEAD: the unreliable frame overtook the reliable
  // membership that gives its slots meaning. BEHIND: the frame was built
  // against a set this client has already replaced.
  uint64_t frames_generation_ahead = 0;
  uint64_t frames_generation_behind = 0;
  uint64_t frames_before_first_set = 0;
  uint64_t frames_stale = 0;
  uint64_t frames_duplicate = 0;
  // A tick far beyond this follower's own progress is not a latest-value
  // sample, it is nonsense. Only the receiver can ask that question, because
  // only the receiver knows where it is.
  uint64_t frames_implausible_tick = 0;
  // Nonzero would mean the quantizer disagreed between two builds or two
  // machines, which is the whole reason the payload is verifiable at all.
  uint64_t corrections_needed = 0;
  uint64_t max_age_exceeded = 0;
};

// What survives a process death. The ticket and the derived secret are the only
// things a fresh process needs: the causal state itself comes back from the
// authority's checkpoint, so persisting it would be persisting a guess.
struct follower_ticket_file {
  net::reconnect_credential credential;
  net::credential_mac secret{};
  uint64_t issued_at = 0;
  uint64_t session = 0;
};

inline bool save_ticket(const std::filesystem::path& path, const follower_ticket_file& value) {
  std::vector<std::byte> credential;
  credential.reserve(net::reconnect_credential_bytes);
  if (net::try_encode_credential(value.credential, credential) != net::credential_status::accepted)
    return false;
  const auto temporary = std::filesystem::path(path).concat(".tmp");
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&value.issued_at), sizeof(value.issued_at));
    out.write(reinterpret_cast<const char*>(&value.session), sizeof(value.session));
    out.write(reinterpret_cast<const char*>(credential.data()), std::streamsize(credential.size()));
    out.write(reinterpret_cast<const char*>(value.secret.data()),
              std::streamsize(value.secret.size()));
    if (!out) return false;
  }
  std::error_code code;
  std::filesystem::rename(temporary, path, code);
  return !code;
}

inline bool load_ticket(const std::filesystem::path& path, follower_ticket_file& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  follower_ticket_file value;
  std::vector<std::byte> credential(net::reconnect_credential_bytes);
  in.read(reinterpret_cast<char*>(&value.issued_at), sizeof(value.issued_at));
  in.read(reinterpret_cast<char*>(&value.session), sizeof(value.session));
  in.read(reinterpret_cast<char*>(credential.data()), std::streamsize(credential.size()));
  in.read(reinterpret_cast<char*>(value.secret.data()), std::streamsize(value.secret.size()));
  if (!in) return false;
  if (net::try_decode_credential(credential, value.credential) != net::credential_status::accepted)
    return false;
  out = value;
  return true;
}

class follower_run {
public:
  follower_run(const lab_schedule& schedule, const size_t roster, const uint32_t host_address,
               const uint16_t port, const std::filesystem::path& ticket_path, const bool resume,
               const net::session_compatibility& compatibility = lab_compatibility(),
               const uint16_t envelope_version = net::session_wire_envelope_version)
    : schedule_(schedule), local_(compatibility), envelope_version_(envelope_version),
      ticket_path_(ticket_path), host_address_(host_address), port_(port), roster_(roster) {
    std::random_device source;
    nonce_seed_ = (uint64_t(source()) << 32) | uint64_t(source());
    scratch_.reserve(lab_max_message_bytes);
    reply_.reserve(net::session_wire_max_message_bytes);
    credential_bytes_.reserve(net::reconnect_credential_bytes);
    proposals_.reserve(lab_intent_window);
    // Deep enough for the lead plus slack; overflow is counted rather than
    // silently dropping the record.
    pending_orders_.reserve(32);
    batch_.reserve(lab_max_message_bytes);
    recovery_bundles_.reserve(128);
    deferred_.reserve(64);
    incoming_.intents.reserve(lab_max_intents_per_tick);
    // Decoding never grows caller storage, so the capacity is declared here or
    // the message is refused. A full set is the largest membership message this
    // stand can receive.
    membership_scratch_.reserve(lab_entity_count + lab_relevant_capacity);
    samples_.reserve(lab_relevant_capacity);
    if (resume) {
      verify_.require(load_ticket(ticket_path_, ticket_),
                      "a resuming follower could not read the ticket it persisted");
      has_ticket_ = true;
      resuming_ = true;
      // The persisted ticket's instants are the AUTHORITY's. A fresh process
      // has a brand-new monotonic origin, so it must anchor again.
      anchor_ = int64_t(ticket_.issued_at) - int64_t(monotonic_ms());
      anchored_ = true;
    }
    final_tick_ = schedule_.final_tick;
    verify_.require(roster_ < lab_join_tokens.size(),
                    "a follower was asked for a roster position which is not declared");
    verify_.require(link_.connect_to(port_, host_address_),
                    "follower could not open its first connection");
    peer_ = link_.pending();
  }

  verifier& checks() noexcept {
    return verify_;
  }
  const follower_counters& counters() const noexcept {
    return counters_;
  }
  const lab_host& host() const noexcept {
    return host_;
  }
  bool failed() const noexcept {
    return failed_;
  }
  const lab_link::conditions& conditions() const noexcept {
    return measured_;
  }
  uint64_t superseded() const noexcept {
    return link_.superseded();
  }
  // Live reading, for sampling during the run. `conditions()` is the one taken
  // at the last tick, which is the right value for the final line and the wrong
  // one for a time series.
  lab_link::conditions measure_now() {
    return connected_ ? link_.measure(peer_) : lab_link::conditions{};
  }
  // The pacing the AUTHORITY announced, which is the only true one: a
  // follower's own `--tick-ms` is not used for anything and reporting it would
  // be reporting the wrong quantity.
  uint64_t announced_tick_period_ms() const noexcept {
    return announced_tick_period_ms_;
  }
  uint64_t announced_lead_ticks() const noexcept {
    return announced_lead_ticks_;
  }
  // Proposals still waiting for their tick when the run ended.
  uint64_t orders_pending() const noexcept {
    return uint64_t(pending_orders_.size());
  }
  size_t roster() const noexcept {
    return roster_;
  }
  uint64_t final_tick() const noexcept {
    return final_tick_;
  }
  uint64_t exit_request() const noexcept {
    return exit_request_;
  }

  bool step() {
    const uint64_t local = monotonic_ms();
    poll_transport(local);
    drain_messages(local);
    drive_reconnect(local);
    if (exit_request_ != 0) return false;
    if (failed_) return false;
    if (host_.state.tick >= final_tick_) {
      if (linger_until_ == 0) {
        // Sampled HERE, at the last tick, not after the loop: by then the
        // linger has passed with no traffic and the backend reports a stale
        // connection instead of the one that just carried the run.
        measured_ = link_.measure(peer_);
        linger_until_ = local + lab_linger_ms;
      }
      return local < linger_until_;
    }
    return true;
  }

private:
  uint64_t authority_now(const uint64_t local) const noexcept {
    return anchored_ ? uint64_t(int64_t(local) + anchor_) : local;
  }

  // -------------------------------------------------------------- connections

  void poll_transport(const uint64_t local) {
    std::array<lab_link::observation, 4> events;
    const size_t count = link_.poll(events);
    for (size_t i = 0; i < count; ++i) {
      const auto& event = events[i];
      if (!(event.peer == peer_)) continue;
      if (event.connected && !connected_) {
        connected_ = true;
        if (coordinator_) coordinator_->observe_connected(authority_now(local));
        start_handshake();
      }
      if (event.terminal) {
        connected_ = false;
        // A connection which dies during the handshake is terminal for the
        // attempt: without this a client whose peer simply hung up would wait
        // forever for a refusal that is never coming.
        if (handshake_ && !handshake_->established()) {
          failed_ = true;
          return;
        }
        if (host_.state.tick < final_tick_) ++counters_.transport_losses;
        // A terminal handle stays OWNED until it is closed, so a reconnect
        // which does not close it finds the peer table full and spends its
        // attempts on a capacity refusal instead of on the network.
        (void)link_.close(peer_);
        // Direct evidence, so the silence budget is skipped entirely.
        if (coordinator_) coordinator_->observe_transport_lost(authority_now(local));
        else pending_transport_loss_ = true;
      }
    }
  }

  void start_handshake() {
    net::session_nonce nonce{};
    uint64_t word = splitmix64(nonce_seed_ ^ ++nonce_counter_);
    for (size_t i = 0; i < nonce.size(); ++i) {
      if (i % 8 == 0) word = splitmix64(word);
      nonce[i] = std::byte(uint8_t(word >> (8 * (i % 8))));
    }
    std::optional<uint64_t> resumed;
    std::optional<net::recovery_anchor<uint64_t, utils::digest>> confirmed;
    if (has_ticket_ && resuming_) resumed = ticket_.credential.ticket.session;
    const uint16_t supported_version =
      envelope_version_ <= net::session_wire_envelope_version
        ? envelope_version_
        : net::session_wire_envelope_version;
    handshake_.emplace(local_, nonce, resumed, confirmed, supported_version);
    scratch_.reserve(net::session_wire_max_message_bytes);
    verify_.require(handshake_->start(scratch_) == net::session_wire_status::ok,
                    "client hello did not encode");
    if (envelope_version_ > net::session_wire_envelope_version) {
      // This deliberately emulates a future build. The authority must reject
      // the envelope before it parses the payload or issues a challenge.
      scratch_[4] = std::byte(uint8_t(envelope_version_));
      scratch_[5] = std::byte(uint8_t(envelope_version_ >> 8));
    }
    verify_.require(link_.send(peer_, lab_lane_control, scratch_) == lab_send_result::sent,
                    "client hello could not be sent");
  }

  // ---------------------------------------------------------------- messages

  void drain_messages(const uint64_t local) {
    link_.receive([&](const net::gns_peer peer, const uint16_t lane,
                      const std::span<const std::byte> bytes) {
      if (!(peer == peer_) || bytes.empty()) return;
      if (coordinator_) coordinator_->observe_traffic(authority_now(local));
      if (handshake_ && !handshake_->established()) {
        on_handshake_message(bytes, local);
        return;
      }
      on_session_message(lane, bytes, local);
    });
  }

  void on_handshake_message(const std::span<const std::byte> bytes, const uint64_t local) {
    reply_.clear();
    reply_.reserve(net::session_wire_max_message_bytes);
    const auto status = handshake_->consume(bytes, reply_, *this);
    if (!reply_.empty()) (void)link_.send(peer_, lab_lane_control, reply_);
    if (status != net::session_wire_status::ok ||
        handshake_->phase() == net::handshake_phase::refused) {
      // A refusal is terminal. Retrying it on the reconnect schedule would only
      // spend the deadline.
      refusal_ = handshake_->refusal();
      if (coordinator_) coordinator_->observe_refused(authority_now(local));
      failed_ = true;
      return;
    }
    if (!handshake_->established()) return;

    const auto& accepted = handshake_->accepted();
    membership_.session = accepted.session;
    membership_.local_peer = accepted.local_peer;
    membership_.authority_peer = accepted.authority_peer;
    // The principal is what the AUTHORITY granted, echoed back in the accept.
    // A follower which used its own idea of who it is would not notice being
    // admitted as somebody else.
    membership_.principal = accepted.local_peer;
    verify_.require(membership_.principal == lab_principal_of(roster_),
                    "the authority admitted this follower under another principal");
    membership_.authority_epoch = accepted.authority_epoch;
    if (resuming_) ++counters_.reconnects;
    if (coordinator_) coordinator_->observe_admitted(authority_now(local));
    awaiting_recovery_ = resuming_;
    resuming_ = false;
  }

  void on_session_message(const uint16_t lane, const std::span<const std::byte> bytes,
                          const uint64_t local) {
    const auto type = uint8_t(bytes[0]);
    if (type == uint8_t(net::hot_message_type::relevant_set_update) ||
        type == uint8_t(net::hot_message_type::relevant_set_full)) {
      verify_.require(lane == lab_lane_control,
                      "membership arrived off the reliable ordered lane");
      on_relevant_set(bytes);
      return;
    }
    if (type == uint8_t(net::hot_message_type::transform_frame)) {
      verify_.require(lane == lab_lane_transform, "a transform frame arrived off its own lane");
      on_transform_frame(bytes);
      return;
    }
    if (type == uint8_t(lab_message::bundle)) {
      if (!decode_bundle(bytes, incoming_)) {
        verify_.require(false, "authority sent a bundle this follower cannot decode");
        return;
      }
      on_bundle(incoming_, local);
      return;
    }
    if (type == uint8_t(lab_message::reconnect_grant)) {
      lab_grant grant;
      verify_.require(decode_grant(bytes, grant), "reconnect grant did not decode");
      accept_grant(grant, local);
      return;
    }
    if (type == uint8_t(lab_message::recovery_plan)) {
      verify_.require(decode_recovery_plan(bytes, plan_), "recovery plan did not decode");
      if (plan_.target_tick > authority_tick_seen_) authority_tick_seen_ = plan_.target_tick;
      checkpoint_bytes_.assign(plan_.checkpoint_bytes, std::byte{});
      checkpoint_filled_ = 0;
      recovery_bundles_.clear();
      have_plan_ = true;
      return;
    }
    if (type == uint8_t(lab_message::recovery_unavailable)) {
      // "Recovery is impossible, join fresh" is a normal outcome, and this is
      // the branch which measures the authority's retention budget. The REASON
      // has to travel and be reported: three different faults reach this branch
      // and "impossible" alone cannot tell them apart.
      verify_.require(decode_recovery_unavailable(bytes, unrecoverable_reason_),
                      "unavailability did not decode");
      if (coordinator_) coordinator_->observe_unrecoverable(authority_now(local));
      unrecoverable_ = true;
      return;
    }
    if (type == uint8_t(lab_message::checkpoint_chunk)) {
      verify_.require(lane == lab_lane_bulk, "a checkpoint chunk arrived off the bulk lane");
      lab_chunk_view chunk;
      verify_.require(decode_chunk(bytes, chunk), "checkpoint chunk did not decode");
      verify_.require(have_plan_, "a checkpoint chunk arrived before its plan");
      verify_.require(size_t(chunk.offset) + chunk.payload.size() <= checkpoint_bytes_.size(),
                      "a checkpoint chunk claimed to reach past the declared size");
      std::copy(chunk.payload.begin(), chunk.payload.end(),
                checkpoint_bytes_.begin() + chunk.offset);
      checkpoint_filled_ += chunk.payload.size();
      ++counters_.chunks_received;
      try_recover(local);
      return;
    }
    verify_.require(false, "authority sent an unknown message class");
  }

  void accept_grant(const lab_grant& grant, const uint64_t local) {
    ticket_.credential = grant.credential;
    ticket_.secret = grant.session_secret;
    ticket_.issued_at = grant.issued_at;
    ticket_.session = grant.credential.ticket.session;
    has_ticket_ = true;
    // The run's end is the authority's to declare, and it can move: a late
    // rejoin extends it. Taking the larger value means an extension is honored
    // and a stale re-grant cannot shorten a run already under way.
    if (grant.final_tick > final_tick_) {
      final_tick_ = grant.final_tick;
      linger_until_ = 0;
    }
    // The expiry is in the authority's clock. Anchoring to the issue instant is
    // what makes the deadline the same deadline on both sides; without it the
    // two are unrelated numbers which happen to be milliseconds.
    anchor_ = int64_t(grant.issued_at) - int64_t(local);
    anchored_ = true;
    // Budgets announced by the authority, never assumed locally.
    announced_tick_period_ms_ = grant.tick_period_ms;
    announced_lead_ticks_ = grant.intent_lead_ticks;
    const uint64_t backoff = grant.tick_period_ms * 10 < 40 ? 40 : grant.tick_period_ms * 10;
    const net::reconnect_policy policy{
      .suspect_after = grant.suspect_after_ms,
      .lost_after = grant.lost_after_ms,
      .first_backoff = backoff,
      .max_backoff = grant.lost_after_ms * 2,
      .max_attempts = 8};
    verify_.require(policy.valid(), "the authority announced an incoherent reconnect policy");
    coordinator_.emplace(policy, grant.credential.ticket.expires_at, authority_now(local));
    if (pending_transport_loss_) {
      coordinator_->observe_transport_lost(authority_now(local));
      pending_transport_loss_ = false;
    }
  }

  // ------------------------------------------------------- relevant set and frames

  void on_relevant_set(const std::span<const std::byte> bytes) {
    net::relevant_set_update_view view;
    const auto status = net::try_decode_relevant_set_update(bytes, view, membership_scratch_);
    verify_.require(status == net::transform_wire_status::ok,
                    "the authority sent a relevant set this follower cannot decode");
    // A full set replaces; a diff merges and is only meaningful against exactly
    // the previous generation. This process may be a brand-new one whose mirror
    // is empty, which is why the scope has to be on the wire.
    const auto applied = view.scope == net::relevant_set_scope::full
                           ? mirror_.adopt(view.generation, membership_scratch_)
                           : mirror_.apply(view.generation, membership_scratch_);
    verify_.require(applied == net::transform_wire_status::ok,
                    "the membership lane delivered a generation this mirror cannot apply");
    if (view.scope == net::relevant_set_scope::full) {
      ++counters_.membership_full_sets;
      // A full set means this client's view is being re-established, so what
      // any class last knew is no longer the newest thing about it.
      for (auto& gate : gates_) gate.reset();
    } else {
      ++counters_.membership_updates;
    }
    counters_.membership_bytes += bytes.size();
    counters_.membership_changes += view.count;
    have_set_ = true;
  }

  void on_transform_frame(const std::span<const std::byte> bytes) {
    net::transform_frame_view view;
    const auto status = net::try_decode_transform_frame(bytes, transforms_, mirror_,
                                                       host_.state.tick, view, samples_);
    if (status == net::transform_wire_status::generation_mismatch) {
      // The refused view still reports the frame's own generation, which is the
      // only way to tell an overtaking frame from a stale one.
      if (!have_set_) ++counters_.frames_before_first_set;
      else if (view.generation != mirror_.generation() &&
               uint16_t(view.generation - mirror_.generation()) < 0x8000u)
        ++counters_.frames_generation_ahead;
      else
        ++counters_.frames_generation_behind;
      return;
    }
    verify_.require(status == net::transform_wire_status::ok,
                    "the authority sent a transform frame this follower cannot decode");
    // A sanity bound, and the basis for it is NOT this follower's own tick. A
    // rejoining process legitimately receives frames for ticks far beyond
    // anything it has applied — its checkpoint has not landed yet — and the
    // first thing this check caught was that assumption of mine rather than any
    // corrupt frame. The right basis is the furthest tick the AUTHORITY is
    // known to have reached, which a bundle or a recovery plan establishes.
    //
    // Dropped rather than fatal: the class is latest-value, so accepting a
    // nonsense future tick would poison the gate and every later frame with it,
    // while the frame itself arrived over an authenticated connection and
    // cannot have been tampered with in flight.
    if (authority_tick_seen_ != 0 &&
        view.tick > authority_tick_seen_ + lab_frame_future_slack_ticks) {
      ++counters_.frames_implausible_tick;
      return;
    }
    ++counters_.frames_received;
    counters_.frame_bytes += bytes.size();
    counters_.frame_records += view.count;

    // The payload is verified whatever the gate decides: an entity's transform
    // is a pure function of its index and the frame's own tick, so a late frame
    // is still exactly checkable. What the gate decides is only whether this
    // frame is the newest thing known about its class.
    for (const auto& sample : samples_) {
      const auto index = lab_entity_index(mirror_.handle_at(sample.slot));
      verify_.require(index.has_value(), "a frame named a slot holding no known entity");
      const std::array<double, 3> predicted{lab_entity_axis(*index, 0, view.tick),
                                            lab_entity_axis(*index, 1, view.tick),
                                            lab_entity_axis(*index, 2, view.tick)};
      const auto verdict = net::evaluate_correction(lab_axis, 3, predicted, sample,
                                                    net::correction_policy{});
      if (verdict.needs_correction || verdict.worst_error_codes != 0) {
        ++counters_.corrections_needed;
        verify_.require(false,
                        "an authoritative transform disagreed with the same computation here");
      }
      const auto turn = net::split_turn<uint16_t>(lab_entity_turn(*index, view.tick));
      verify_.require(uint16_t(turn.code) == sample.turn,
                      "an authoritative turn disagreed with the same computation here");
      ++counters_.samples_verified;
    }

    // Latest-value, per class. The lane is plain unreliable, so an older frame
    // really can arrive after a newer one; and staleness has to be per class
    // because two cadences legitimately sit at different ticks at once.
    switch (gate_for(view.class_id).commit(view.tick)) {
      case net::transform_frame_acceptance::accepted: break;
      case net::transform_frame_acceptance::duplicate: ++counters_.frames_duplicate; break;
      case net::transform_frame_acceptance::stale: ++counters_.frames_stale; break;
    }
  }

  net::transform_frame_gate& gate_for(const uint8_t class_id) {
    verify_.require(class_id < gates_.size(), "a frame named a class with no gate");
    return gates_[class_id];
  }

  // Declared staleness, checked once a tick rather than believed. A class which
  // has not arrived within its bound is one presentation must stop
  // extrapolating from, and this stand records how often that happened.
  void observe_max_age() {
    // An empty set is not a stale class: there is nothing to send and nothing
    // to extrapolate, so asking the question at all would count a correct
    // silence as a fault.
    if (!have_set_ || mirror_.size() == 0) return;
    for (const auto class_id : transforms_.classes()) {
      if (gate_for(class_id).exceeded_max_age(host_.state.tick, transforms_.cadence(class_id)))
        ++counters_.max_age_exceeded;
    }
  }

  // ------------------------------------------------------------------ bundles

  void on_bundle(const lab_bundle& bundle, const uint64_t local) {
    // What the authority is known to have reached, whether or not this bundle
    // can be applied yet.
    if (bundle.tick > authority_tick_seen_) authority_tick_seen_ = bundle.tick;
    if (awaiting_recovery_) {
      // Reliable ordered delivery puts the replay range ahead of later bundles,
      // but the checkpoint travels on a lower-priority lane and can arrive
      // after them. A follower which cannot hold those back would apply a tick
      // onto a state it has not restored yet.
      if (have_plan_ && bundle.tick <= plan_.target_tick) {
        recovery_bundles_.push_back({bundle.tick, bundle});
        try_recover(local);
        return;
      }
      verify_.require(deferred_.size() < deferred_.capacity(),
                      "deferred bundle storage overflowed during recovery");
      deferred_.push_back(bundle);
      ++counters_.bundles_deferred;
      return;
    }
    apply_live(bundle);
    propose_intents(local);
  }

  // Did this tick's canonical bundle carry the order proposed for it?
  void observe_order(const lab_bundle& bundle) {
    for (auto it = pending_orders_.begin(); it != pending_orders_.end();) {
      if (it->tick > bundle.tick) {
        ++it;
        continue;
      }
      if (it->tick < bundle.tick) {
        // The tick went by without being applied live -- only possible across a
        // recovery, and its bundles are not inspected here.
        ++counters_.orders_unobserved;
        it = pending_orders_.erase(it);
        continue;
      }
      const bool landed = std::ranges::any_of(bundle.intents, [&](const net::intent& value) {
        return value.kind == it->value.kind && value.cell_delta[0] == it->value.cell_delta[0] &&
               value.code[0] == it->value.code[0];
      });
      if (landed) ++counters_.orders_landed;
      else ++counters_.orders_lost;
      it = pending_orders_.erase(it);
    }
  }

  void apply_live(const lab_bundle& bundle) {
    traffic_started_ = true;
    observe_order(bundle);
    verify_.require(bundle.tick == host_.state.tick + 1,
                    "authority bundles arrived out of tick order on a reliable lane");
    authority_run::apply_bundle(host_, bundle);
    lab_step(host_, bundle.tick);
    ++counters_.bundles_applied;
    observe_max_age();
    maybe_self_exit();
  }

  void try_recover(const uint64_t local) {
    if (!awaiting_recovery_ || !have_plan_) return;
    if (checkpoint_filled_ != checkpoint_bytes_.size()) return;
    const uint64_t needed = plan_.target_tick - plan_.checkpoint_tick;
    if (recovery_bundles_.size() < needed) return;

    std::sort(recovery_bundles_.begin(), recovery_bundles_.end(),
              [](const replay_entry& l, const replay_entry& r) {
                return l.tick < r.tick;
              });

    net::session_recovery_plan<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t> plan;
    plan.session = membership_.session;
    plan.authority_peer = membership_.authority_peer;
    plan.principal = membership_.principal;
    plan.authority_epoch = membership_.authority_epoch;
    plan.checkpoint_tick = plan_.checkpoint_tick;
    plan.target_tick = plan_.target_tick;
    plan.checkpoint_root = plan_.checkpoint_root;
    plan.target_root = plan_.target_root;

    // Detached: a recovery which fails must not touch the running state. The
    // presentation counter is carried across on purpose, so a replay which
    // publishes a presentation effect is visible as a changed counter.
    lab_host candidate = host_;
    const uint64_t presentation_before = candidate.presentation_events;

    const auto result = net::recover_session(
      membership_, plan, candidate, checkpoint_bytes_, recovery_bundles_,
      [](lab_host& target, const std::vector<std::byte>& bytes) {
        net::state_reader reader{bytes};
        const auto loaded = lab_schema::load(
          target, reader, lab_state{},
          [](const lab_state& state) {
            return lab_section::validate(state);
          },
          [](lab_host& live, lab_state&& state) noexcept {
            live.state = std::move(state);
          });
        return loaded.loaded();
      },
      [](lab_host& target, const lab_bundle& bundle, const net::replay_context) {
        authority_run::apply_bundle(target, bundle);
        return true;
      },
      [](lab_host& target, const uint64_t tick, const net::replay_context context) {
        lab_step(target, tick);
        // Honoring the suppression is the project's contract: generic code
        // cannot see an effect hidden inside a callback.
        if (!context.presentation_suppressed()) ++target.presentation_events;
        return true;
      },
      [](const lab_host& state) {
        return lab_root(state);
      },
      [this](lab_host& published) noexcept {
        host_ = std::move(published);
      },
      net::checked_tick_successor<uint64_t>{});

    verify_.require(result.recovered(), "transactional recovery refused the authority's plan");
    if (!result.recovered()) {
      failed_ = true;
      return;
    }
    verify_.require(host_.presentation_events == presentation_before,
                    "replay published a presentation effect it was told to suppress");
    verify_.require(host_.state.tick == plan_.target_tick,
                    "recovery published a state at the wrong tick");
    counters_.replayed_ticks += result.replay ? result.replay->replayed_ticks : 0;
    ++counters_.recoveries;

    awaiting_recovery_ = false;
    have_plan_ = false;
    if (coordinator_) coordinator_->observe_recovered(authority_now(local));

    // Bundles which overtook the bulk transfer are applied now, in order.
    std::sort(deferred_.begin(), deferred_.end(), [](const lab_bundle& l, const lab_bundle& r) {
      return l.tick < r.tick;
    });
    for (const auto& bundle : deferred_) {
      if (bundle.tick <= host_.state.tick) continue;
      apply_live(bundle);
    }
    deferred_.clear();
    propose_intents(local);
  }

  // ------------------------------------------------------------------ intents

  // The authored target. This is the only floating-point step in the whole
  // path, and it is confined to the one header HOT-01 put it in.
  static double lab_authored_target(const uint64_t tick, const size_t roster) {
    // Per-follower, because three followers authoring the same target would
    // make the canonical order across principals unobservable: the bundle
    // would commit the same value whichever way it was sorted.
    const auto word = splitmix64((UINT64_C(0x746172676574) + roster * 0x9e37) ^ tick);
    return -48.0 + double(word % 12000u) * 0.008;
  }

  void propose_intents(const uint64_t local) {
    const uint64_t for_tick = host_.state.tick + announced_lead_ticks_;
    // Two cadences on purpose: one tick every window is SHARED by every
    // follower, so a bundle carrying several principals is guaranteed for any
    // roster size, and the rest are private, so single-principal ticks happen
    // too. An offset-only schedule looked fine with three followers and stopped
    // exercising the cross-principal order entirely with two -- the assertion
    // caught it, and the schedule was what was wrong.
    const uint64_t phase = for_tick % 9;
    const bool shared = phase == 4;
    const bool mine = phase == (5 + roster_) % 9;
    if (for_tick > last_proposed_ && (shared || mine)) {
      const auto split = net::split_axis(lab_axis, lab_authored_target(for_tick, roster_));
      verify_.require(!split.clamped, "an authored target left the declared fixed-point range");
      const int64_t quanta = (int64_t(split.key) << 16) | int64_t(split.code);
      const int32_t base_key = int32_t(host_.state.position >> 16);
      bool out_of_range = false;
      const auto value = lab_make_intent(quanta, base_key, 0, out_of_range);
      verify_.require(!out_of_range,
                      "an authored target was more cells away than the wire allows");
      if (proposals_.size() == proposals_.capacity()) proposals_.erase(proposals_.begin());
      proposals_.push_back({for_tick, value});
      // Same record again, kept until its tick is committed, so the proposal's
      // fate can be observed rather than inferred from copy counters.
      if (pending_orders_.size() == pending_orders_.capacity()) {
        ++counters_.orders_unobserved;
        pending_orders_.erase(pending_orders_.begin());
      }
      pending_orders_.push_back({for_tick, value});
      last_proposed_ = for_tick;
      ++counters_.intents_proposed;
    }
    if (schedule_.stale_batch_tick != 0 && host_.state.tick == schedule_.stale_batch_tick &&
        roster_ == schedule_.stale_batch_follower && !stale_sent_) {
      stale_sent_ = true;
      send_batch_at(host_.state.tick - 2);
      ++counters_.stale_batches_sent;
    }
    // A batch goes out EVERY tick, whether or not a new intent was authored.
    // That is where the redundancy lives: the same proposal appears in the next
    // few batches at a growing tick delta, so a lost packet costs nothing and
    // no retransmit protocol is needed.
    send_batch(local);
  }

  void send_batch(const uint64_t) {
    send_batch_at(host_.state.tick + announced_lead_ticks_);
  }

  void send_batch_at(const uint64_t base) {
    if (proposals_.empty() || !connected_) return;
    window_.clear();
    for (const auto& proposal : proposals_) {
      if (proposal.tick > base) continue;
      const uint64_t delta = base - proposal.tick;
      if (delta >= lab_intent_window) continue;
      auto value = proposal.value;
      value.tick_delta = uint8_t(delta);
      window_.push_back(value);
    }
    // A stale base can leave nothing inside the window; there is then nothing
    // to send and nothing to prove.
    if (window_.empty() && base < host_.state.tick) {
      window_.push_back(proposals_.back().value);
      window_.back().tick_delta = 0;
    }
    if (window_.empty()) return;
    batch_.reserve(lab_max_message_bytes);
    const auto status =
      net::try_encode_intent_batch(base, window_, layouts_, registry_, batch_);
    verify_.require(status == net::intent_wire_status::ok, "intent batch did not encode");
    verify_.require(batch_.size() <= net::intent_batch_max_bytes,
                    "intent batch left the declared single-packet budget");
    const auto sent = link_.send(peer_, lab_lane_intent, batch_);
    // An unreliable proposal may be refused by backpressure without any fault:
    // its own redundancy is the recovery.
    if (sent == lab_send_result::sent) ++counters_.batches_sent;
  }

  // ---------------------------------------------------------------- reconnect

  void drive_reconnect(const uint64_t local) {
    if (!coordinator_) return;
    // The silence budget measures INTERRUPTED traffic, so it cannot start
    // before traffic does. An authority waits for its whole roster before its
    // clock starts, which across machines is however long the operator takes
    // to launch the others; a follower admitted first would otherwise declare
    // the session lost, reconnect, and be told there is no checkpoint — a
    // failure whose every symptom points somewhere other than "the run has
    // not begun".
    if (!traffic_started_) return;
    // Past the final tick the authority is winding down on purpose. Counting
    // that silence as a loss would make both counters measure the shutdown
    // instead of the two failures the schedule actually injected.
    if (host_.state.tick >= final_tick_) return;
    const uint64_t now = authority_now(local);
    // Which budget declared the loss is visible only at the transition: a
    // coordinator entering `lost` from `live`/`suspect` inside poll was told by
    // silence, while `observe_transport_lost` had already put it there.
    const auto before = coordinator_->state();
    const auto decision = coordinator_->poll(now);
    switch (decision.action) {
      case net::reconnect_action::warn:
        ++counters_.warnings;
        return;
      case net::reconnect_action::connect: {
        if (coordinator_->state() == net::reconnect_state::attempting && !connecting_) {
          if (before == net::reconnect_state::live || before == net::reconnect_state::suspect)
            ++counters_.silence_losses;
          open_reconnect();
        }
        return;
      }
      case net::reconnect_action::give_up:
        failed_ = true;
        return;
      case net::reconnect_action::rejoin:
        // A real client would now join fresh. This stand records it instead of
        // pretending it recovered, which is the whole point of the answer.
        unrecoverable_ = true;
        failed_ = true;
        return;
      default: return;
    }
  }

  void open_reconnect() {
    connecting_ = true;
    connected_ = false;
    handshake_.reset();
    resuming_ = true;
    if (!link_.connect_to(port_, host_address_)) {
      connecting_ = false;
      return;
    }
    peer_ = link_.pending();
    connecting_ = false;
  }

  void maybe_self_exit() {
    if (schedule_.self_exit_tick == 0 || host_.state.tick != schedule_.self_exit_tick ||
        roster_ != schedule_.self_exit_follower)
      return;
    verify_.require(has_ticket_, "a follower about to die holds no ticket to come back with");
    verify_.require(save_ticket(ticket_path_, ticket_),
                    "a follower about to die could not persist its ticket");
    exit_request_ = 7;
  }

public:
  // ---------------------------------------------------- client_handshake_policy

  bool answer(const net::authority_challenge&, const utils::digest& transcript,
              std::vector<std::byte>& out) {
    if (!resuming_ || !has_ticket_) {
      // First join: the injected policy's token. Nothing here is a design for
      // real identity.
      const auto& token = lab_join_tokens[roster_];
      out.assign(reinterpret_cast<const std::byte*>(token.data()),
                 reinterpret_cast<const std::byte*>(token.data()) + token.size());
      return true;
    }
    // Reconnect: the entitlement is replayable by design, so what proves the
    // bearer is present is a MAC over THIS exchange's transcript.
    auto credential = ticket_.credential;
    if (net::seal_reconnect_presentation(ticket_.secret, transcript, follower_mac_,
                                         credential_scratch_, credential) !=
        net::credential_status::accepted)
      return false;
    credential_bytes_.reserve(net::reconnect_credential_bytes);
    if (net::try_encode_credential(credential, credential_bytes_) !=
        net::credential_status::accepted)
      return false;
    out.assign(credential_bytes_.begin(), credential_bytes_.end());
    return true;
  }

  bool unrecoverable() const noexcept {
    return unrecoverable_;
  }
  // net::recovery_feasibility as an integer: 0 recoverable, 1 no_checkpoint,
  // 2 history_gap, 3 target_before_checkpoint.
  uint8_t unrecoverable_reason() const noexcept {
    return unrecoverable_reason_;
  }
  net::session_refusal_reason refusal() const noexcept {
    return refusal_;
  }
  bool established() const noexcept {
    return handshake_.has_value() && handshake_->established();
  }

private:
  struct replay_entry {
    uint64_t tick = 0;
    lab_bundle bundle;
  };
  struct proposal {
    uint64_t tick = 0;
    net::intent value;
  };



  lab_schedule schedule_;
  verifier verify_;
  follower_counters counters_;
  gns_runtime runtime_;
  lab_link link_;
  lab_follower_mac follower_mac_;
  net::credential_scratch credential_scratch_;
  net::session_compatibility local_ = lab_compatibility();
  uint16_t envelope_version_ = net::session_wire_envelope_version;
  net::intent_layout_table layouts_ = lab_layouts();
  net::id_index_table registry_ = lab_registry();
  lab_host host_;
  std::optional<net::client_handshake> handshake_;
  std::optional<net::reconnect_coordinator> coordinator_;
  net::session_membership<uint64_t, uint64_t, uint64_t, uint64_t> membership_{};
  follower_ticket_file ticket_;
  lab_recovery_plan plan_;
  lab_bundle incoming_;
  std::vector<std::byte> scratch_, reply_, credential_bytes_, batch_, checkpoint_bytes_;
  std::vector<replay_entry> recovery_bundles_;
  std::vector<lab_bundle> deferred_;
  std::vector<proposal> proposals_;
  std::vector<proposal> pending_orders_;
  std::vector<net::intent> window_;
  net::transform_layout_table transforms_ = lab_transform_layouts();
  net::relevant_set_mirror mirror_{lab_relevant_capacity};
  std::array<net::transform_frame_gate, 4> gates_{};
  std::vector<net::slot_change> membership_scratch_;
  std::vector<net::transform_sample> samples_;
  std::filesystem::path ticket_path_;
  net::gns_peer peer_;
  lab_link::conditions measured_;

  uint32_t host_address_ = 0x7f000001;
  int64_t anchor_ = 0;
  uint64_t nonce_seed_ = 0, nonce_counter_ = 0;
  uint64_t checkpoint_filled_ = 0, last_proposed_ = 0, exit_request_ = 0, linger_until_ = 0;
  uint64_t final_tick_ = 0;
  uint64_t authority_tick_seen_ = 0;
  uint64_t announced_tick_period_ms_ = 0;
  // Until the grant arrives there is nothing to propose for anyway.
  uint64_t announced_lead_ticks_ = 4;
  uint8_t unrecoverable_reason_ = 0;
  uint16_t port_ = 0;
  size_t roster_ = 0;
  net::session_refusal_reason refusal_ = net::session_refusal_reason::none;
  bool connected_ = false, connecting_ = false, has_ticket_ = false, resuming_ = false;
  bool anchored_ = false, awaiting_recovery_ = false, have_plan_ = false;
  bool failed_ = false, unrecoverable_ = false, pending_transport_loss_ = false;
  bool stale_sent_ = false, traffic_started_ = false, have_set_ = false;
};

static_assert(net::client_handshake_policy<follower_run>);

} // namespace netlab01

#endif
