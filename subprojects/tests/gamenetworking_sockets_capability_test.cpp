#include <doctest/doctest.h>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class gns_runtime {
public:
  explicit gns_runtime(const SteamNetworkingIdentity* identity = nullptr) {
    SteamNetworkingErrMsg error{};
    initialized_ = GameNetworkingSockets_Init(identity, error);
    std::memcpy(error_.data(), error, error_.size());
    error_.back() = '\0';
  }

  ~gns_runtime() {
    if (initialized_) GameNetworkingSockets_Kill();
  }

  gns_runtime(const gns_runtime&) = delete;
  gns_runtime& operator=(const gns_runtime&) = delete;

  [[nodiscard]] bool initialized() const noexcept {
    return initialized_;
  }
  [[nodiscard]] const char* error() const noexcept {
    return error_.data();
  }

private:
  bool initialized_ = false;
  std::array<char, sizeof(SteamNetworkingErrMsg)> error_{};
};

class connection_pair {
public:
  explicit connection_pair(const bool network_loopback) {
    valid_ = SteamNetworkingSockets()->CreateSocketPair(
      &sender_, &receiver_, network_loopback, nullptr, nullptr);
  }

  ~connection_pair() {
    if (sender_ != k_HSteamNetConnection_Invalid) {
      SteamNetworkingSockets()->CloseConnection(sender_, 0, nullptr, false);
    }
    if (receiver_ != k_HSteamNetConnection_Invalid) {
      SteamNetworkingSockets()->CloseConnection(receiver_, 0, nullptr, false);
    }
  }

  connection_pair(const connection_pair&) = delete;
  connection_pair& operator=(const connection_pair&) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return valid_;
  }
  [[nodiscard]] HSteamNetConnection sender() const noexcept {
    return sender_;
  }
  [[nodiscard]] HSteamNetConnection receiver() const noexcept {
    return receiver_;
  }

private:
  HSteamNetConnection sender_ = k_HSteamNetConnection_Invalid;
  HSteamNetConnection receiver_ = k_HSteamNetConnection_Invalid;
  bool valid_ = false;
};

class poll_group {
public:
  poll_group() : handle_(SteamNetworkingSockets()->CreatePollGroup()) {}

  ~poll_group() {
    if (handle_ != k_HSteamNetPollGroup_Invalid) {
      SteamNetworkingSockets()->DestroyPollGroup(handle_);
    }
  }

  poll_group(const poll_group&) = delete;
  poll_group& operator=(const poll_group&) = delete;

  [[nodiscard]] HSteamNetPollGroup handle() const noexcept {
    return handle_;
  }

private:
  HSteamNetPollGroup handle_ = k_HSteamNetPollGroup_Invalid;
};

struct opaque_message {
  std::uint32_t magic = 0x4e455430u;
  std::uint32_t sequence = 0;
  std::array<std::byte, 37> body{};
};

[[nodiscard]] opaque_message make_message(const std::uint32_t sequence) {
  opaque_message result{};
  result.sequence = sequence;
  for (std::size_t i = 0; i < result.body.size(); ++i) {
    result.body[i] = static_cast<std::byte>((sequence * 31u + i * 17u) & 0xffu);
  }
  return result;
}

[[nodiscard]] bool same_bytes(const SteamNetworkingMessage_t& message, const opaque_message& expected) {
  return message.m_cbSize == static_cast<int>(sizeof(expected)) &&
         std::memcmp(message.m_pData, &expected, sizeof(expected)) == 0;
}

[[nodiscard]] std::vector<std::byte> make_payload(const std::size_t byte_count, const std::uint32_t seed) {
  std::vector<std::byte> result(byte_count);
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<std::byte>((seed + i * 131u + i / 7u) & 0xffu);
  }
  return result;
}

[[nodiscard]] bool wait_for_messages(
  const HSteamNetConnection receiver,
  std::vector<SteamNetworkingMessage_t*>& messages,
  const std::size_t expected_count,
  const std::chrono::steady_clock::duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (messages.size() < expected_count && std::chrono::steady_clock::now() < deadline) {
    SteamNetworkingSockets()->RunCallbacks();

    std::array<SteamNetworkingMessage_t*, 8> batch{};
    const int received = SteamNetworkingSockets()->ReceiveMessagesOnConnection(
      receiver, batch.data(), static_cast<int>(batch.size()));
    if (received < 0) return false;
    messages.insert(messages.end(), batch.begin(), batch.begin() + received);

    if (messages.size() < expected_count) std::this_thread::sleep_for(1ms);
  }
  return messages.size() == expected_count;
}

[[nodiscard]] SteamNetworkingMessage_t* wait_for_one(
  const HSteamNetConnection receiver,
  const std::chrono::steady_clock::duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    SteamNetworkingSockets()->RunCallbacks();
    SteamNetworkingMessage_t* message = nullptr;
    const int received = SteamNetworkingSockets()->ReceiveMessagesOnConnection(receiver, &message, 1);
    if (received == 1) return message;
    if (received < 0) return nullptr;
    std::this_thread::sleep_for(1ms);
  }
  return nullptr;
}

void release_all(const std::span<SteamNetworkingMessage_t*> messages) {
  for (SteamNetworkingMessage_t* message : messages) {
    if (message != nullptr) message->Release();
  }
}

std::atomic_uint32_t custom_buffer_releases = 0;

void release_custom_buffer(SteamNetworkingMessage_t* message) {
  delete[] static_cast<std::byte*>(message->m_pData);
  custom_buffer_releases.fetch_add(1, std::memory_order_relaxed);
}

class fake_network_reset {
public:
  ~fake_network_reset() {
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, 0);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, 0);
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f);
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0.0f);
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send, 0.0f);
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Recv, 0.0f);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, 0);
  }

  fake_network_reset(const fake_network_reset&) = delete;
  fake_network_reset& operator=(const fake_network_reset&) = delete;
  fake_network_reset() = default;
};

struct connection_callback_context {
  HSteamListenSocket listen_socket = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup poll_group = k_HSteamNetPollGroup_Invalid;
  std::vector<HSteamNetConnection> accepted_connections;
  std::vector<HSteamNetConnection> terminal_connections;
  std::vector<EResult> accept_results;
  bool poll_group_assignment_failed = false;

  void on_status_changed(const SteamNetConnectionStatusChangedCallback_t& event) {
    if (event.m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
        event.m_info.m_hListenSocket == listen_socket) {
      accepted_connections.push_back(event.m_hConn);
      const EResult result = SteamNetworkingSockets()->AcceptConnection(event.m_hConn);
      accept_results.push_back(result);
      if (result == k_EResultOK && poll_group != k_HSteamNetPollGroup_Invalid &&
          !SteamNetworkingSockets()->SetConnectionPollGroup(event.m_hConn, poll_group)) {
        poll_group_assignment_failed = true;
      }
    }

    if (event.m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        event.m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
      terminal_connections.push_back(event.m_hConn);
    }
  }
};

connection_callback_context* active_connection_callback = nullptr;

void connection_status_changed(SteamNetConnectionStatusChangedCallback_t* event) {
  if (active_connection_callback != nullptr && event != nullptr) {
    active_connection_callback->on_status_changed(*event);
  }
}

class connection_callback_scope {
public:
  explicit connection_callback_scope(connection_callback_context& context) {
    active_connection_callback = &context;
    installed_ = SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
      &connection_status_changed);
  }

  ~connection_callback_scope() {
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(nullptr);
    active_connection_callback = nullptr;
  }

  connection_callback_scope(const connection_callback_scope&) = delete;
  connection_callback_scope& operator=(const connection_callback_scope&) = delete;

  [[nodiscard]] bool installed() const noexcept {
    return installed_;
  }

private:
  bool installed_ = false;
};

[[nodiscard]] bool connection_is_connected(const HSteamNetConnection connection) {
  SteamNetConnectionInfo_t info{};
  return SteamNetworkingSockets()->GetConnectionInfo(connection, &info) &&
         info.m_eState == k_ESteamNetworkingConnectionState_Connected;
}

template <class Predicate>
[[nodiscard]] bool pump_until(Predicate&& predicate, const std::chrono::steady_clock::duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    SteamNetworkingSockets()->RunCallbacks();
    if (predicate()) return true;
    std::this_thread::sleep_for(1ms);
  }
  SteamNetworkingSockets()->RunCallbacks();
  return predicate();
}

enum class load_message_kind : std::uint32_t {
  intent = 1,
  state = 2,
  bulk = 3,
};

struct load_message_header {
  std::uint32_t magic = 0x4c4f4144u;
  load_message_kind kind = load_message_kind::intent;
  std::uint32_t sequence = 0;
  std::uint32_t size = 0;
};

void fill_load_message(
  const std::span<std::byte> bytes,
  const load_message_kind kind,
  const std::uint32_t sequence) {
  const load_message_header header{0x4c4f4144u, kind, sequence, static_cast<std::uint32_t>(bytes.size())};
  std::memcpy(bytes.data(), &header, sizeof(header));
  for (std::size_t i = sizeof(header); i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>(
      (static_cast<std::uint32_t>(kind) * 43u + sequence * 17u + i * 29u) & 0xffu);
  }
}

[[nodiscard]] bool valid_load_message(const SteamNetworkingMessage_t& message, load_message_header& header) {
  if (message.m_cbSize < static_cast<int>(sizeof(header))) return false;
  std::memcpy(&header, message.m_pData, sizeof(header));
  if (header.magic != 0x4c4f4144u || header.size != static_cast<std::uint32_t>(message.m_cbSize)) return false;

  const auto* bytes = static_cast<const std::byte*>(message.m_pData);
  for (std::size_t i = sizeof(header); i < header.size; ++i) {
    const auto expected = static_cast<std::byte>(
      (static_cast<std::uint32_t>(header.kind) * 43u + header.sequence * 17u + i * 29u) & 0xffu);
    if (bytes[i] != expected) return false;
  }
  return true;
}

[[nodiscard]] EResult queue_load_message(
  const HSteamNetConnection connection,
  const load_message_kind kind,
  const std::uint32_t sequence,
  const std::size_t size,
  const std::uint16_t lane,
  const bool reliable) {
  if (size < sizeof(load_message_header) || size > static_cast<std::size_t>(INT32_MAX)) {
    return k_EResultInvalidParam;
  }

  SteamNetworkingMessage_t* message = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(size));
  if (message == nullptr) return k_EResultFail;
  fill_load_message({static_cast<std::byte*>(message->m_pData), size}, kind, sequence);
  message->m_conn = connection;
  message->m_nFlags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable) |
                      k_nSteamNetworkingSend_NoNagle;
  message->m_idxLane = lane;

  int64 result = 0;
  SteamNetworkingSockets()->SendMessages(1, &message, &result, false);
  if (result > 0) return k_EResultOK;
  if (message != nullptr) message->Release();
  return result < 0 ? static_cast<EResult>(-result) : k_EResultFail;
}

struct load_observations {
  std::vector<std::uint32_t> intents;
  std::vector<std::uint32_t> states;
  std::vector<std::uint32_t> bulk;
  bool corrupt = false;
};

[[nodiscard]] bool drain_load_messages(
  const HSteamNetConnection receiver,
  load_observations& observations) {
  std::array<SteamNetworkingMessage_t*, 64> messages{};
  for (;;) {
    const int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(
      receiver, messages.data(), static_cast<int>(messages.size()));
    if (count < 0) return false;
    if (count == 0) return true;

    for (int i = 0; i < count; ++i) {
      SteamNetworkingMessage_t* message = messages[static_cast<std::size_t>(i)];
      load_message_header header{};
      if (!valid_load_message(*message, header)) {
        observations.corrupt = true;
      } else {
        switch (header.kind) {
          case load_message_kind::intent: observations.intents.push_back(header.sequence); break;
          case load_message_kind::state: observations.states.push_back(header.sequence); break;
          case load_message_kind::bulk: observations.bulk.push_back(header.sequence); break;
          default: observations.corrupt = true; break;
        }
      }
      message->Release();
    }
  }
}

} // namespace

TEST_CASE("GNS internal pipe preserves opaque messages, lanes, poll groups and ownership") {
  gns_runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());
  REQUIRE(SteamNetworkingSockets() != nullptr);
  REQUIRE(SteamNetworkingUtils() != nullptr);

  poll_group group;
  REQUIRE(group.handle() != k_HSteamNetPollGroup_Invalid);

  connection_pair pair(false);
  REQUIRE(pair.valid());
  REQUIRE(SteamNetworkingSockets()->SetConnectionPollGroup(pair.receiver(), group.handle()));

  constexpr std::array<int, 3> priorities{0, 1, 1};
  constexpr std::array<std::uint16_t, 3> weights{1, 3, 1};
  REQUIRE(
    SteamNetworkingSockets()->ConfigureConnectionLanes(
      pair.sender(), static_cast<int>(priorities.size()), priorities.data(), weights.data()) == k_EResultOK);

  constexpr std::array<std::uint16_t, 4> lanes{0, 0, 1, 2};
  constexpr std::array<int, 4> flags{
    k_nSteamNetworkingSend_Reliable,
    k_nSteamNetworkingSend_Reliable,
    k_nSteamNetworkingSend_Unreliable,
    k_nSteamNetworkingSend_Reliable,
  };

  std::array<opaque_message, lanes.size()> expected{};
  std::array<void*, lanes.size()> submitted_payloads{};
  std::array<SteamNetworkingMessage_t*, lanes.size()> outgoing{};
  for (std::size_t i = 0; i < outgoing.size(); ++i) {
    expected[i] = make_message(static_cast<std::uint32_t>(i + 1));
    outgoing[i] = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(sizeof(opaque_message)));
    REQUIRE(outgoing[i] != nullptr);
    std::memcpy(outgoing[i]->m_pData, &expected[i], sizeof(opaque_message));
    submitted_payloads[i] = outgoing[i]->m_pData;
    outgoing[i]->m_conn = pair.sender();
    outgoing[i]->m_nFlags = flags[i];
    outgoing[i]->m_idxLane = lanes[i];
  }

  std::array<int64, lanes.size()> send_results{};
  SteamNetworkingSockets()->SendMessages(
    static_cast<int>(outgoing.size()), outgoing.data(), send_results.data(), false);
  for (std::size_t i = 0; i < outgoing.size(); ++i) {
    CAPTURE(i);
    CAPTURE(send_results[i]);
    CHECK(send_results[i] > 0);
    CHECK(outgoing[i] == nullptr);
  }

  std::array<SteamNetworkingMessage_t*, lanes.size()> received{};
  const int received_count = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(
    group.handle(), received.data(), static_cast<int>(received.size()));
  REQUIRE(received_count == static_cast<int>(received.size()));

  std::array<bool, lanes.size()> seen{};
  std::uint32_t previous_lane_zero_sequence = 0;
  for (SteamNetworkingMessage_t* message : received) {
    REQUIRE(message != nullptr);
    REQUIRE(message->m_cbSize == static_cast<int>(sizeof(opaque_message)));
    REQUIRE(message->m_conn == pair.receiver());

    opaque_message decoded{};
    std::memcpy(&decoded, message->m_pData, sizeof(decoded));
    REQUIRE(decoded.magic == opaque_message{}.magic);
    REQUIRE(decoded.sequence >= 1);
    REQUIRE(decoded.sequence <= expected.size());
    const std::size_t index = decoded.sequence - 1;
    CHECK_FALSE(seen[index]);
    seen[index] = true;
    CHECK(same_bytes(*message, expected[index]));
    CHECK(message->m_idxLane == lanes[index]);
    CHECK(message->m_pData == submitted_payloads[index]);

    if (message->m_idxLane == 0) {
      CHECK(decoded.sequence > previous_lane_zero_sequence);
      previous_lane_zero_sequence = decoded.sequence;
    }
    message->Release();
  }
  CHECK(std::ranges::all_of(seen, [](const bool value) {
    return value;
  }));

  SteamNetConnectionRealTimeStatus_t status{};
  std::array<SteamNetConnectionRealTimeLaneStatus_t, priorities.size()> lane_status{};
  CHECK(
    SteamNetworkingSockets()->GetConnectionRealTimeStatus(
      pair.sender(), &status, static_cast<int>(lane_status.size()), lane_status.data()) == k_EResultOK);

  custom_buffer_releases.store(0, std::memory_order_relaxed);
  SteamNetworkingMessage_t* custom = SteamNetworkingUtils()->AllocateMessage(0);
  REQUIRE(custom != nullptr);
  constexpr std::size_t custom_size = 211;
  custom->m_pData = new std::byte[custom_size];
  custom->m_cbSize = static_cast<int>(custom_size);
  custom->m_pfnFreeData = &release_custom_buffer;
  custom->m_conn = pair.sender();
  custom->m_nFlags = k_nSteamNetworkingSend_Reliable;

  int64 custom_result = 0;
  SteamNetworkingSockets()->SendMessages(1, &custom, &custom_result, false);
  REQUIRE(custom_result > 0);
  REQUIRE(custom == nullptr);

  SteamNetworkingMessage_t* custom_received = nullptr;
  REQUIRE(SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(group.handle(), &custom_received, 1) == 1);
  REQUIRE(custom_received != nullptr);
  CHECK(custom_received->m_cbSize == static_cast<int>(custom_size));
  custom_received->Release();
  CHECK(custom_buffer_releases.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("GNS real UDP loopback reassembles messages larger than MTU") {
  gns_runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());

  connection_pair pair(true);
  REQUIRE(pair.valid());

  const auto reliable_payload = make_payload(128 * 1024, 0x1234u);
  const auto unreliable_payload = make_payload(4 * 1024, 0x5678u);

  int64 reliable_number = 0;
  int64 unreliable_number = 0;
  REQUIRE(
    SteamNetworkingSockets()->SendMessageToConnection(
      pair.sender(), reliable_payload.data(), static_cast<std::uint32_t>(reliable_payload.size()),
      k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle, &reliable_number) == k_EResultOK);
  REQUIRE(
    SteamNetworkingSockets()->SendMessageToConnection(
      pair.sender(), unreliable_payload.data(), static_cast<std::uint32_t>(unreliable_payload.size()),
      k_nSteamNetworkingSend_Unreliable | k_nSteamNetworkingSend_NoNagle, &unreliable_number) == k_EResultOK);
  CHECK(reliable_number > 0);
  CHECK(unreliable_number > 0);

  std::vector<SteamNetworkingMessage_t*> received;
  received.reserve(2);
  const auto started = std::chrono::steady_clock::now();
  const bool complete = wait_for_messages(pair.receiver(), received, 2, 5s);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  INFO("UDP loopback delivery took "
       << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << " us");
  REQUIRE(complete);

  const auto reliable = std::ranges::find_if(received, [](const SteamNetworkingMessage_t* message) {
    return (message->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;
  });
  const auto unreliable = std::ranges::find_if(received, [](const SteamNetworkingMessage_t* message) {
    return (message->m_nFlags & k_nSteamNetworkingSend_Reliable) == 0;
  });
  REQUIRE(reliable != received.end());
  REQUIRE(unreliable != received.end());
  CHECK((*reliable)->m_cbSize == static_cast<int>(reliable_payload.size()));
  CHECK(std::memcmp((*reliable)->m_pData, reliable_payload.data(), reliable_payload.size()) == 0);
  CHECK((*unreliable)->m_cbSize == static_cast<int>(unreliable_payload.size()));
  CHECK(std::memcmp((*unreliable)->m_pData, unreliable_payload.data(), unreliable_payload.size()) == 0);

  SteamNetConnectionRealTimeStatus_t status{};
  CHECK(SteamNetworkingSockets()->GetConnectionRealTimeStatus(pair.sender(), &status, 0, nullptr) == k_EResultOK);
  CHECK(status.m_eState == k_ESteamNetworkingConnectionState_Connected);

  std::array<char, 4096> details{};
  CHECK(SteamNetworkingSockets()->GetDetailedConnectionStatus(
          pair.sender(), details.data(), static_cast<int>(details.size())) == 0);
  CHECK(details.front() != '\0');

  release_all(received);
}

TEST_CASE("GNS Nagle flush and fault-injected lag are externally controllable") {
  gns_runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());
  fake_network_reset reset_fake_network;

  connection_pair pair(true);
  REQUIRE(pair.valid());

  constexpr int long_nagle_delay_us = 20'000;
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_NagleTime, long_nagle_delay_us));

  const auto nagled_payload = make_payload(83, 0xabcdu);
  REQUIRE(
    SteamNetworkingSockets()->SendMessageToConnection(
      pair.sender(), nagled_payload.data(), static_cast<std::uint32_t>(nagled_payload.size()),
      k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK);

  SteamNetConnectionRealTimeStatus_t nagle_status{};
  REQUIRE(
    SteamNetworkingSockets()->GetConnectionRealTimeStatus(pair.sender(), &nagle_status, 0, nullptr) ==
    k_EResultOK);
  CHECK(nagle_status.m_cbPendingReliable + nagle_status.m_cbSentUnackedReliable > 0);
  REQUIRE(SteamNetworkingSockets()->FlushMessagesOnConnection(pair.sender()) == k_EResultOK);

  std::vector<SteamNetworkingMessage_t*> received;
  REQUIRE(wait_for_messages(pair.receiver(), received, 1, 2s));
  REQUIRE(received.front()->m_cbSize == static_cast<int>(nagled_payload.size()));
  CHECK(std::memcmp(received.front()->m_pData, nagled_payload.data(), nagled_payload.size()) == 0);
  release_all(received);
  received.clear();

  constexpr int injected_lag_ms = 40;
  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueInt32(
    k_ESteamNetworkingConfig_FakePacketLag_Send, injected_lag_ms));
  const auto delayed_payload = make_payload(97, 0x123456u);
  const auto started = std::chrono::steady_clock::now();
  REQUIRE(
    SteamNetworkingSockets()->SendMessageToConnection(
      pair.sender(), delayed_payload.data(), static_cast<std::uint32_t>(delayed_payload.size()),
      k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle, nullptr) == k_EResultOK);

  REQUIRE(wait_for_messages(pair.receiver(), received, 1, 2s));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed >= 25ms);
  REQUIRE(received.front()->m_cbSize == static_cast<int>(delayed_payload.size()));
  CHECK(std::memcmp(received.front()->m_pData, delayed_payload.data(), delayed_payload.size()) == 0);
  release_all(received);
}

TEST_CASE("GNS high-priority reliable lane completes before an earlier bulk message") {
  gns_runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());

  connection_pair pair(true);
  REQUIRE(pair.valid());

  constexpr std::array<int, 2> priorities{0, 1};
  constexpr std::array<std::uint16_t, 2> weights{1, 1};
  REQUIRE(
    SteamNetworkingSockets()->ConfigureConnectionLanes(
      pair.sender(), static_cast<int>(priorities.size()), priorities.data(), weights.data()) == k_EResultOK);

  constexpr int send_rate = 512 * 1024;
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendRateMin, send_rate));
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendRateMax, send_rate));
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendBufferSize, 1024 * 1024));

  const auto bulk_payload = make_payload(400 * 1024, 0xb01cu);
  const auto intent_payload = make_payload(79, 0x1a7e47u);
  std::array<SteamNetworkingMessage_t*, 2> outgoing{
    SteamNetworkingUtils()->AllocateMessage(static_cast<int>(bulk_payload.size())),
    SteamNetworkingUtils()->AllocateMessage(static_cast<int>(intent_payload.size())),
  };
  REQUIRE(outgoing[0] != nullptr);
  REQUIRE(outgoing[1] != nullptr);

  std::memcpy(outgoing[0]->m_pData, bulk_payload.data(), bulk_payload.size());
  outgoing[0]->m_conn = pair.sender();
  outgoing[0]->m_nFlags = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle;
  outgoing[0]->m_idxLane = 1;

  std::memcpy(outgoing[1]->m_pData, intent_payload.data(), intent_payload.size());
  outgoing[1]->m_conn = pair.sender();
  outgoing[1]->m_nFlags = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle;
  outgoing[1]->m_idxLane = 0;

  std::array<int64, outgoing.size()> results{};
  SteamNetworkingSockets()->SendMessages(
    static_cast<int>(outgoing.size()), outgoing.data(), results.data(), false);
  REQUIRE(results[0] > 0);
  REQUIRE(results[1] > 0);
  REQUIRE(outgoing[0] == nullptr);
  REQUIRE(outgoing[1] == nullptr);

  SteamNetConnectionRealTimeStatus_t status{};
  std::array<SteamNetConnectionRealTimeLaneStatus_t, priorities.size()> lane_status{};
  REQUIRE(
    SteamNetworkingSockets()->GetConnectionRealTimeStatus(
      pair.sender(), &status, static_cast<int>(lane_status.size()), lane_status.data()) == k_EResultOK);
  CHECK(
    lane_status[1].m_cbPendingReliable + lane_status[1].m_cbSentUnackedReliable >
    lane_status[0].m_cbPendingReliable + lane_status[0].m_cbSentUnackedReliable);

  SteamNetworkingMessage_t* first = wait_for_one(pair.receiver(), 3s);
  REQUIRE(first != nullptr);
  REQUIRE(first->m_idxLane == 0);
  REQUIRE(first->m_cbSize == static_cast<int>(intent_payload.size()));
  CHECK(std::memcmp(first->m_pData, intent_payload.data(), intent_payload.size()) == 0);
  first->Release();

  SteamNetworkingMessage_t* second = wait_for_one(pair.receiver(), 5s);
  REQUIRE(second != nullptr);
  REQUIRE(second->m_idxLane == 1);
  REQUIRE(second->m_cbSize == static_cast<int>(bulk_payload.size()));
  CHECK(std::memcmp(second->m_pData, bulk_payload.data(), bulk_payload.size()) == 0);
  second->Release();
}

TEST_CASE("GNS sustained mixed-lane load recovers reliable data after loss") {
  gns_runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());
  fake_network_reset reset_fake_network;

  connection_pair pair(true);
  REQUIRE(pair.valid());

  constexpr std::array<int, 3> priorities{0, 2, 1};
  constexpr std::array<std::uint16_t, 3> weights{1, 1, 1};
  REQUIRE(
    SteamNetworkingSockets()->ConfigureConnectionLanes(
      pair.sender(), static_cast<int>(priorities.size()), priorities.data(), weights.data()) == k_EResultOK);
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendRateMin, 1024 * 1024));
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendRateMax, 1024 * 1024));
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendBufferSize, 4 * 1024 * 1024));

  load_observations observations;

  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(
    k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0f));
  REQUIRE(
    queue_load_message(pair.sender(), load_message_kind::intent, 0, 73, 0, true) == k_EResultOK);

  const auto black_hole_deadline = std::chrono::steady_clock::now() + 60ms;
  while (std::chrono::steady_clock::now() < black_hole_deadline) {
    SteamNetworkingSockets()->RunCallbacks();
    REQUIRE(drain_load_messages(pair.receiver(), observations));
    std::this_thread::sleep_for(1ms);
  }
  CHECK(observations.intents.empty());

  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(
    k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f));
  REQUIRE(pump_until(
    [&] {
      if (!drain_load_messages(pair.receiver(), observations)) return false;
      return observations.intents.size() == 1;
    },
    3s));
  REQUIRE(observations.intents == std::vector<std::uint32_t>{0});

  constexpr std::uint32_t tick_count = 240;
  constexpr std::uint32_t bulk_interval = 40;
  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(
    k_ESteamNetworkingConfig_FakePacketLoss_Send, 15.0f));
  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(
    k_ESteamNetworkingConfig_FakePacketReorder_Send, 15.0f));
  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueInt32(
    k_ESteamNetworkingConfig_FakePacketReorder_Time, 5));

  for (std::uint32_t tick = 1; tick <= tick_count; ++tick) {
    REQUIRE(
      queue_load_message(pair.sender(), load_message_kind::intent, tick, 73, 0, true) == k_EResultOK);
    REQUIRE(
      queue_load_message(pair.sender(), load_message_kind::state, tick, 101, 2, false) == k_EResultOK);
    if (tick % bulk_interval == 0) {
      REQUIRE(
        queue_load_message(pair.sender(), load_message_kind::bulk, tick, 48 * 1024, 1, true) ==
        k_EResultOK);
    }

    SteamNetworkingSockets()->RunCallbacks();
    REQUIRE(drain_load_messages(pair.receiver(), observations));
    std::this_thread::sleep_for(2ms);
  }

  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(
    k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f));
  REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(
    k_ESteamNetworkingConfig_FakePacketReorder_Send, 0.0f));
  REQUIRE(pump_until(
    [&] {
      if (!drain_load_messages(pair.receiver(), observations)) return false;
      return observations.intents.size() == tick_count + 1 &&
             observations.bulk.size() == tick_count / bulk_interval;
    },
    5s));

  CHECK_FALSE(observations.corrupt);
  REQUIRE(observations.intents.size() == tick_count + 1);
  for (std::uint32_t sequence = 0; sequence <= tick_count; ++sequence) {
    CAPTURE(sequence);
    CHECK(observations.intents[sequence] == sequence);
  }

  REQUIRE(observations.bulk.size() == tick_count / bulk_interval);
  for (std::size_t i = 0; i < observations.bulk.size(); ++i) {
    CAPTURE(i);
    CHECK(observations.bulk[i] == (i + 1) * bulk_interval);
  }

  REQUIRE_FALSE(observations.states.empty());
  CHECK(observations.states.size() < tick_count);
  std::array<bool, tick_count + 1> seen_states{};
  for (const std::uint32_t sequence : observations.states) {
    CAPTURE(sequence);
    REQUIRE(sequence >= 1);
    REQUIRE(sequence <= tick_count);
    CHECK_FALSE(seen_states[sequence]);
    seen_states[sequence] = true;
  }

  SteamNetConnectionRealTimeStatus_t status{};
  REQUIRE(
    SteamNetworkingSockets()->GetConnectionRealTimeStatus(pair.sender(), &status, 0, nullptr) == k_EResultOK);
  CHECK(status.m_eState == k_ESteamNetworkingConnectionState_Connected);
}

TEST_CASE("GNS IP listen connect and reconnect use explicit new connections") {
  gns_runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());

  poll_group group;
  REQUIRE(group.handle() != k_HSteamNetPollGroup_Invalid);

  connection_callback_context callbacks;
  callbacks.poll_group = group.handle();
  connection_callback_scope callback_scope(callbacks);
  REQUIRE(callback_scope.installed());

  SteamNetworkingConfigValue_t allow_without_auth{};
  allow_without_auth.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);

  SteamNetworkingIPAddr bind_address{};
  HSteamListenSocket listen_socket = k_HSteamListenSocket_Invalid;
  for (std::uint16_t port = 39'000; port < 39'128 && listen_socket == k_HSteamListenSocket_Invalid; ++port) {
    bind_address.SetIPv4(0x7f000001u, port);
    listen_socket = SteamNetworkingSockets()->CreateListenSocketIP(bind_address, 1, &allow_without_auth);
  }
  REQUIRE(listen_socket != k_HSteamListenSocket_Invalid);
  callbacks.listen_socket = listen_socket;

  SteamNetworkingIPAddr listen_address{};
  REQUIRE(SteamNetworkingSockets()->GetListenSocketAddress(listen_socket, &listen_address));
  REQUIRE(listen_address.IsIPv4());
  REQUIRE(listen_address.GetIPv4() == 0x7f000001u);
  REQUIRE(listen_address.m_port != 0);

  const HSteamNetConnection first_client =
    SteamNetworkingSockets()->ConnectByIPAddress(listen_address, 1, &allow_without_auth);
  REQUIRE(first_client != k_HSteamNetConnection_Invalid);
  REQUIRE(pump_until(
    [&] {
      return callbacks.accepted_connections.size() == 1 && connection_is_connected(first_client) &&
             connection_is_connected(callbacks.accepted_connections.front());
    },
    3s));
  REQUIRE(callbacks.accept_results.size() == 1);
  REQUIRE(callbacks.accept_results.front() == k_EResultOK);
  REQUIRE_FALSE(callbacks.poll_group_assignment_failed);
  const HSteamNetConnection first_server = callbacks.accepted_connections.front();

  const auto first_payload = make_payload(257, 0xc011ec7u);
  REQUIRE(
    SteamNetworkingSockets()->SendMessageToConnection(
      first_client, first_payload.data(), static_cast<std::uint32_t>(first_payload.size()),
      k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle, nullptr) == k_EResultOK);

  SteamNetworkingMessage_t* first_received = nullptr;
  REQUIRE(pump_until(
    [&] {
      return SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(group.handle(), &first_received, 1) == 1;
    },
    2s));
  REQUIRE(first_received != nullptr);
  REQUIRE(first_received->m_conn == first_server);
  REQUIRE(first_received->m_cbSize == static_cast<int>(first_payload.size()));
  CHECK(std::memcmp(first_received->m_pData, first_payload.data(), first_payload.size()) == 0);
  first_received->Release();

  REQUIRE(SteamNetworkingSockets()->CloseConnection(first_client, 0, "capability reconnect", false));
  REQUIRE(pump_until(
    [&] {
      return std::ranges::find(callbacks.terminal_connections, first_server) !=
             callbacks.terminal_connections.end();
    },
    2s));
  REQUIRE(SteamNetworkingSockets()->CloseConnection(first_server, 0, nullptr, false));

  const HSteamNetConnection second_client =
    SteamNetworkingSockets()->ConnectByIPAddress(listen_address, 1, &allow_without_auth);
  REQUIRE(second_client != k_HSteamNetConnection_Invalid);
  REQUIRE(pump_until(
    [&] {
      return callbacks.accepted_connections.size() == 2 && connection_is_connected(second_client) &&
             connection_is_connected(callbacks.accepted_connections.back());
    },
    3s));
  REQUIRE(callbacks.accept_results.size() == 2);
  REQUIRE(callbacks.accept_results.back() == k_EResultOK);
  REQUIRE_FALSE(callbacks.poll_group_assignment_failed);
  const HSteamNetConnection second_server = callbacks.accepted_connections.back();

  const auto second_payload = make_payload(263, 0x5ec0adu);
  REQUIRE(
    SteamNetworkingSockets()->SendMessageToConnection(
      second_server, second_payload.data(), static_cast<std::uint32_t>(second_payload.size()),
      k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle, nullptr) == k_EResultOK);
  SteamNetworkingMessage_t* second_received = wait_for_one(second_client, 2s);
  REQUIRE(second_received != nullptr);
  REQUIRE(second_received->m_cbSize == static_cast<int>(second_payload.size()));
  CHECK(std::memcmp(second_received->m_pData, second_payload.data(), second_payload.size()) == 0);
  second_received->Release();

  CHECK(SteamNetworkingSockets()->CloseConnection(second_client, 0, nullptr, false));
  CHECK(SteamNetworkingSockets()->CloseConnection(second_server, 0, nullptr, false));
  CHECK(SteamNetworkingSockets()->CloseListenSocket(listen_socket));
}

TEST_CASE("GNS standalone auth boundary and local P2P fast path are explicit") {
  SteamNetworkingIdentity configured_identity{};
  REQUIRE(configured_identity.SetGenericString("devils-engine-capability-peer"));

  gns_runtime runtime(&configured_identity);
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());

  SteamNetworkingIdentity observed_identity{};
  REQUIRE(SteamNetworkingSockets()->GetIdentity(&observed_identity));
  CHECK(observed_identity == configured_identity);

  SteamNetAuthenticationStatus_t auth_status{};
  CHECK(SteamNetworkingSockets()->InitAuthentication() == k_ESteamNetworkingAvailability_CannotTry);
  CHECK(
    SteamNetworkingSockets()->GetAuthenticationStatus(&auth_status) ==
    k_ESteamNetworkingAvailability_CannotTry);
  CHECK(auth_status.m_eAvail == k_ESteamNetworkingAvailability_CannotTry);
  CHECK(std::strstr(auth_status.m_debugMsg, "No certificate authority") != nullptr);

  poll_group group;
  REQUIRE(group.handle() != k_HSteamNetPollGroup_Invalid);

  connection_callback_context callbacks;
  callbacks.poll_group = group.handle();
  connection_callback_scope callback_scope(callbacks);
  REQUIRE(callback_scope.installed());

  constexpr int virtual_port = 37;
  const HSteamListenSocket listen_socket =
    SteamNetworkingSockets()->CreateListenSocketP2P(virtual_port, 0, nullptr);
  REQUIRE(listen_socket != k_HSteamListenSocket_Invalid);
  callbacks.listen_socket = listen_socket;

  const HSteamNetConnection client =
    SteamNetworkingSockets()->ConnectP2P(configured_identity, virtual_port, 0, nullptr);
  REQUIRE(client != k_HSteamNetConnection_Invalid);
  REQUIRE(pump_until(
    [&] {
      return callbacks.accepted_connections.size() == 1 && connection_is_connected(client) &&
             connection_is_connected(callbacks.accepted_connections.front());
    },
    2s));
  REQUIRE(callbacks.accept_results.size() == 1);
  REQUIRE(callbacks.accept_results.front() == k_EResultOK);
  REQUIRE_FALSE(callbacks.poll_group_assignment_failed);
  const HSteamNetConnection server = callbacks.accepted_connections.front();

  SteamNetConnectionInfo_t client_info{};
  REQUIRE(SteamNetworkingSockets()->GetConnectionInfo(client, &client_info));
  CHECK(client_info.m_identityRemote == configured_identity);

  const auto payload = make_payload(127, 0x42u);
  REQUIRE(
    SteamNetworkingSockets()->SendMessageToConnection(
      client, payload.data(), static_cast<std::uint32_t>(payload.size()),
      k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK);
  SteamNetworkingMessage_t* received = nullptr;
  REQUIRE(pump_until(
    [&] {
      return SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(group.handle(), &received, 1) == 1;
    },
    2s));
  REQUIRE(received != nullptr);
  REQUIRE(received->m_conn == server);
  REQUIRE(received->m_cbSize == static_cast<int>(payload.size()));
  CHECK(std::memcmp(received->m_pData, payload.data(), payload.size()) == 0);
  received->Release();

  CHECK(SteamNetworkingSockets()->CloseConnection(client, 0, nullptr, false));
  CHECK(SteamNetworkingSockets()->CloseConnection(server, 0, nullptr, false));
  CHECK(SteamNetworkingSockets()->CloseListenSocket(listen_socket));
}

TEST_CASE("GNS bounded send queue reports backpressure without stealing failed messages") {
  gns_runtime runtime;
  REQUIRE_MESSAGE(runtime.initialized(), runtime.error());

  connection_pair pair(true);
  REQUIRE(pair.valid());

  constexpr int send_rate = 64 * 1024;
  constexpr int message_size = 100 * 1024;
  constexpr int send_buffer_size = 160 * 1024;
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendRateMin, send_rate));
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendRateMax, send_rate));
  REQUIRE(SteamNetworkingUtils()->SetConnectionConfigValueInt32(
    pair.sender(), k_ESteamNetworkingConfig_SendBufferSize, send_buffer_size));

  std::array<SteamNetworkingMessage_t*, 3> outgoing{};
  for (SteamNetworkingMessage_t*& message : outgoing) {
    message = SteamNetworkingUtils()->AllocateMessage(message_size);
    REQUIRE(message != nullptr);
    message->m_conn = pair.sender();
    message->m_nFlags = k_nSteamNetworkingSend_Reliable;
  }

  std::array<int64, outgoing.size()> results{};
  SteamNetworkingSockets()->SendMessages(
    static_cast<int>(outgoing.size()), outgoing.data(), results.data(), false);

  CHECK(results[0] > 0);
  CHECK(outgoing[0] == nullptr);
  CHECK(results[1] == -k_EResultLimitExceeded);
  CHECK(outgoing[1] != nullptr);
  CHECK(results[2] == 0);
  CHECK(outgoing[2] != nullptr);

  SteamNetConnectionRealTimeStatus_t status{};
  CHECK(SteamNetworkingSockets()->GetConnectionRealTimeStatus(pair.sender(), &status, 0, nullptr) == k_EResultOK);
  CHECK(status.m_cbPendingReliable + status.m_cbSentUnackedReliable > 0);
  CHECK(status.m_nSendRateBytesPerSecond == send_rate);

  for (SteamNetworkingMessage_t* message : outgoing) {
    if (message != nullptr) message->Release();
  }
}
