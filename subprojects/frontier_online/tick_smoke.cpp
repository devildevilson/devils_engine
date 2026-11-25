// A real actor world predicts an input which the authority never accepts.
// Canonical tick messages close every tick; replay from the join checkpoint
// removes that prediction and reaches the authority's full causal root.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <devils_engine/network/canonical_tick_wire.h>
#include <devils_engine/network/replay.h>
#include <devils_engine/network/sequence_window.h>
#include <devils_engine/network/tick_journal.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/log.h>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/timeline.h>
#include <spdlog/spdlog.h>

#include "core/actor_checkpoint.h"
#include "core/actor_simulation.h"
#include "core/causal_content.h"
#include "net/player_intent_wire.h"

namespace fo = frontier_online::core;
namespace fn = frontier_online::net;
namespace net = devils_engine::network;
namespace utils = devils_engine::utils;

namespace {

void require(const bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}

// This one project action is deliberately exact downstream. The upstream
// quantized proposal still needs its project adapter and authority validation.
std::vector<std::byte> food_action_bytes(const double x, const double y) {
  std::vector<std::byte> bytes;
  require(fn::encode(fn::canonical_food_action{x, y}, bytes), "food action did not encode");
  return bytes;
}

devils_engine::act::intent food_action(const double x, const double y) {
  devils_engine::act::intent action;
  action.kind = devils_engine::act::intent_kind::spawn_prefab;
  action.payload.spawn.prefab = utils::string_hash("food");
  action.payload.spawn.target = devils_engine::act::vec3{x, y, 0.0};
  action.source_action = utils::string_hash("network_spawn_food");
  return action;
}

// The same reading the live session gives a canonical record: a join creates the peer's
// player, a leave removes it, food is executed by the player of its origin peer.
bool decode_and_enqueue(fo::actor_world_slice& actors, const uint64_t peer, const uint64_t sequence,
                        const std::span<const std::byte> bytes) {
  switch (fn::classify(bytes, sequence)) {
    case fn::canonical_action_kind::peer_joined: return actors.enqueue_canonical_join(peer);
    case fn::canonical_action_kind::peer_left: return actors.enqueue_canonical_leave(peer);
    case fn::canonical_action_kind::spawn_food: {
      fn::canonical_food_action action;
      return fn::decode(bytes, action) &&
             actors.enqueue_canonical_intent(food_action(action.x, action.y), peer);
    }
    case fn::canonical_action_kind::invalid: break;
  }
  return false;
}

uint64_t root_of(const fo::actor_world_slice& actors, const utils::timelines& clocks) {
  fo::actor_checkpoint_buffers buffers;
  net::state_digest_report<uint64_t> digest;
  digest.sections.reserve(fo::actor_checkpoint_section_count);
  require(fo::actor_checkpoint_digest(actors, clocks, buffers, digest), "digest failed");
  return digest.root;
}

struct host {
  fo::actor_world_slice actors;
  utils::timelines clocks{utils::simulation_rate(60)};
  devils_engine::thread::atomic_pool pool{2};
  uint64_t expected_root = 0;
  uint64_t pending_tick = 0;
};

void step(host& value, const uint64_t number) {
  const auto tick = value.clocks.simulation_now() + utils::simulation_duration{1};
  require(tick.value == number, "simulation tick skipped");
  value.actors.update(tick, value.clocks.advance_simulation(tick), nullptr, value.pool);
}

struct wire_tick {
  uint64_t tick = 0;
  std::vector<std::byte> bundle;
};

struct accepted_input {
  uint64_t tick = 0;
  uint64_t origin_peer = 0;
  uint64_t sequence = 0;
  devils_engine::act::intent action;
  std::vector<std::byte> payload;
};

struct input_tick {
  uint64_t operator()(const accepted_input& value) const noexcept { return value.tick; }
};

struct input_less {
  bool operator()(const accepted_input& a, const accepted_input& b) const noexcept {
    if (a.origin_peer != b.origin_peer) return a.origin_peer < b.origin_peer;
    return a.sequence < b.sequence;
  }
};

struct input_same {
  bool operator()(const accepted_input& a, const accepted_input& b) const noexcept {
    return a.origin_peer == b.origin_peer && a.sequence == b.sequence;
  }
};

using input_journal = net::tick_journal<accepted_input, uint64_t, input_tick, input_less, input_same>;

} // namespace

int main() {
  try {
    spdlog::set_level(spdlog::level::err);
    fo::causal_content content(frontier_online::core::project_modules_root());
    host authority;
    authority.actors.init(128, {0.5f, 0.5f}, {64.0f, 64.0f}, 1, content.config());
    fo::actor_checkpoint_buffers initial;
    require(fo::write_actor_checkpoint(authority.actors, authority.clocks, initial),
            "initial checkpoint failed");
    const uint64_t initial_root = root_of(authority.actors, authority.clocks);

    host predicted;
    require(fo::load_actor_checkpoint(predicted.actors, predicted.clocks,
                                      initial.document, content.config()).loaded(),
            "prediction checkpoint refused");
    require(root_of(predicted.actors, predicted.clocks) == initial_root,
            "prediction checkpoint root differs");

    const auto layouts = fn::player_intent_layouts();
    const net::id_index_table empty_registry; // spawn_food has no indexed reference field
    net::intent proposal;
    require(fn::encode_player_intent(food_action(12.0, 13.0), 2, 0, proposal),
            "project action did not fit the intent lattice");
    std::vector<std::byte> upstream;
    upstream.reserve(net::intent_batch_max_bytes);
    require(net::try_encode_intent_batch(4, std::span(&proposal, 1), layouts, empty_registry,
                                         upstream) == net::intent_wire_status::ok,
            "upstream intent did not encode");
    net::intent dropped_proposal;
    require(fn::encode_player_intent(food_action(15.0, 13.0), 1, 0, dropped_proposal), "dropped action did not encode");
    devils_engine::act::intent predicted_dropped;
    require(fn::decode_player_intent(dropped_proposal, predicted_dropped),
            "dropped prediction did not decode");
    devils_engine::act::intent predicted_accepted;
    require(fn::decode_player_intent(proposal, predicted_accepted),
            "accepted prediction did not decode");

    input_journal journal;
    net::sequence_window<uint16_t, 32> seen;
    seen.reset(0); // first client input sequence is 1
    std::vector<wire_tick> history;
    history.reserve(6);
    size_t wire_bytes = 0;
    for (uint64_t tick = 1; tick <= 6; ++tick) {
      const auto tag = journal.begin(tick, net::canonical_tick_max_records);
      if (tick == 1) {
        // Peer 1 agreed on the checkpoint: its player enters the world by a canonical record.
        accepted_input join;
        join.tick = tick;
        join.origin_peer = 1;
        join.sequence = fn::canonical_join_sequence;
        join.payload = fn::encode_join();
        require(journal.try_record(std::move(join)) == net::tick_record_result::recorded,
                "join did not enter the journal");
      }
      if (tick == 4) {
        const auto admit = [&](const uint64_t authenticated_peer) {
          // Ownership comes from the established membership, not from bytes.
          if (authenticated_peer != 1) return false;
          net::intent_batch_view view;
          std::vector<net::intent> decoded;
          decoded.reserve(2);
          if (net::try_decode_intent_batch(upstream, layouts, empty_registry, view, decoded) !=
                net::intent_wire_status::ok || decoded.size() != 1) return false;
          const auto& incoming = decoded.front();
          if (net::intent_absolute_tick(tick, view.base_tick_low, incoming.tick_delta) != tick ||
              seen.classify(incoming.sequence_low) != net::sequence_classification::new_value)
            return false;
          devils_engine::act::intent action;
          if (!fn::decode_player_intent(incoming, action)) return false;
          accepted_input record;
          record.tick = tick;
          record.origin_peer = authenticated_peer;
          record.sequence = net::widen_input_sequence_low16(0, incoming.sequence_low);
          record.action = action;
          record.payload = food_action_bytes(action.payload.spawn.target.x,
                                             action.payload.spawn.target.y);
          if (journal.try_record(std::move(record)) != net::tick_record_result::recorded)
            return false;
          return seen.observe(incoming.sequence_low) == net::sequence_classification::new_value;
        };
        require(!admit(2), "foreign logical peer controlled the player");
        require(admit(1), "valid network proposal was not admitted");
        require(!admit(1), "resent proposal entered the journal twice");
      }
      require(journal.seal() == net::tick_seal_result::sealed, "tick journal did not seal");
      auto sealed = journal.consume(tag);
      for (const auto& record : sealed.records())
        require(decode_and_enqueue(authority.actors, record.origin_peer, record.sequence,
                                   record.payload),
                "sealed action could not enter causal ECS");
      step(authority, tick);
      const auto root = root_of(authority.actors, authority.clocks);
      std::vector<net::canonical_tick_record_view> accepted;
      accepted.reserve(sealed.records().size());
      for (const auto& record : sealed.records())
        accepted.push_back({record.origin_peer, record.sequence, record.payload});
      auto& entry = history.emplace_back();
      entry.tick = tick;
      entry.bundle.reserve(net::canonical_tick_max_bytes);
      require(net::try_encode_canonical_tick(tick, root, accepted, entry.bundle) ==
                net::canonical_tick_wire_status::ok, "canonical tick did not encode");
      wire_bytes += entry.bundle.size();
      auto recycled = std::move(sealed).release_storage();
      journal.retire(tag);
      journal.recycle(std::move(recycled));
    }

    // Client proposal 1 for tick 2 never reached authority. Proposal 2 for
    // tick 4 did. The local predicted world includes both until tick 2 closes.
    for (uint64_t tick = 1; tick <= 6; ++tick) {
      if (tick == 2) require(predicted.actors.enqueue_player_intent(predicted_dropped),
                              "predicted action rejected");
      if (tick == 4) require(predicted.actors.enqueue_player_intent(predicted_accepted),
                              "predicted accepted action rejected");
      step(predicted, tick);
    }
    const uint64_t authoritative_root = root_of(authority.actors, authority.clocks);
    const uint64_t predicted_root = root_of(predicted.actors, predicted.clocks);
    require(predicted_root != authoritative_root, "wrong prediction was not detected");

    host corrected; // detached candidate: a refusal cannot corrupt the predicted live world
    const auto restore = [&](host& target, const std::vector<std::byte>& checkpoint) {
      target.expected_root = initial_root;
      return fo::load_actor_checkpoint(target.actors, target.clocks, checkpoint,
                                       content.config()).loaded();
    };
    const auto apply = [&](host& target, const std::vector<std::byte>& bytes,
                           const net::replay_context context) {
      if (!context.presentation_suppressed()) return false;
      std::array<net::canonical_tick_record_view, net::canonical_tick_max_records> storage{};
      net::canonical_tick_view view;
      if (net::try_decode_canonical_tick(bytes, target.clocks.simulation_now().value, storage, view) !=
            net::canonical_tick_wire_status::ok || !view.state_root) return false;
      if (view.tick != target.clocks.simulation_now().value + 1) return false;
      target.pending_tick = view.tick;
      target.expected_root = *view.state_root;
      for (const auto& record : view.records) {
        if (record.origin_peer != 1 ||
            (record.input_sequence != 2 && record.input_sequence != fn::canonical_join_sequence) ||
            !decode_and_enqueue(target.actors, record.origin_peer, record.input_sequence,
                                record.payload)) return false;
      }
      return true;
    };
    const auto advance = [](host& target, const uint64_t tick,
                            const net::replay_context context) {
      if (!context.presentation_suppressed() || tick != target.pending_tick) return false;
      step(target, tick);
      return true;
    };
    const auto verify = [](const host& target, const uint64_t tick) {
      return target.clocks.simulation_now().value == tick &&
             root_of(target.actors, target.clocks) == target.expected_root;
    };
    const auto result = net::replay_to(corrected, uint64_t(0), initial.document, uint64_t(6),
      history, restore, apply, advance, verify, net::checked_tick_successor<uint64_t>{});
    require(result.completed() && result.replayed_ticks == 6, "canonical replay failed");
    require(root_of(corrected.actors, corrected.clocks) == authoritative_root,
            "replayed root differs from authority");
    require(root_of(predicted.actors, predicted.clocks) == predicted_root,
            "detached replay changed live prediction");

    // One player per peer, created only by the canonical join. None of these refusals mutates.
    const auto corrected_root = root_of(corrected.actors, corrected.clocks);
    require(corrected.actors.player_of(1) != devils_engine::aesthetics::invalid_entityid &&
              corrected.actors.player_of(1) == authority.actors.player_of(1) &&
              corrected.actors.player_of(1) != corrected.actors.player_entity(),
            "peer 1 has no player of its own after the replayed join");
    require(!corrected.actors.enqueue_canonical_intent(food_action(5.0, 5.0), 2),
            "an action of a peer without a player was accepted");
    require(!corrected.actors.enqueue_canonical_join(1) && !corrected.actors.enqueue_canonical_join(0),
            "a duplicate or local join was accepted");
    require(!decode_and_enqueue(corrected.actors, 1, 7, fn::encode_join()) &&
              !decode_and_enqueue(corrected.actors, 1, fn::canonical_join_sequence,
                                  food_action_bytes(1.0, 1.0)),
            "a join with an input sequence or food with the join sequence was accepted");
    require(!corrected.actors.player_inputs_pending() &&
              root_of(corrected.actors, corrected.clocks) == corrected_root,
            "a refused record changed the world");

    // Canonical leave: the player of peer 1 disappears on the same tick boundary everywhere.
    // A leave in the tick of its own action is refused (the authority drops the actions of a
    // departed peer), and nothing acts for the peer once its leave is queued.
    const auto departed_player = authority.actors.player_of(1);
    for (host* target : {&authority, &corrected}) {
      auto& actors = target->actors;
      require(!actors.enqueue_canonical_leave(0) && !actors.enqueue_canonical_leave(2),
              "a leave of the local player or of a peer without a player was accepted");
      require(!decode_and_enqueue(actors, 1, fn::canonical_join_sequence, fn::encode_leave()) &&
                !decode_and_enqueue(actors, 1, 9, fn::encode_leave()) &&
                !decode_and_enqueue(actors, 1, fn::canonical_leave_sequence, fn::encode_join()),
              "a leave with a foreign sequence or a join with the leave sequence was accepted");
      require(actors.enqueue_canonical_intent(food_action(3.0, 3.0), 1) &&
                !actors.enqueue_canonical_leave(1),
              "a leave in the tick of the peer's own action was accepted");
      step(*target, 7);
      require(decode_and_enqueue(actors, 1, fn::canonical_leave_sequence, fn::encode_leave()),
              "a valid canonical leave was refused");
      require(!actors.enqueue_canonical_leave(1) &&
                !actors.enqueue_canonical_intent(food_action(4.0, 4.0), 1) &&
                actors.player_inputs_pending(),
              "a second leave or an action after the leave was accepted");
      step(*target, 8);
      require(actors.player_of(1) == devils_engine::aesthetics::invalid_entityid &&
                !actors.ecs().exists(departed_player) && !actors.player_inputs_pending() &&
                actors.player_of(0) == actors.player_entity(),
              "the departed player survived its leave");
    }
    require(root_of(authority.actors, authority.clocks) ==
              root_of(corrected.actors, corrected.clocks),
            "a canonical leave diverged between the authority and a follower");

    // Missing a reliable bundle must refuse before checkpoint restoration or
    // partial replay, even if the missing tick would have been empty.
    auto gap = history;
    gap.erase(gap.begin() + 2);
    host untouched;
    const auto missing = net::replay_to(untouched, uint64_t(0), initial.document, uint64_t(6),
      gap, restore, apply, advance, verify, net::checked_tick_successor<uint64_t>{});
    require(missing.status == net::replay_status::missing_bundle &&
              missing.tick == 3 && untouched.clocks.simulation_now().value == 0,
            "missing empty tick was not refused before restore");

    auto corrupted = history;
    corrupted[3].bundle[net::canonical_tick_header_bytes] ^= std::byte{1};
    host refused;
    const auto mismatch = net::replay_to(refused, uint64_t(0), initial.document, uint64_t(6),
      corrupted, restore, apply, advance, verify, net::checked_tick_successor<uint64_t>{});
    require(mismatch.status == net::replay_status::state_mismatch && mismatch.tick == 4 &&
              root_of(predicted.actors, predicted.clocks) == predicted_root,
            "wrong authoritative root was not refused");

    std::printf("canonical tick: %zu upstream bytes, 6 bundles, %zu downstream bytes, wrong root %llu, corrected root %llu\n",
                upstream.size(), wire_bytes, static_cast<unsigned long long>(predicted_root),
                static_cast<unsigned long long>(authoritative_root));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "canonical tick smoke failed: %s\n", error.what());
    return 1;
  }
}
