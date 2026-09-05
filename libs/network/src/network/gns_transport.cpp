#include <devils_engine/network/gns_transport.h>
#include <devils_engine/utils/core.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace devils_engine::network {
namespace detail {
struct gns_receive_budget {
  std::atomic_size_t count = 0;
};
} // namespace detail

namespace {
enum class slot_state : std::uint8_t { free,
                                       in_flight,
                                       released };
struct send_pool;
struct send_slot {
  std::atomic<slot_state> state = slot_state::free;
  // A native message may survive transport destruction. This reference keeps
  // its slab alive; the native callback breaks the cycle before publishing
  // release. No callback stores or dereferences a transport pointer.
  std::shared_ptr<send_pool> owner;
  gns_send_release completion;
};

struct send_pool {
  explicit send_pool(const gns_lane_config& config)
    : slots(std::make_unique<send_slot[]>(config.send_slots)),
      bytes(config.send_slots * config.max_payload_bytes), config(config) {}
  std::unique_ptr<send_slot[]> slots;
  std::vector<std::byte> bytes;
  gns_lane_config config;
  // Only the transport's owner accesses these counters.
  std::size_t retained_count = 0;
  std::size_t retained_bytes = 0;
};

void release_send_payload(SteamNetworkingMessage_t* message) noexcept {
  auto* slot = reinterpret_cast<send_slot*>(std::uintptr_t(message->m_nUserData));
  auto keep_alive = std::move(slot->owner);
  // This must be the last slot access: the owner may reuse it immediately.
  slot->state.store(slot_state::released, std::memory_order_release);
}

std::atomic_uint64_t next_peer_generation{1};

std::uint64_t new_generation() {
  const auto value = next_peer_generation.fetch_add(1, std::memory_order_relaxed);
  if (value == 0 || value == UINT64_MAX) utils::error{}("network GNS peer generation exhausted");
  return value;
}

void reclaim(send_pool& pool, send_slot& slot) {
  --pool.retained_count;
  pool.retained_bytes -= slot.completion.bytes;
  slot.state.store(slot_state::free, std::memory_order_relaxed);
}
} // namespace

gns_received_message::~gns_received_message() {
  reset();
}

gns_received_message::gns_received_message(gns_received_message&& other) noexcept
  : message_(std::exchange(other.message_, nullptr)), budget_(std::move(other.budget_)),
    peer_(std::exchange(other.peer_, {})) {}

gns_received_message& gns_received_message::operator=(gns_received_message&& other) noexcept {
  if (this == &other) return *this;
  reset();
  message_ = std::exchange(other.message_, nullptr);
  budget_ = std::move(other.budget_);
  peer_ = std::exchange(other.peer_, {});
  return *this;
}

void gns_received_message::reset() noexcept {
  if (!message_) return;
  message_->Release();
  message_ = nullptr;
  budget_->count.fetch_sub(1, std::memory_order_release);
  budget_.reset();
  peer_ = {};
}

std::span<const std::byte> gns_received_message::payload() const noexcept {
  if (!message_) return {};
  return {static_cast<const std::byte*>(message_->m_pData), std::size_t(message_->m_cbSize)};
}
std::uint16_t gns_received_message::lane() const noexcept {
  return message_ ? message_->m_idxLane : 0;
}
std::int64_t gns_received_message::message_number() const noexcept {
  return message_ ? message_->m_nMessageNumber : 0;
}

struct gns_transport::impl {
  struct connection {
    HSteamNetConnection handle = k_HSteamNetConnection_Invalid;
    std::uint64_t generation = 0;
    ESteamNetworkingConnectionState reported = k_ESteamNetworkingConnectionState_None;
  };

  impl(ISteamNetworkingSockets& sockets, ISteamNetworkingUtils& utils,
       const gns_transport_config config, const std::span<const gns_lane_config> lanes)
    : sockets(sockets), utils(utils), config(config) {
    if (config.peers == 0 || config.peers >= UINT32_MAX || lanes.empty() || lanes.size() > 255 ||
        config.receive_leases == 0 || config.max_receive_bytes == 0 ||
        config.max_receive_bytes > std::size_t(INT32_MAX) || config.backend_receive_messages <= 0 ||
        config.backend_receive_bytes <= 0 || std::size_t(config.backend_receive_bytes) <= config.max_receive_bytes)
      devils_engine::utils::error{}("network GNS invalid receive/peer/lane configuration");
    if (lanes.size() > std::numeric_limits<std::size_t>::max() / config.peers)
      devils_engine::utils::error{}("network GNS connection table size overflow");
    std::size_t total_bytes = 0;
    for (const auto& lane : lanes) {
      if (lane.send_slots == 0 || lane.max_payload_bytes == 0 || lane.weight == 0 ||
          lane.max_payload_bytes > std::size_t(k_cbMaxSteamNetworkingSocketsMessageSizeSend) ||
          lane.send_slots > std::numeric_limits<std::size_t>::max() / lane.max_payload_bytes ||
          lane.send_byte_budget == 0 || lane.send_byte_budget > lane.send_slots * lane.max_payload_bytes ||
          lane.send_byte_budget > std::size_t(INT32_MAX) - total_bytes)
        devils_engine::utils::error{}("network GNS invalid send lane budget");
      total_bytes += lane.send_byte_budget;
    }
    // The pinned GNS backend clamps SendBufferSize to at least 4 KiB. Our
    // per-lane slabs still enforce the smaller application budgets exactly.
    send_buffer_bytes = int(std::max(total_bytes, std::size_t{4096}));
    peers.resize(config.peers);
    newest.resize(config.peers * lanes.size());
    pools.reserve(lanes.size());
    priorities.reserve(lanes.size());
    weights.reserve(lanes.size());
    for (const auto& lane : lanes) {
      pools.push_back(std::make_shared<send_pool>(lane));
      priorities.push_back(lane.priority);
      weights.push_back(lane.weight);
    }
    receive_budget = std::make_shared<detail::gns_receive_budget>();
    group = sockets.CreatePollGroup();
  }

  ~impl() {
    for (auto& peer : peers) {
      if (peer.handle != k_HSteamNetConnection_Invalid)
        sockets.CloseConnection(peer.handle, 0, "transport shutdown", false);
    }
    if (group != k_HSteamNetPollGroup_Invalid) sockets.DestroyPollGroup(group);
    // In-flight slots retain their own pools until GNS frees the payload.
  }

  connection* find(const gns_peer peer) {
    if (peer.slot >= peers.size() || peer.generation == 0) return nullptr;
    auto& value = peers[peer.slot];
    return value.handle != k_HSteamNetConnection_Invalid && value.generation == peer.generation ? &value : nullptr;
  }

  ISteamNetworkingSockets& sockets;
  ISteamNetworkingUtils& utils;
  gns_transport_config config;
  std::vector<connection> peers;
  std::vector<std::int64_t> newest;
  std::vector<std::shared_ptr<send_pool>> pools;
  std::vector<int> priorities;
  std::vector<std::uint16_t> weights;
  std::shared_ptr<detail::gns_receive_budget> receive_budget;
  HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
  int send_buffer_bytes = 0;
};

gns_transport::gns_transport(ISteamNetworkingSockets& sockets, ISteamNetworkingUtils& utils,
                             const gns_transport_config config, const std::span<const gns_lane_config> lanes)
  : impl_(std::make_unique<impl>(sockets, utils, config, lanes)) {}
gns_transport::~gns_transport() = default;
bool gns_transport::ready() const noexcept {
  return impl_->group != k_HSteamNetPollGroup_Invalid;
}

gns_adopt_result gns_transport::adopt(const HSteamNetConnection handle) {
  auto& state = *impl_;
  if (handle == k_HSteamNetConnection_Invalid) return {gns_status::invalid_peer, {}};
  for (const auto& peer : state.peers) {
    if (peer.handle == handle) return {gns_status::already_owned, {}};
  }
  const auto refuse = [&](const gns_status reason) {
    state.sockets.CloseConnection(handle, 0, "transport adoption refused", false);
    return gns_adopt_result{reason, {}};
  };
  if (!ready()) return refuse(gns_status::not_ready);
  const auto free = std::find_if(state.peers.begin(), state.peers.end(), [](const auto& peer) {
    return peer.handle == k_HSteamNetConnection_Invalid;
  });
  if (free == state.peers.end()) return refuse(gns_status::peer_capacity_exceeded);
  SteamNetConnectionInfo_t info{};
  if (!state.sockets.GetConnectionInfo(handle, &info)) return refuse(gns_status::invalid_peer);
  const auto set = [&](const ESteamNetworkingConfigValue key, const int value) {
    if (!state.utils.SetConnectionConfigValueInt32(handle, key, value)) return false;
    int32 actual = 0;
    std::size_t bytes = sizeof(actual);
    ESteamNetworkingConfigDataType type{};
    const auto result = state.utils.GetConfigValue(key, k_ESteamNetworkingConfig_Connection,
                                                   handle, &type, &actual, &bytes);
    return result == k_ESteamNetworkingGetConfigValue_OK &&
           type == k_ESteamNetworkingConfig_Int32 && actual == value;
  };
  if (!set(k_ESteamNetworkingConfig_SendBufferSize, state.send_buffer_bytes) ||
      !set(k_ESteamNetworkingConfig_RecvBufferSize, state.config.backend_receive_bytes) ||
      !set(k_ESteamNetworkingConfig_RecvBufferMessages, state.config.backend_receive_messages) ||
      !set(k_ESteamNetworkingConfig_RecvMaxMessageSize, int(state.config.max_receive_bytes)) ||
      state.sockets.ConfigureConnectionLanes(handle, int(state.pools.size()),
                                             state.priorities.data(), state.weights.data()) != k_EResultOK ||
      !state.sockets.SetConnectionPollGroup(handle, state.group))
    return refuse(gns_status::configuration_failed);
  const auto index = std::size_t(free - state.peers.begin());
  *free = {handle, new_generation(), k_ESteamNetworkingConnectionState_None};
  std::fill_n(state.newest.begin() + std::ptrdiff_t(index * state.pools.size()), state.pools.size(), 0);
  return {gns_status::ok, {std::uint32_t(index), free->generation}};
}

gns_status gns_transport::close(const gns_peer peer) {
  auto* value = impl_->find(peer);
  if (!value) return gns_status::invalid_peer;
  impl_->sockets.CloseConnection(value->handle, 0, "transport close", false);
  *value = {};
  return gns_status::ok;
}

gns_send_result gns_transport::try_send(const gns_peer peer, const std::uint16_t lane,
                                        const std::span<const std::byte> payload, const std::uint64_t tag) {
  auto& state = *impl_;
  auto* connection = state.find(peer);
  if (!connection) return {gns_status::invalid_peer};
  if (lane >= state.pools.size()) return {gns_status::invalid_lane};
  auto& pool = *state.pools[lane];
  if (payload.size() > pool.config.max_payload_bytes) return {gns_status::payload_too_large};
  if (pool.retained_count == pool.config.send_slots) return {gns_status::count_budget_exceeded};
  if (payload.size() > pool.config.send_byte_budget - pool.retained_bytes) return {gns_status::byte_budget_exceeded};
  std::size_t index = 0;
  while (pool.slots[index].state.load(std::memory_order_acquire) != slot_state::free)
    ++index;
  auto& slot = pool.slots[index];
  // The pinned GNS still allocates its native header here. Only payload storage
  // is supplied by our slab; this is not a zero-allocation native send claim.
  auto* message = state.utils.AllocateMessage(0);
  if (!message) return {gns_status::backend_rejected, -k_EResultFail};
  auto* bytes = pool.bytes.data() + index * pool.config.max_payload_bytes;
  if (!payload.empty()) std::memcpy(bytes, payload.data(), payload.size());
  slot.completion = {peer, lane, tag, 0, payload.size()};
  slot.owner = state.pools[lane];
  slot.state.store(slot_state::in_flight, std::memory_order_relaxed);
  ++pool.retained_count;
  pool.retained_bytes += payload.size();
  message->m_pData = bytes;
  message->m_cbSize = int(payload.size());
  message->m_pfnFreeData = &release_send_payload;
  message->m_nUserData = int64(reinterpret_cast<std::uintptr_t>(&slot));
  message->m_conn = connection->handle;
  message->m_idxLane = lane;
  message->m_nFlags = (pool.config.no_nagle ? k_nSteamNetworkingSend_NoNagle : 0) |
                      (pool.config.delivery == gns_delivery::reliable_ordered ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable);
  int64 result = 0;
  state.sockets.SendMessages(1, &message, &result, false);
  if (result <= 0) {
    // With bDeleteFailedMessages=false GNS leaves ownership of rejected
    // messages here. Release invokes our callback synchronously.
    if (message) message->Release();
    reclaim(pool, slot);
    return {gns_status::backend_rejected, result};
  }
  slot.completion.message_number = result;
  return {gns_status::ok, result};
}

std::size_t gns_transport::poll_send_releases(const std::span<gns_send_release> output) {
  std::size_t count = 0;
  for (auto& pool_ptr : impl_->pools) {
    auto& pool = *pool_ptr;
    for (std::size_t i = 0; i < pool.config.send_slots && count < output.size(); ++i) {
      auto& slot = pool.slots[i];
      if (slot.state.load(std::memory_order_acquire) != slot_state::released) continue;
      output[count++] = slot.completion;
      reclaim(pool, slot);
    }
  }
  return count;
}

gns_receive_result gns_transport::receive(const std::span<gns_received_message> output,
                                          const std::size_t work_budget) {
  auto& state = *impl_;
  if (!ready()) return {gns_status::not_ready};
  for (const auto& value : output)
    if (value) return {gns_status::output_not_empty};
  gns_receive_result result;
  for (std::size_t examined = 0; examined < work_budget && result.count < output.size(); ++examined) {
    if (state.receive_budget->count.load(std::memory_order_acquire) >= state.config.receive_leases) {
      result.status = gns_status::count_budget_exceeded;
      break;
    }
    SteamNetworkingMessage_t* message = nullptr;
    const int received = state.sockets.ReceiveMessagesOnPollGroup(state.group, &message, 1);
    if (received == 0) break;
    if (received < 0) {
      result.status = gns_status::backend_rejected;
      break;
    }
    const auto connection = std::find_if(state.peers.begin(), state.peers.end(), [&](const auto& peer) {
      return peer.handle == message->m_conn;
    });
    const auto index = std::size_t(connection - state.peers.begin());
    const auto lane = message->m_idxLane;
    const bool reliable = (message->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;
    if (connection == state.peers.end() || lane >= state.pools.size() || message->m_cbSize < 0 ||
        std::size_t(message->m_cbSize) > state.config.max_receive_bytes ||
        reliable != (state.pools[lane]->config.delivery == gns_delivery::reliable_ordered)) {
      message->Release();
      if (connection != state.peers.end()) (void)close({std::uint32_t(index), connection->generation});
      result.status = gns_status::invalid_message;
      break;
    }
    auto& newest = state.newest[index * state.pools.size() + lane];
    if (!reliable && message->m_nMessageNumber <= newest) {
      message->Release();
      ++result.superseded;
      continue;
    }
    newest = message->m_nMessageNumber;
    auto& target = output[result.count++];
    state.receive_budget->count.fetch_add(1, std::memory_order_relaxed);
    target.message_ = message;
    target.budget_ = state.receive_budget;
    target.peer_ = {std::uint32_t(index), connection->generation};
  }
  return result;
}

std::size_t gns_transport::poll_connections(const std::span<gns_connection_event> output) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < impl_->peers.size() && count < output.size(); ++i) {
    auto& peer = impl_->peers[i];
    SteamNetConnectionInfo_t info{};
    if (peer.handle == k_HSteamNetConnection_Invalid || !impl_->sockets.GetConnectionInfo(peer.handle, &info) ||
        info.m_eState == peer.reported) continue;
    output[count++] = {{std::uint32_t(i), peer.generation}, info};
    peer.reported = info.m_eState;
  }
  return count;
}

gns_status gns_transport::statistics(const gns_peer peer, SteamNetConnectionRealTimeStatus_t& status,
                                     const std::span<SteamNetConnectionRealTimeLaneStatus_t> lanes) {
  auto* connection = impl_->find(peer);
  if (!connection) return gns_status::invalid_peer;
  if (lanes.size() > impl_->pools.size()) return gns_status::invalid_lane;
  return impl_->sockets.GetConnectionRealTimeStatus(connection->handle, &status, int(lanes.size()), lanes.data()) == k_EResultOK
           ? gns_status::ok
           : gns_status::backend_rejected;
}

std::size_t gns_transport::retained_send_count(const std::uint16_t lane) const noexcept {
  return lane < impl_->pools.size() ? impl_->pools[lane]->retained_count : 0;
}
std::size_t gns_transport::retained_send_bytes(const std::uint16_t lane) const noexcept {
  return lane < impl_->pools.size() ? impl_->pools[lane]->retained_bytes : 0;
}
std::size_t gns_transport::leased_receive_count() const noexcept {
  return impl_->receive_budget->count.load(std::memory_order_acquire);
}

} // namespace devils_engine::network
