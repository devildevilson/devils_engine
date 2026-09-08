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
               const uint16_t port, const std::filesystem::path& ticket_path, const bool resume)
    : schedule_(schedule), ticket_path_(ticket_path), host_address_(host_address), port_(port),
      roster_(roster) {
    std::random_device source;
    nonce_seed_ = (uint64_t(source()) << 32) | uint64_t(source());
    scratch_.reserve(lab_max_message_bytes);
    reply_.reserve(net::session_wire_max_message_bytes);
    credential_bytes_.reserve(net::reconnect_credential_bytes);
    proposals_.reserve(lab_intent_window);
    batch_.reserve(lab_max_message_bytes);
    recovery_bundles_.reserve(128);
    deferred_.reserve(64);
    incoming_.intents.reserve(lab_max_intents_per_tick);
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
    handshake_.emplace(local_, nonce, resumed, confirmed);
    scratch_.reserve(net::session_wire_max_message_bytes);
    verify_.require(handshake_->start(scratch_) == net::session_wire_status::ok,
                    "client hello did not encode");
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
      checkpoint_bytes_.assign(plan_.checkpoint_bytes, std::byte{});
      checkpoint_filled_ = 0;
      recovery_bundles_.clear();
      have_plan_ = true;
      return;
    }
    if (type == uint8_t(lab_message::recovery_unavailable)) {
      // "Recovery is impossible, join fresh" is a normal outcome, and this is
      // the branch which measures the authority's retention budget.
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
    const net::reconnect_policy policy{
      .suspect_after = 60, .lost_after = 220,
      .first_backoff = 40, .max_backoff = 320, .max_attempts = 8};
    verify_.require(policy.valid(), "the laboratory reconnect policy is incoherent");
    coordinator_.emplace(policy, grant.credential.ticket.expires_at, authority_now(local));
    if (pending_transport_loss_) {
      coordinator_->observe_transport_lost(authority_now(local));
      pending_transport_loss_ = false;
    }
  }

  // ------------------------------------------------------------------ bundles

  void on_bundle(const lab_bundle& bundle, const uint64_t local) {
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

  void apply_live(const lab_bundle& bundle) {
    verify_.require(bundle.tick == host_.state.tick + 1,
                    "authority bundles arrived out of tick order on a reliable lane");
    authority_run::apply_bundle(host_, bundle);
    lab_step(host_, bundle.tick);
    ++counters_.bundles_applied;
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
    const uint64_t for_tick = host_.state.tick + intent_lead;
    // Offsets chosen so that some ticks carry intents from SEVERAL principals
    // and some from one: a schedule where they never collide would never
    // exercise the cross-principal order at all.
    if (for_tick > last_proposed_ && for_tick % 9 == (4 + roster_ % 2)) {
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
    send_batch_at(host_.state.tick + intent_lead);
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

  static constexpr uint64_t intent_lead = 4;

  lab_schedule schedule_;
  verifier verify_;
  follower_counters counters_;
  gns_runtime runtime_;
  lab_link link_;
  lab_follower_mac follower_mac_;
  net::credential_scratch credential_scratch_;
  net::session_compatibility local_ = lab_compatibility();
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
  std::vector<net::intent> window_;
  std::filesystem::path ticket_path_;
  net::gns_peer peer_;
  lab_link::conditions measured_;
  uint32_t host_address_ = 0x7f000001;
  int64_t anchor_ = 0;
  uint64_t nonce_seed_ = 0, nonce_counter_ = 0;
  uint64_t checkpoint_filled_ = 0, last_proposed_ = 0, exit_request_ = 0, linger_until_ = 0;
  uint64_t final_tick_ = 0;
  uint16_t port_ = 0;
  size_t roster_ = 0;
  net::session_refusal_reason refusal_ = net::session_refusal_reason::none;
  bool connected_ = false, connecting_ = false, has_ticket_ = false, resuming_ = false;
  bool anchored_ = false, awaiting_recovery_ = false, have_plan_ = false;
  bool failed_ = false, unrecoverable_ = false, pending_transport_loss_ = false;
  bool stale_sent_ = false;
};

static_assert(net::client_handshake_policy<follower_run>);

} // namespace netlab01

#endif
