#include "../playgrounds/NET07_replication_baselines/session.h"
#include "../playgrounds/NET06_in_memory_transport/causal_fixture.h"
#include <devils_engine/network/gns_transport.h>
#include <steam/steamnetworkingsockets.h>
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <chrono>
#include <thread>

namespace {
namespace net = devils_engine::network;
namespace fixture = net07_fixture;
namespace causal = net06_fixture;
using namespace std::chrono_literals;
constexpr auto authority = net::link_endpoint::first;
constexpr auto follower = net::link_endpoint::second;
constexpr std::size_t max_bytes = 1024, max_entries = 8;

// A laboratory protocol, not the engine's session wire format. All fields have
// explicit widths/endianness; no pointers, padding, vector layout or native enums.
struct packet {
  std::array<std::byte, max_bytes> bytes{};
  std::size_t size = 0;
};
struct packet_size {
  std::size_t operator()(const packet& p) const {
    return p.size;
  }
};

bool encode(const fixture::message& m, std::vector<std::byte>& bytes) {
  bytes.clear();
  if (m.full.size() > max_entries || m.changes.size() > max_entries) return false;
  net::state_writer w(bytes, false);
  w.u32(1); // Envelope version, distinct from the state format.
  w.u32(std::uint32_t(m.kind));
  w.u32(m.header.format_version);
  w.u64(m.header.server_tick);
  w.u32(m.header.state_sequence);
  w.u32(m.header.base_baseline.has_value());
  w.u64(m.header.base_baseline.value_or(0));
  w.u64(m.header.result_baseline);
  w.u64(m.header.acknowledged_input_sequence);
  w.u64(m.requested_baseline);
  w.u64(m.recovery_token);
  w.u32(std::uint32_t(m.full.size()));
  for (const auto& e : m.full) {
    w.u32(e.key);
    w.u32(e.version);
    w.u32(std::bit_cast<std::uint32_t>(e.value.x));
    w.u32(std::bit_cast<std::uint32_t>(e.value.health));
  }
  w.u32(std::uint32_t(m.changes.size()));
  for (const auto& e : m.changes) {
    w.u32(e.key);
    w.u32(e.expected_version.has_value());
    w.u32(e.expected_version.value_or(0));
    w.u32(e.result.has_value());
    if (e.result) {
      w.u32(e.result->version);
      w.u32(std::bit_cast<std::uint32_t>(e.result->value.x));
      w.u32(std::bit_cast<std::uint32_t>(e.result->value.health));
    }
  }
  return w.good();
}

bool decode(std::span<const std::byte> bytes, fixture::message& m) {
  if (bytes.size() > max_bytes) return false;
  net::state_reader r(bytes);
  if (r.u32() != 1) return false;
  const auto kind = r.u32();
  if (kind > std::uint32_t(fixture::message_kind::baseline_request)) return false;
  m.kind = fixture::message_kind(kind);
  m.header.format_version = r.u32();
  m.header.server_tick = r.u64();
  const auto sequence = r.u32(), has_base = r.u32();
  if (sequence > UINT16_MAX || has_base > 1) return false;
  m.header.state_sequence = std::uint16_t(sequence);
  const auto base = r.u64();
  if (!has_base && base != 0) return false;
  m.header.base_baseline = has_base ? std::optional(base) : std::nullopt;
  m.header.result_baseline = r.u64();
  m.header.acknowledged_input_sequence = r.u64();
  m.requested_baseline = r.u64();
  m.recovery_token = r.u64();
  const auto full_count = r.u32();
  if (!r.good() || full_count > max_entries || full_count > m.full.capacity()) return false;
  m.full.resize(full_count);
  for (auto& e : m.full) {
    e.key = r.u32();
    e.version = r.u32();
    e.value.x = std::bit_cast<std::int32_t>(r.u32());
    e.value.health = std::bit_cast<std::int32_t>(r.u32());
  }
  const auto delta_count = r.u32();
  if (!r.good() || delta_count > max_entries || delta_count > m.changes.capacity()) return false;
  m.changes.resize(delta_count);
  for (auto& e : m.changes) {
    e.key = r.u32();
    const auto has_version = r.u32(), version = r.u32(), has_result = r.u32();
    if (has_version > 1 || has_result > 1 || (!has_version && version != 0)) return false;
    e.expected_version = has_version ? std::optional(version) : std::nullopt;
    e.result.reset();
    if (has_result) {
      e.result.emplace();
      e.result->version = r.u32();
      e.result->value.x = std::bit_cast<std::int32_t>(r.u32());
      e.result->value.health = std::bit_cast<std::int32_t>(r.u32());
    }
  }
  if ((m.kind == fixture::message_kind::full_baseline && delta_count != 0) ||
      (m.kind == fixture::message_kind::delta && full_count != 0) ||
      (m.kind == fixture::message_kind::baseline_request && (full_count || delta_count))) return false;
  m.wire_bytes = bytes.size();
  return r.good() && r.position() == r.size();
}

struct first_attempt_loss {
  net::link_fault_effect operator()(const net::link_transmission& t) const {
    return {.drop = t.options.reliability == net::link_reliability::reliable_ordered && t.attempt == 0};
  }
};

struct memory_backend {
  net::in_memory_link<packet, packet_size, first_attempt_loss> link{{64, 65536, 65536, 1, 1, 256}};
  memory_backend() {
    link.connect();
  }
  void pump() {
    link.advance();
  }
  net::link_send_status send(net::link_endpoint source, net::link_send_options options,
                             std::span<const std::byte> bytes) {
    if (bytes.size() > max_bytes) return net::link_send_status::byte_budget_exceeded;
    packet p;
    p.size = bytes.size();
    std::ranges::copy(bytes, p.bytes.begin());
    return link.try_send(source, options, std::move(p));
  }
  template <class Consumer>
  void consume(net::link_endpoint target, Consumer&& consumer) {
    link.consume(target, [&](const auto& delivered) {
      consumer(std::span(delivered.message.bytes).first(delivered.message.size));
    });
  }
};

struct runtime {
  SteamNetworkingErrMsg error{};
  bool initialized = GameNetworkingSockets_Init(nullptr, error);
  ~runtime() {
    if (initialized) GameNetworkingSockets_Kill();
  }
};

constexpr net::gns_transport_config config{1, 4, max_bytes, 64, 65536};
constexpr std::array<net::gns_lane_config, 3> lanes{{
  {net::gns_delivery::reliable_ordered, 0, 1, 4, max_bytes, 4 * max_bytes},
  {net::gns_delivery::reliable_ordered, 1, 1, 4, max_bytes, 4 * max_bytes},
  {net::gns_delivery::unreliable_sequenced, 0, 1, 4, max_bytes, 4 * max_bytes},
}};

struct gns_backend {
  net::gns_dispatcher dispatcher{*SteamNetworkingSockets(), *SteamNetworkingUtils(), 2};
  net::gns_transport a{dispatcher, config, lanes}, b{dispatcher, config, lanes};
  net::gns_peer pa, pb;
  bool reclaim = true;
  bool outage = false;
  std::chrono::steady_clock::time_point outage_end;
  gns_backend() {
    SteamNetworkingConfigValue_t option;
    option.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
    SteamNetworkingIPAddr address{};
    address.SetIPv4(0x7f000001, 0);
    net::gns_listen_result listening;
    for (std::uint16_t port = 40300; port < 40556; ++port) {
      address.m_port = port;
      listening = a.listen(address, {&option, 1});
      if (listening.status != net::gns_status::backend_rejected) break;
    }
    REQUIRE(listening.status == net::gns_status::ok);
    const auto connected = b.connect(address, {&option, 1});
    REQUIRE(connected.status == net::gns_status::ok);
    pb = connected.peer;
    bool a_ready = false, b_ready = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while ((!a_ready || !b_ready) && std::chrono::steady_clock::now() < deadline) {
      dispatcher.pump();
      net::gns_connection_event e;
      if (a.poll_connections({&e, 1})) {
        pa = e.peer;
        if (e.needs_accept) REQUIRE(a.accept(pa) == net::gns_status::ok);
        a_ready = e.info.m_eState == k_ESteamNetworkingConnectionState_Connected;
      }
      if (b.poll_connections({&e, 1})) b_ready = e.info.m_eState == k_ESteamNetworkingConnectionState_Connected;
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(a_ready);
    REQUIRE(b_ready);
  }
  ~gns_backend() {
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0);
  }
  void start_outage() {
    REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 100));
    outage = true;
    outage_end = std::chrono::steady_clock::now() + 40ms;
  }
  void pump() {
    if (outage && std::chrono::steady_clock::now() >= outage_end) {
      SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0);
      outage = false;
    }
    dispatcher.pump();
    if (reclaim) {
      std::array<net::gns_send_release, 12> completed;
      a.poll_send_releases(completed);
      b.poll_send_releases(completed);
    }
    std::this_thread::sleep_for(1ms);
  }
  net::link_send_status send(net::link_endpoint source, net::link_send_options options,
                             std::span<const std::byte> bytes) {
    const auto result = source == authority ? a.try_send(pa, options.lane, bytes) : b.try_send(pb, options.lane, bytes);
    if (result.status == net::gns_status::ok) return net::link_send_status::accepted;
    if (result.status == net::gns_status::count_budget_exceeded) return net::link_send_status::count_budget_exceeded;
    if (result.status == net::gns_status::byte_budget_exceeded) return net::link_send_status::byte_budget_exceeded;
    return net::link_send_status::disconnected;
  }
  template <class Consumer>
  void consume(net::link_endpoint target, Consumer&& consumer) {
    auto& transport = target == authority ? a : b;
    for (unsigned work = 0; work < 16; ++work) {
      std::array<net::gns_received_message, 1> input;
      const auto result = transport.receive(input);
      REQUIRE(result.status == net::gns_status::ok);
      if (!result.count) break;
      REQUIRE(input[0].peer() == (target == authority ? pa : pb));
      consumer(input[0].payload());
    }
  }
};

template <class Backend>
struct message_link {
  Backend& backend;
  std::vector<std::byte> scratch;
  fixture::message received;
  explicit message_link(Backend& value) : backend(value) {
    scratch.reserve(max_bytes);
    received.full.reserve(max_entries);
    received.changes.reserve(max_entries);
  }
  net::link_send_status try_send(net::link_endpoint source, net::link_send_options options, const fixture::message& m) {
    if (!encode(m, scratch)) return net::link_send_status::byte_budget_exceeded;
    return backend.send(source, options, scratch);
  }
  template <class Consumer>
  void consume(net::link_endpoint target, Consumer&& consumer) {
    backend.consume(target, [&](auto bytes) {
      REQUIRE(decode(bytes, received));
      struct delivery {
        const fixture::message& message;
      } value{received};
      consumer(value);
    });
  }
};

template <class Pump, class Predicate>
void until(Pump&& pump, Predicate&& done) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!done() && std::chrono::steady_clock::now() < deadline)
    pump();
  REQUIRE(done());
}

// Identical NET07 receiver, messages, entity create/update/delete and recovery
// assertions on both transports. Scripted loss/reordering below is explicitly
// at the logical-message boundary, not a claim about native packet scheduling.
template <class Backend>
void replication_session(Backend& backend) {
  message_link link(backend);
  fixture::verifier verify;
  fixture::follower replica;
  const fixture::snapshot s100{{1, 1, {0, 100}}, {2, 1, {10, 80}}};
  const fixture::snapshot s101{{1, 2, {1, 95}}, {2, 1, {10, 80}}, {3, 1, {-5, 50}}};
  const fixture::snapshot s102{{1, 2, {1, 95}}, {3, 2, {-4, 45}}};
  const fixture::snapshot s103{{1, 3, {2, 90}}, {3, 2, {-4, 45}}, {4, 1, {20, 100}}};
  const fixture::snapshot s104{{1, 3, {2, 90}}, {4, 2, {19, 70}}};
  const fixture::snapshot s105{{1, 4, {3, 88}}, {4, 2, {19, 70}}};
  const fixture::snapshot* current = &s100;
  std::uint64_t tick = 100;
  std::uint16_t sequence = 1;
  unsigned requests = 0;
  const auto send = [&](const auto& m, auto options) {
    REQUIRE(link.try_send(authority, options, m) == net::link_send_status::accepted);
  };
  const auto pump = [&] {
    backend.pump();
    link.consume(follower, [&](const auto& d) {
      replica.receive(d.message, link, verify);
    });
    link.consume(authority, [&](const auto& d) {
      REQUIRE(d.message.kind == fixture::message_kind::baseline_request);
      ++requests;
      auto response = fixture::make_full(sequence++, tick, tick, *current);
      response.recovery_token = d.message.recovery_token;
      send(response, fixture::reliable(fixture::baseline_lane));
    });
  };
  const auto converged = [&] {
    return replica.current() && *replica.current() == *current;
  };
  send(fixture::make_full(sequence++, tick, tick, *current), fixture::reliable(1));
  until(pump, converged);
  // Lost 100->101 consumes an application sequence, but is deliberately not sent.
  ++sequence;
  current = &s102;
  tick = 102;
  send(fixture::make_delta_frame(sequence++, 102, 101, 102, fixture::make_delta(verify, s101, s102)), fixture::unreliable(2));
  until(pump, converged);
  auto delayed = fixture::make_delta_frame(sequence++, 103, 102, 103, fixture::make_delta(verify, s102, s103));
  current = &s104;
  tick = 104;
  send(fixture::make_delta_frame(sequence++, 104, 103, 104, fixture::make_delta(verify, s103, s104)), fixture::unreliable(2));
  until(pump, converged);
  send(delayed, fixture::unreliable(2));
  until(pump, [&] {
    return replica.stale == 1;
  });
  current = &s105;
  tick = 105;
  const auto final = fixture::make_delta_frame(sequence++, 105, 104, 105, fixture::make_delta(verify, s104, s105));
  send(final, fixture::unreliable(2));
  until(pump, converged);
  send(final, fixture::unreliable(2));
  until(pump, [&] {
    return replica.duplicate == 1;
  });
  CHECK(replica.applied_full == 3);
  CHECK(replica.applied_delta == 1);
  CHECK(replica.missing_base == 2);
  CHECK(requests == 2);
  CHECK(replica.current_baseline == 105);
  CHECK(replica.baselines.retained_count() <= 4);
  CHECK(replica.baselines.retained_bytes() <= 4096);
  CHECK(replica.rejected == 0);
}

template <class Backend>
void causal_replay_session(Backend& backend) {
  std::vector<std::byte> wire;
  wire.reserve(8);
  net::state_writer writer(wire, false);
  writer.u32(2);
  writer.u32(std::bit_cast<std::uint32_t>(2.25f));
  REQUIRE(writer.good());
  REQUIRE(backend.send(authority, fixture::reliable(0), wire) == net::link_send_status::accepted);

  net::checkpoint_ring<unsigned, std::vector<std::byte>, causal::blob_size> checkpoints(2, 128);
  std::vector<std::byte> checkpoint, scratch;
  checkpoint.reserve(64);
  scratch.reserve(16);
  causal::causal_host reference, predicted;
  REQUIRE(causal::causal_schema::try_write(reference, checkpoint, scratch));
  REQUIRE(checkpoints.try_store(0, std::move(checkpoint)).stored());
  checkpoint.reserve(64);
  struct input {
    unsigned tick;
    float bundle;
  };
  std::array<input, 6> history{};
  const auto integrate = [](causal::causal_host& host, const unsigned tick,
                            const bool presentation) {
    host.state.x += host.state.velocity * 0.1f;
    host.state.tick = tick;
    if (presentation) ++host.presentation;
  };
  for (unsigned tick = 1; tick <= history.size(); ++tick) {
    const float actual = tick == 2 ? 2.25f : 1.5f;
    history[tick - 1] = {tick, 1.5f};
    reference.state.velocity = actual;
    predicted.state.velocity = 1.5f;
    integrate(reference, tick, true);
    integrate(predicted, tick, true);
  }

  net::state_digest_report<std::uint64_t> expected, before, after;
  expected.sections.reserve(1);
  before.sections.reserve(1);
  after.sections.reserve(1);
  REQUIRE(causal::causal_schema::try_write(reference, checkpoint, scratch));
  REQUIRE(net::try_murmur64_digest<causal::causal_schema>(checkpoint, expected) ==
          net::state_digest_build_status::built);
  REQUIRE(causal::causal_schema::try_write(predicted, checkpoint, scratch));
  REQUIRE(net::try_murmur64_digest<causal::causal_schema>(checkpoint, before) ==
          net::state_digest_build_status::built);
  CHECK_FALSE(net::compare_state_digests(expected, before).matched());

  bool received = false;
  net::replay_result<unsigned> replayed;
  until([&] {
    backend.pump();
    backend.consume(follower, [&](const std::span<const std::byte> bytes) {
      net::state_reader reader(bytes);
      const unsigned tick = reader.u32();
      const float velocity = std::bit_cast<float>(reader.u32());
      REQUIRE(reader.good());
      REQUIRE(reader.position() == reader.size());
      REQUIRE(tick > 0);
      REQUIRE(tick <= history.size());
      received = true;
      history[tick - 1].bundle = velocity;
      const auto* saved = checkpoints.latest_at_or_before(tick - 1);
      REQUIRE(saved != nullptr);
      causal::causal_host staging = predicted;
      bool suppressed = true;
      replayed = net::replay_to(
        staging, saved->tick, saved->bundle, unsigned(history.size()), history,
        [](causal::causal_host& host, const std::vector<std::byte>& bytes) {
          net::state_reader checkpoint_reader(bytes);
          return causal::causal_schema::load(
                   host, checkpoint_reader, causal::causal_state{},
                   [](const causal::causal_state&) {
                     return true;
                   },
                   [](causal::causal_host& live, causal::causal_state&& state) noexcept {
                     live.state = state;
                   })
            .loaded();
        },
        [&](causal::causal_host& host, const float value, const net::replay_context context) {
          suppressed &= context.presentation_suppressed();
          host.state.velocity = value;
          return true;
        },
        [&](causal::causal_host& host, const unsigned replay_tick,
            const net::replay_context context) {
          suppressed &= context.presentation_suppressed();
          integrate(host, replay_tick, !context.presentation_suppressed());
          return true;
        },
        [](const causal::causal_host&, unsigned) {
          return true;
        },
        net::checked_tick_successor<unsigned>{});
      CHECK(suppressed);
      if (replayed.completed()) predicted = staging;
    });
  },
        [&] {
          return received;
        });
  REQUIRE(replayed.completed());
  CHECK(replayed.replayed_ticks == history.size());
  CHECK(predicted.presentation == history.size());
  REQUIRE(causal::causal_schema::try_write(predicted, checkpoint, scratch));
  REQUIRE(net::try_murmur64_digest<causal::causal_schema>(checkpoint, after) ==
          net::state_digest_build_status::built);
  CHECK(net::compare_state_digests(expected, after).matched());
}

TEST_CASE("network shared NET07 session converges over canonical memory and GNS messages") {
  SUBCASE("in memory with reliable retry") {
    memory_backend backend;
    replication_session(backend);
  }
  SUBCASE("real UDP with initial complete outage") {
    runtime runtime;
    REQUIRE_MESSAGE(runtime.initialized, runtime.error);
    gns_backend backend;
    backend.start_outage();
    replication_session(backend);
    CHECK_FALSE(backend.outage);
  }
}

TEST_CASE("network shared NET06 late intent restores checkpoint replays and matches digest") {
  SUBCASE("in memory after delayed consumption") {
    memory_backend backend;
    causal_replay_session(backend);
  }
  SUBCASE("real UDP after complete initial outage") {
    runtime runtime;
    REQUIRE_MESSAGE(runtime.initialized, runtime.error);
    gns_backend backend;
    backend.start_outage();
    causal_replay_session(backend);
    CHECK_FALSE(backend.outage);
  }
}

TEST_CASE("network session wire codec refuses malformed input using prepared scratch") {
  fixture::message decoded;
  decoded.full.reserve(max_entries);
  decoded.changes.reserve(max_entries);
  const fixture::snapshot base{{1, 1, {-1, 100}}}, next{{1, 2, {3, 90}}, {2, 1, {4, 50}}};
  fixture::verifier verify;
  std::vector<std::byte> bytes;
  bytes.reserve(max_bytes);
  for (const auto& m : {fixture::make_full(1, 1, 1, base),
                        fixture::make_delta_frame(2, 2, 1, 2, fixture::make_delta(verify, base, next)), fixture::make_request(3)}) {
    REQUIRE(encode(m, bytes));
    REQUIRE(decode(bytes, decoded));
    CHECK(decoded.kind == m.kind);
    CHECK(decoded.header == m.header);
    CHECK(decoded.full == m.full);
    CHECK(decoded.changes == m.changes);
    CHECK(decoded.requested_baseline == m.requested_baseline);
    for (std::size_t size = 0; size < bytes.size(); ++size)
      CHECK_FALSE(decode(std::span(bytes).first(size), decoded));
    bytes.push_back(std::byte{0});
    CHECK_FALSE(decode(bytes, decoded));
    bytes.pop_back();
    bytes[0] = std::byte{99};
    CHECK_FALSE(decode(bytes, decoded));
  }
  auto too_many = fixture::make_full(1, 1, 1, base);
  too_many.full.resize(max_entries + 1);
  CHECK_FALSE(encode(too_many, bytes));
}

TEST_CASE("network GNS session lane pressure retains completion ownership without blocking control") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  gns_backend backend;
  backend.reclaim = false;
  message_link link(backend);
  const auto bulk = fixture::make_full(1, 1, 1, {{1, 1, {0, 100}}});
  for (unsigned i = 0; i < 4; ++i)
    REQUIRE(link.try_send(authority, fixture::reliable(1), bulk) == net::link_send_status::accepted);
  CHECK(link.try_send(authority, fixture::reliable(1), bulk) == net::link_send_status::count_budget_exceeded);
  unsigned controls = 0, snapshots = 0;
  REQUIRE(link.try_send(authority, fixture::reliable(0), fixture::make_request(1)) == net::link_send_status::accepted);
  until([&] {
    backend.pump();
    link.consume(follower, [&](const auto& d) {
      if (d.message.kind == fixture::message_kind::baseline_request)
        ++controls;
      else
        ++snapshots;
    });
  },
        [&] {
          return controls == 1 && snapshots == 4;
        });
  CHECK(backend.a.retained_send_count(1) == 4);
  CHECK(link.try_send(authority, fixture::reliable(1), bulk) == net::link_send_status::count_budget_exceeded);
  backend.reclaim = true;
  until([&] {
    backend.pump();
  },
        [&] {
          return backend.a.retained_send_count(1) == 0;
        });
  CHECK(link.try_send(authority, fixture::reliable(1), bulk) == net::link_send_status::accepted);
}
} // namespace
