// A real loopback GNS connection survives join and carries project input up,
// then every canonical tick down. The client predicts locally and replays only
// the bytes it actually received over that connection.
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <devils_engine/network/canonical_tick_wire.h>
#include <devils_engine/network/replay.h>
#include <devils_engine/network/sequence_window.h>
#include <devils_engine/network/tick_journal.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/timeline.h>
#include <spdlog/spdlog.h>

#include "core/actor_checkpoint.h"
#include "core/actor_simulation.h"
#include "core/causal_content.h"
#include "net/authority.h"
#include "net/follower.h"
#include "net/player_intent_wire.h"

namespace fo = frontier_online::core;
namespace fn = frontier_online::net;
namespace net = devils_engine::network;
namespace utils = devils_engine::utils;

namespace {

void require(const bool okay, const char* reason) {
  if (!okay) throw std::runtime_error(reason);
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

uint64_t root_of(const host& value) {
  fo::actor_checkpoint_buffers buffers;
  net::state_digest_report<uint64_t> digest;
  digest.sections.reserve(fo::actor_checkpoint_section_count);
  require(fo::actor_checkpoint_digest(value.actors, value.clocks, buffers, digest),
          "actor digest failed");
  return digest.root;
}

class source final : public fn::checkpoint_source {
public:
  explicit source(const host& value) : value_(value) {}

  bool capture(fn::checkpoint_begin& header, std::vector<std::byte>& document) override {
    fo::actor_checkpoint_buffers buffers;
    net::state_digest_report<uint64_t> digest;
    digest.sections.reserve(fo::actor_checkpoint_section_count);
    if (!fo::actor_checkpoint_digest(value_.actors, value_.clocks, buffers, digest)) return false;
    header.tick = value_.clocks.simulation_now().value;
    header.root = digest.root;
    header.total_bytes = uint32_t(buffers.document.size());
    header.schema_fingerprint = fo::actor_checkpoint_schema_fingerprint();
    document = std::move(buffers.document);
    return true;
  }

private:
  const host& value_;
};

class sink final : public fn::checkpoint_sink {
public:
  sink(host& value, const fo::brain_config& brains) : value_(value), brains_(brains) {}

  bool load(const fn::checkpoint_begin& header, const std::span<const std::byte> document,
            uint64_t& root) override {
    if (header.schema_fingerprint != fo::actor_checkpoint_schema_fingerprint()) return false;
    if (!fo::load_actor_checkpoint(value_.actors, value_.clocks, document, brains_).loaded())
      return false;
    checkpoint.assign(document.begin(), document.end());
    root = root_of(value_);
    return true;
  }

  std::vector<std::byte> checkpoint;

private:
  host& value_;
  const fo::brain_config& brains_;
};

devils_engine::act::intent food_action(const double x, const double y) {
  devils_engine::act::intent action;
  action.kind = devils_engine::act::intent_kind::spawn_prefab;
  action.payload.spawn.prefab = utils::string_hash("food");
  action.payload.spawn.target = devils_engine::act::vec3{x, y, 0.0};
  action.source_action = utils::string_hash("network_spawn_food");
  return action;
}

std::vector<std::byte> action_payload(const devils_engine::act::intent& action) {
  std::vector<std::byte> bytes;
  bytes.reserve(25);
  net::state_writer writer(bytes, false);
  writer.u8(1);
  writer.u64(action.payload.spawn.prefab);
  writer.u64(std::bit_cast<uint64_t>(action.payload.spawn.target.x));
  writer.u64(std::bit_cast<uint64_t>(action.payload.spawn.target.y));
  require(writer.good() && bytes.size() == 25, "canonical project action did not encode");
  return bytes;
}

bool enqueue_payload(host& target, const std::span<const std::byte> bytes) {
  net::state_reader reader(bytes);
  const uint8_t kind = reader.u8();
  const uint64_t prefab = reader.u64();
  const double x = std::bit_cast<double>(reader.u64());
  const double y = std::bit_cast<double>(reader.u64());
  return reader.good() && reader.position() == reader.size() && kind == 1 &&
         prefab == utils::string_hash("food") && std::isfinite(x) && std::isfinite(y) &&
         target.actors.enqueue_player_intent(food_action(x, y));
}

struct wire_tick {
  uint64_t tick = 0;
  std::vector<std::byte> bundle;
};

struct accepted_input {
  uint64_t tick = 0;
  uint64_t logical_peer = 0;
  uint64_t sequence = 0;
  devils_engine::act::intent action;
  std::vector<std::byte> payload;
};

struct input_tick {
  uint64_t operator()(const accepted_input& value) const noexcept { return value.tick; }
};

struct input_less {
  bool operator()(const accepted_input& a, const accepted_input& b) const noexcept {
    if (a.logical_peer != b.logical_peer) return a.logical_peer < b.logical_peer;
    return a.sequence < b.sequence;
  }
};

struct input_same {
  bool operator()(const accepted_input& a, const accepted_input& b) const noexcept {
    return a.logical_peer == b.logical_peer && a.sequence == b.sequence;
  }
};

using input_journal = net::tick_journal<accepted_input, uint64_t, input_tick,
                                        input_less, input_same>;

} // namespace

int main(const int argc, const char* const* argv) {
  try {
    require(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--actors=1024"),
            "expected optional --actors=1024");
    const size_t actor_count = argc == 2 ? 1024 : 128;
    spdlog::set_level(spdlog::level::err);
    fo::causal_content authority_content(FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT);
    fo::causal_content follower_content(FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT);
    auto terrain = authority_content.make_terrain("generator/world", 16, 20260911);
    const auto world = fn::declare_world(*terrain, "generator/world");
    fn::net_runtime runtime(8);
    fn::authority_config config;
    config.host = 0x7f000001;
    config.port = 0;
    config.max_peers = 1;
    config.verbose = false;
    config.keep_joined_connection = true;
    fn::authority authority(runtime, authority_content, world, config);

    host authoritative;
    authoritative.actors.init(actor_count, {0.5f, 0.5f}, {64.0f, 64.0f}, 1,
                              authority_content.config());
    step(authoritative, 1);
    source checkpoint_source(authoritative);
    const uint64_t join_root = root_of(authoritative);

    fn::follower_config client_config;
    client_config.host = config.host;
    client_config.port = authority.port();
    client_config.verbose = false;
    fn::follower follower(runtime, follower_content, client_config);
    require(follower.start(), "client did not start connecting");
    host predicted;
    sink checkpoint_sink(predicted, follower_content.config());

    const auto drive = [&] {
      runtime.pump();
      authority.step(checkpoint_source);
      follower.step(checkpoint_sink);
    };
    const auto joined_by = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < joined_by &&
           (authority.completed_joins() != 1 || !follower.live_ready())) {
      drive();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(authority.completed_joins() == 1 && follower.live_ready() &&
              follower.phase() == fn::follower_phase::joined && !follower.live_fault(),
            "live GNS join was not acknowledged");
    require(authority.connected_peers() == 1 && !follower.live_terminal(),
            "joined GNS connection was closed");
    require(follower.report().tick == 1 && follower.report().root == join_root &&
              root_of(predicted) == join_root && !checkpoint_sink.checkpoint.empty(),
            "joined checkpoint differs from authority");
    const uint64_t logical_peer = follower.peer_id();
    require(logical_peer != 0 && authority.joins().front().peer_id == logical_peer &&
              follower.session() == authority.session(), "joined membership differs");

    const auto layouts = fn::player_intent_layouts();
    const net::id_index_table empty_registry;
    const std::array<int32_t, 2> base_cells{0, 0};
    net::intent proposal;
    require(fn::encode_player_intent(food_action(12.0, 13.0), 2, 0, base_cells, proposal),
            "player intent did not quantize");
    std::vector<std::byte> upstream;
    upstream.reserve(net::intent_batch_max_bytes);
    require(net::try_encode_intent_batch(4, std::span(&proposal, 1), layouts, empty_registry,
                                         upstream) == net::intent_wire_status::ok,
            "player intent batch did not encode");
    require(follower.send_intent(upstream) == fn::send_result::sent,
            "GNS refused the live upstream proposal");

    std::vector<fn::live_intent_packet> arrived;
    const auto input_by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < input_by && arrived.empty()) {
      drive();
      arrived = authority.take_live_intents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(arrived.size() == 1 && arrived[0].logical_peer == logical_peer &&
              arrived[0].bytes == upstream && authority.dropped_live_intents() == 0,
            "upstream GNS intent did not retain its established peer route");
    net::intent_batch_view received_batch;
    std::vector<net::intent> decoded;
    decoded.reserve(2);
    require(net::try_decode_intent_batch(arrived[0].bytes, layouts, empty_registry,
                                          received_batch, decoded) == net::intent_wire_status::ok &&
              decoded.size() == 1 &&
              net::intent_absolute_tick(4, received_batch.base_tick_low,
                                        decoded[0].tick_delta) == 4,
            "live proposal did not decode at its declared tick");
    net::sequence_window<uint16_t, 32> seen;
    seen.reset(0);
    require(seen.classify(decoded[0].sequence_low) == net::sequence_classification::new_value,
            "live proposal sequence was stale");
    devils_engine::act::intent accepted;
    require(fn::decode_player_intent(decoded[0], base_cells, accepted),
            "project refused the live proposal");
    require(seen.observe(decoded[0].sequence_low) == net::sequence_classification::new_value,
            "live proposal sequence did not commit");

    net::intent absent;
    require(fn::encode_player_intent(food_action(15.0, 13.0), 1, 0, base_cells, absent),
            "local-only prediction did not quantize");
    devils_engine::act::intent wrong_prediction;
    require(fn::decode_player_intent(absent, base_cells, wrong_prediction),
            "local-only prediction did not decode");

    std::vector<wire_tick> received_ticks;
    received_ticks.reserve(6);
    size_t downstream_bytes = 0;
    input_journal journal;
    for (uint64_t tick = 2; tick <= 7; ++tick) {
      const auto tag = journal.begin(tick, net::canonical_tick_max_records);
      if (tick == 4) {
        accepted_input record;
        record.tick = tick;
        record.logical_peer = logical_peer;
        record.sequence = net::widen_input_sequence_low16(0, decoded[0].sequence_low);
        record.action = accepted;
        record.payload = action_payload(accepted);
        require(journal.try_record(std::move(record)) == net::tick_record_result::recorded,
                "live input did not enter the tick journal");
      }
      require(journal.seal() == net::tick_seal_result::sealed,
              "live tick journal did not seal");
      auto sealed = journal.consume(tag);
      for (const auto& record : sealed.records())
        require(authoritative.actors.enqueue_player_intent(record.action),
                "accepted input could not enter authority world");
      step(authoritative, tick);

      std::vector<net::canonical_tick_record_view> records;
      records.reserve(sealed.records().size());
      for (const auto& record : sealed.records())
        records.push_back({record.logical_peer, record.sequence, record.payload});
      std::vector<std::byte> outgoing;
      outgoing.reserve(net::canonical_tick_max_bytes);
      require(net::try_encode_canonical_tick(tick, root_of(authoritative), records, outgoing) ==
                net::canonical_tick_wire_status::ok, "canonical tick did not encode");
      require(authority.send_canonical(logical_peer, outgoing) == fn::send_result::sent,
              "GNS refused a canonical tick");
      auto recycled = std::move(sealed).release_storage();
      journal.retire(tag);
      journal.recycle(std::move(recycled));

      if (tick == 2)
        require(predicted.actors.enqueue_player_intent(wrong_prediction),
                "local prediction could not enter follower world");
      if (tick == 4)
        require(predicted.actors.enqueue_player_intent(accepted),
                "accepted prediction could not enter follower world");
      step(predicted, tick);

      const auto bundle_by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      std::vector<std::vector<std::byte>> incoming;
      while (std::chrono::steady_clock::now() < bundle_by && incoming.empty()) {
        drive();
        incoming = follower.take_canonical_ticks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      require(incoming.size() == 1 && incoming[0] == outgoing && !follower.live_fault() &&
                !follower.live_terminal(), "canonical tick did not arrive over live GNS");
      downstream_bytes += incoming[0].size();
      received_ticks.push_back({tick, std::move(incoming[0])});
    }
    const uint64_t authoritative_root = root_of(authoritative);
    const uint64_t predicted_root = root_of(predicted);
    require(predicted_root != authoritative_root, "local divergence was not visible");

    host corrected;
    const auto restore = [&](host& target, const std::vector<std::byte>& checkpoint) {
      target.expected_root = join_root;
      return fo::load_actor_checkpoint(target.actors, target.clocks, checkpoint,
                                       follower_content.config()).loaded();
    };
    const auto apply = [&](host& target, const std::vector<std::byte>& bytes,
                           const net::replay_context context) {
      if (!context.presentation_suppressed()) return false;
      std::array<net::canonical_tick_record_view, net::canonical_tick_max_records> storage{};
      net::canonical_tick_view view;
      if (net::try_decode_canonical_tick(bytes, storage, view) !=
            net::canonical_tick_wire_status::ok || !view.state_root ||
          view.tick != target.clocks.simulation_now().value + 1) return false;
      target.pending_tick = view.tick;
      target.expected_root = *view.state_root;
      for (const auto& record : view.records) {
        if (record.origin_peer != logical_peer || record.input_sequence != 2 ||
            !enqueue_payload(target, record.payload)) return false;
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
      return target.clocks.simulation_now().value == tick && root_of(target) == target.expected_root;
    };
    const auto result = net::replay_to(corrected, uint64_t(1), checkpoint_sink.checkpoint,
      uint64_t(7), received_ticks, restore, apply, advance, verify,
      net::checked_tick_successor<uint64_t>{});
    require(result.completed() && result.replayed_ticks == 6 &&
              root_of(corrected) == authoritative_root,
            "received GNS history did not correct the predicted world");
    require(root_of(predicted) == predicted_root,
            "detached replay changed the live prediction");
    authority.close_all();
    std::printf("live GNS: %zu actors, checkpoint %u bytes, joined peer %llu at tick 1, %zu upstream bytes, 6 bundles/%zu downstream bytes, corrected root %llu\n",
                actor_count, follower.received_bytes(),
                static_cast<unsigned long long>(logical_peer), upstream.size(), downstream_bytes,
                static_cast<unsigned long long>(authoritative_root));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "live GNS tick smoke failed: %s\n", error.what());
    return 1;
  }
}
