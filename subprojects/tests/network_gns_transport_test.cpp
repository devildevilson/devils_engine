#include <devils_engine/network/gns_transport.h>
#include <steam/steamnetworkingsockets.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace net = devils_engine::network;
using namespace std::chrono_literals;

struct runtime {
  SteamNetworkingErrMsg error{};
  bool initialized = GameNetworkingSockets_Init(nullptr, error);
  ~runtime() {
    if (initialized) GameNetworkingSockets_Kill();
  }
};

struct socket_pair {
  std::array<HSteamNetConnection, 2> handles{};
  bool valid;
  explicit socket_pair(bool udp = false)
    : valid(SteamNetworkingSockets()->CreateSocketPair(&handles[0], &handles[1], udp, nullptr, nullptr)) {}
  ~socket_pair() {
    for (const auto handle : handles)
      if (handle) SteamNetworkingSockets()->CloseConnection(handle, 0, nullptr, false);
  }
  HSteamNetConnection take(std::size_t index) {
    return std::exchange(handles[index], k_HSteamNetConnection_Invalid);
  }
};

constexpr net::gns_transport_config small_config{1, 2, 64, 16, 4096};
constexpr std::array<net::gns_lane_config, 3> small_lanes{{
  {net::gns_delivery::reliable_ordered, 0, 1, 2, 32, 64},
  {net::gns_delivery::unreliable_sequenced, 0, 1, 2, 32, 64},
  {net::gns_delivery::reliable_ordered, 1, 1, 2, 32, 64},
}};

template <class Predicate>
bool wait_until(Predicate predicate, std::chrono::steady_clock::duration timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    SteamNetworkingSockets()->RunCallbacks();
    if (predicate()) return true;
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

struct fake_loss_scope {
  ~fake_loss_scope() {
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, 0);
  }
};
} // namespace

TEST_CASE("network GNS opaque lanes retain bounded sends and received leases") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  net::gns_transport receiver(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  REQUIRE(sender.ready());
  REQUIRE(receiver.ready());
  socket_pair pair;
  REQUIRE(pair.valid);
  const auto a = sender.adopt(pair.take(0));
  const auto b = receiver.adopt(pair.take(1));
  REQUIRE(a.status == net::gns_status::ok);
  REQUIRE(b.status == net::gns_status::ok);
  std::array<std::byte, 16> payload{};
  payload.fill(std::byte{17});
  for (std::uint16_t lane = 0; lane < 3; ++lane) {
    const auto result = sender.try_send(a.peer, lane, payload, 100 + lane);
    CHECK(result.status == net::gns_status::ok);
    CHECK(result.message_number_or_error > 0);
  }
  payload.fill(std::byte{99}); // No caller pointer is retained by the adapter.
  std::array<net::gns_received_message, 3> received;
  const auto first = receiver.receive(received);
  CHECK(first.status == net::gns_status::count_budget_exceeded);
  REQUIRE(first.count == 2);
  CHECK(receiver.leased_receive_count() == 2);
  std::array<net::gns_received_message, 1> extra;
  CHECK(receiver.receive(extra).status == net::gns_status::count_budget_exceeded);
  CHECK(receiver.receive(received).status == net::gns_status::output_not_empty);
  for (std::size_t i = 0; i < first.count; ++i) {
    CHECK(received[i].peer() == b.peer);
    CHECK(received[i].payload().size() == payload.size());
    CHECK(std::ranges::all_of(received[i].payload(), [](std::byte b) {
      return b == std::byte{17};
    }));
    received[i].reset();
  }
  REQUIRE(receiver.receive(extra).count == 1);
  extra[0].reset();
  std::array<net::gns_send_release, 3> releases;
  REQUIRE(sender.poll_send_releases(releases) == 3);
  for (std::uint16_t lane = 0; lane < 3; ++lane) {
    CHECK(releases[lane].peer == a.peer);
    CHECK(releases[lane].tag == 100u + lane);
    CHECK(releases[lane].bytes == 16);
    CHECK(sender.retained_send_count(lane) == 0);
  }
  CHECK(sender.poll_send_releases(releases) == 0);
  std::array<net::gns_connection_event, 1> event;
  REQUIRE(sender.poll_connections(event) == 1);
  CHECK(event[0].peer == a.peer);
  CHECK(event[0].info.m_eState == k_ESteamNetworkingConnectionState_Connected);
  CHECK(sender.poll_connections(event) == 0);
  SteamNetConnectionRealTimeStatus_t status{};
  std::array<SteamNetConnectionRealTimeLaneStatus_t, 3> lanes{};
  CHECK(sender.statistics(a.peer, status, lanes) == net::gns_status::ok);
}

TEST_CASE("network GNS lane reservations and expected refusals do not consume caller bytes") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  auto lanes = small_lanes;
  lanes[0].send_slots = 3;
  lanes[0].send_byte_budget = 20;
  net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, lanes);
  net::gns_transport receiver(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, lanes);
  socket_pair pair;
  REQUIRE(pair.valid);
  const auto raw = pair.take(0);
  const auto a = sender.adopt(raw);
  const auto b = receiver.adopt(pair.take(1));
  REQUIRE(a.status == net::gns_status::ok);
  REQUIRE(b.status == net::gns_status::ok);
  std::array<std::byte, 16> payload{};
  payload.fill(std::byte{42});
  CHECK(sender.adopt(raw).status == net::gns_status::already_owned);
  CHECK(sender.try_send(b.peer, 0, payload).status == net::gns_status::invalid_peer);
  CHECK(sender.try_send(a.peer, 3, payload).status == net::gns_status::invalid_lane);
  std::array<std::byte, 33> oversized{};
  CHECK(sender.try_send(a.peer, 0, oversized).status == net::gns_status::payload_too_large);
  REQUIRE(sender.try_send(a.peer, 2, payload).status == net::gns_status::ok);
  REQUIRE(sender.try_send(a.peer, 2, payload).status == net::gns_status::ok);
  CHECK(sender.try_send(a.peer, 2, payload).status == net::gns_status::count_budget_exceeded);
  REQUIRE(sender.try_send(a.peer, 0, payload).status == net::gns_status::ok);
  CHECK(sender.try_send(a.peer, 0, payload).status == net::gns_status::byte_budget_exceeded);
  CHECK(sender.retained_send_bytes(0) == 16);
  CHECK(std::ranges::all_of(payload, [](std::byte b) {
    return b == std::byte{42};
  }));
  // Empty application messages are legal and still consume one count slot.
  CHECK(sender.try_send(a.peer, 0, {}).status == net::gns_status::ok);
  CHECK(sender.retained_send_count(0) == 2);
}

TEST_CASE("network GNS leases and callbacks outlive adapter destruction") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  std::array<net::gns_received_message, 1> received;
  {
    net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
    net::gns_transport receiver(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
    socket_pair pair;
    REQUIRE(pair.valid);
    const auto a = sender.adopt(pair.take(0));
    const auto b = receiver.adopt(pair.take(1));
    REQUIRE(a.status == net::gns_status::ok);
    REQUIRE(b.status == net::gns_status::ok);
    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(sender.try_send(a.peer, 0, bytes).status == net::gns_status::ok);
    REQUIRE(receiver.receive(received).count == 1);
  }
  REQUIRE(received[0].payload().size() == 3);
  CHECK(received[0].payload()[2] == std::byte{3});
  // GNS permits release on any thread; neither callback nor lease can use the
  // destroyed transport. Runtime remains alive until this worker has joined.
  std::thread release_worker([message = std::move(received[0])]() mutable {
    message.reset();
  });
  release_worker.join();
  CHECK_FALSE(received[0]);
}

TEST_CASE("network GNS adoption refuses capacity and silently clamped backend limits") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  net::gns_transport transport(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  socket_pair pair;
  REQUIRE(pair.valid);
  const auto first = transport.adopt(pair.take(0));
  REQUIRE(first.status == net::gns_status::ok);
  const auto rejected_handle = pair.take(1);
  CHECK(transport.adopt(rejected_handle).status == net::gns_status::peer_capacity_exceeded);
  SteamNetConnectionInfo_t info{};
  CHECK_FALSE(SteamNetworkingSockets()->GetConnectionInfo(rejected_handle, &info));

  auto config = small_config;
  config.backend_receive_bytes = 256; // GNS clamps this to 4096; this is NOT the requested cap.
  net::gns_transport limited(*SteamNetworkingSockets(), *SteamNetworkingUtils(), config, small_lanes);
  socket_pair second;
  REQUIRE(second.valid);
  const auto clamped_handle = second.take(0);
  CHECK(limited.adopt(clamped_handle).status == net::gns_status::configuration_failed);
  CHECK_FALSE(SteamNetworkingSockets()->GetConnectionInfo(clamped_handle, &info));
}

TEST_CASE("network GNS out-of-order release reuses only observed slots across reconnect") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  net::gns_transport receiver(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  socket_pair pair;
  REQUIRE(pair.valid);
  const auto a = sender.adopt(pair.take(0));
  const auto b = receiver.adopt(pair.take(1));
  REQUIRE(a.status == net::gns_status::ok);
  REQUIRE(b.status == net::gns_status::ok);
  const std::array first{std::byte{1}}, second{std::byte{2}}, third{std::byte{3}};
  REQUIRE(sender.try_send(a.peer, 0, first, 1).status == net::gns_status::ok);
  REQUIRE(sender.try_send(a.peer, 0, second, 2).status == net::gns_status::ok);
  std::array<net::gns_received_message, 2> received;
  REQUIRE(receiver.receive(received).count == 2);
  received[1].reset(); // The first slot is still held by the receiver.
  CHECK(sender.try_send(a.peer, 0, third, 3).status == net::gns_status::count_budget_exceeded);
  std::array<net::gns_send_release, 1> release;
  REQUIRE(sender.poll_send_releases(release) == 1);
  CHECK(release[0].tag == 2);
  REQUIRE(sender.close(a.peer) == net::gns_status::ok);
  REQUIRE(receiver.close(b.peer) == net::gns_status::ok);
  socket_pair next;
  REQUIRE(next.valid);
  const auto next_a = sender.adopt(next.take(0));
  const auto next_b = receiver.adopt(next.take(1));
  REQUIRE(next_a.status == net::gns_status::ok);
  REQUIRE(next_b.status == net::gns_status::ok);
  REQUIRE(sender.try_send(next_a.peer, 0, third, 3).status == net::gns_status::ok);
  REQUIRE(receiver.receive(std::span{received}.subspan(1)).count == 1);
  CHECK(received[0].payload()[0] == std::byte{1});
  CHECK(received[0].peer() == b.peer);
  CHECK(received[1].payload()[0] == std::byte{3});
  CHECK(received[1].peer() == next_b.peer);
  received[0].reset();
  REQUIRE(sender.poll_send_releases(release) == 1);
  CHECK(release[0].tag == 1);
  CHECK(release[0].peer == a.peer); // Never relabel a late completion as the new peer.
  received[1].reset();
  REQUIRE(sender.poll_send_releases(release) == 1);
  CHECK(release[0].peer == next_a.peer);
}

TEST_CASE("network GNS reconnect generations reject stale peers and reuse slab storage") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  net::gns_transport receiver(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  net::gns_peer previous;
  const std::byte* first_storage = nullptr;
  for (std::uint64_t i = 0; i < 100; ++i) {
    socket_pair pair;
    REQUIRE(pair.valid);
    const auto a = sender.adopt(pair.take(0));
    const auto b = receiver.adopt(pair.take(1));
    REQUIRE(a.status == net::gns_status::ok);
    REQUIRE(b.status == net::gns_status::ok);
    CHECK(a.peer != previous);
    CHECK(sender.try_send(previous, 0, {}).status == net::gns_status::invalid_peer);
    const std::array bytes{std::byte(i)};
    REQUIRE(sender.try_send(a.peer, 0, bytes, i).status == net::gns_status::ok);
    std::array<net::gns_received_message, 1> received;
    REQUIRE(receiver.receive(received).count == 1);
    CHECK(received[0].message_number() == 1);
    // Internal pipe forwards the native message, exposing the actual slab.
    if (i == 0) first_storage = received[0].payload().data();
    CHECK(received[0].payload().data() == first_storage);
    CHECK(received[0].payload()[0] == std::byte(i));
    received[0].reset();
    std::array<net::gns_send_release, 1> release;
    REQUIRE(sender.poll_send_releases(release) == 1);
    CHECK(release[0].tag == i);
    CHECK(sender.close(a.peer) == net::gns_status::ok);
    CHECK(receiver.close(b.peer) == net::gns_status::ok);
    CHECK(sender.close(a.peer) == net::gns_status::invalid_peer);
    previous = a.peer;
  }
}

TEST_CASE("network GNS native send refusal releases prepared ownership immediately") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  auto lanes = small_lanes;
  lanes[0].max_payload_bytes = 128 * 1024;
  lanes[0].send_byte_budget = 256 * 1024;
  net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), {}, lanes);
  socket_pair pair(true);
  REQUIRE(pair.valid);
  const auto raw = pair.take(0);
  const auto a = sender.adopt(raw);
  REQUIRE(a.status == net::gns_status::ok);
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(raw, k_ESteamNetworkingConfig_SendBufferSize, 64 * 1024));
  const std::vector<std::byte> payload(100 * 1024, std::byte{5});
  const auto result = sender.try_send(a.peer, 0, payload);
  CHECK(result.status == net::gns_status::backend_rejected);
  CHECK(result.message_number_or_error == -k_EResultLimitExceeded);
  CHECK(sender.retained_send_count(0) == 0);
  CHECK(sender.retained_send_bytes(0) == 0);
  std::array<net::gns_send_release, 1> releases;
  CHECK(sender.poll_send_releases(releases) == 0);
  CHECK(sender.try_send(a.peer, 0, {}).status == net::gns_status::ok);
}

TEST_CASE("network GNS real UDP reliable traffic resumes after complete packet loss") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  fake_loss_scope reset;
  net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  net::gns_transport receiver(*SteamNetworkingSockets(), *SteamNetworkingUtils(), small_config, small_lanes);
  socket_pair pair(true);
  REQUIRE(pair.valid);
  const auto a = sender.adopt(pair.take(0));
  const auto b = receiver.adopt(pair.take(1));
  REQUIRE(a.status == net::gns_status::ok);
  REQUIRE(b.status == net::gns_status::ok);
  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0f));
  const std::array bytes{std::byte{81}, std::byte{82}};
  REQUIRE(sender.try_send(a.peer, 0, bytes, 7).status == net::gns_status::ok);
  std::array<net::gns_received_message, 1> received;
  CHECK_FALSE(wait_until([&] {
    return receiver.receive(received).count != 0;
  },
                         40ms));
  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f));
  REQUIRE(wait_until([&] {
    return receiver.receive(received).count == 1;
  }));
  REQUIRE(received[0].payload().size() == bytes.size());
  CHECK(std::ranges::equal(received[0].payload(), bytes));
  received[0].reset();
  std::array<net::gns_send_release, 1> releases;
  REQUIRE(wait_until([&] {
    return sender.poll_send_releases(releases) == 1;
  }));
  CHECK(releases[0].tag == 7);
  CHECK(sender.retained_send_count(0) == 0);
}

TEST_CASE("network GNS real UDP high priority message overtakes fragmented bulk") {
  runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized, runtime.error);
  auto lanes = small_lanes;
  lanes[2].max_payload_bytes = 400 * 1024;
  lanes[2].send_byte_budget = 800 * 1024;
  net::gns_transport sender(*SteamNetworkingSockets(), *SteamNetworkingUtils(), {}, lanes);
  net::gns_transport receiver(*SteamNetworkingSockets(), *SteamNetworkingUtils(), {}, lanes);
  socket_pair pair(true);
  REQUIRE(pair.valid);
  const auto raw = pair.take(0);
  const auto a = sender.adopt(raw);
  const auto b = receiver.adopt(pair.take(1));
  REQUIRE(a.status == net::gns_status::ok);
  REQUIRE(b.status == net::gns_status::ok);
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(raw, k_ESteamNetworkingConfig_SendRateMin, 512 * 1024));
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(raw, k_ESteamNetworkingConfig_SendRateMax, 512 * 1024));
  const std::vector<std::byte> bulk(400 * 1024, std::byte{47});
  const std::array urgent{std::byte{99}};
  REQUIRE(sender.try_send(a.peer, 2, bulk).status == net::gns_status::ok);
  REQUIRE(sender.try_send(a.peer, 0, urgent).status == net::gns_status::ok);
  SteamNetConnectionRealTimeStatus_t status{};
  std::array<SteamNetConnectionRealTimeLaneStatus_t, 3> lane_status{};
  REQUIRE(sender.statistics(a.peer, status, lane_status) == net::gns_status::ok);
  CHECK(lane_status[2].m_cbPendingReliable + lane_status[2].m_cbSentUnackedReliable >
        lane_status[0].m_cbPendingReliable + lane_status[0].m_cbSentUnackedReliable);
  std::array<net::gns_received_message, 1> received;
  REQUIRE(wait_until([&] {
    return receiver.receive(received).count == 1;
  }));
  CHECK(received[0].lane() == 0);
  CHECK(std::ranges::equal(received[0].payload(), urgent));
  received[0].reset();
  REQUIRE(wait_until([&] {
    return receiver.receive(received).count == 1;
  },
                     5s));
  CHECK(received[0].lane() == 2);
  CHECK(std::ranges::equal(received[0].payload(), bulk));
}
