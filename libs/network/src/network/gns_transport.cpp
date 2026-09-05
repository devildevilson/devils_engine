#include <devils_engine/network/gns_transport.h>
#include <devils_engine/utils/core.h>

#include <algorithm>
#include <array>
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

constexpr std::size_t max_creation_options = 64;
bool valid_options(const std::span<const SteamNetworkingConfigValue_t> options) {
  return options.size() <= max_creation_options &&
         std::ranges::none_of(options, [](const auto& option) {
           return option.m_eValue == k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged;
         });
}
} // namespace

struct gns_dispatcher::impl {
  ISteamNetworkingSockets& sockets;
  ISteamNetworkingUtils& utils;
  std::vector<gns_transport*> transports;
};

thread_local gns_dispatcher* gns_dispatcher::active_ = nullptr;

gns_dispatcher::gns_dispatcher(ISteamNetworkingSockets& sockets, ISteamNetworkingUtils& utils,
                               const std::size_t capacity)
  : impl_(std::make_unique<impl>(sockets, utils, std::vector<gns_transport*>(capacity))) {
  if (capacity == 0) devils_engine::utils::error{}("network GNS dispatcher capacity must be positive");
}

gns_dispatcher::~gns_dispatcher() {
  for (const auto* transport : impl_->transports)
    if (transport) devils_engine::utils::error{}("network GNS dispatcher must outlive transports");
}

void gns_dispatcher::pump() {
  if (active_) devils_engine::utils::error{}("network GNS callback pump must not be recursive");
  active_ = this;
  impl_->sockets.RunCallbacks();
  active_ = nullptr;
}

void gns_dispatcher::on_connection(SteamNetConnectionStatusChangedCallback_t* event) {
  // A stale native callback carries no pointer to a destroyed owner. Raw
  // RunCallbacks outside pump violates the dispatch contract, but must not UAF.
  if (!active_) return;
  for (auto* transport : active_->impl_->transports)
    if (transport && transport->on_connection(*event)) return;
}

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
    HSteamListenSocket listener = k_HSteamListenSocket_Invalid;
    bool needs_accept = false;
  };
  struct listener {
    HSteamListenSocket handle = k_HSteamListenSocket_Invalid;
    std::uint64_t generation = 0;
  };

  impl(ISteamNetworkingSockets& sockets, ISteamNetworkingUtils& utils,
       const gns_transport_config config, const std::span<const gns_lane_config> lanes)
    : sockets(sockets), utils(utils), config(config) {
    if (config.peers == 0 || config.peers >= UINT32_MAX || config.listeners >= UINT32_MAX ||
        lanes.empty() || lanes.size() > 255 ||
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
    listeners.resize(config.listeners);
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
    for (const auto& listener : listeners)
      if (listener.handle != k_HSteamListenSocket_Invalid) sockets.CloseListenSocket(listener.handle);
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
  std::vector<listener> listeners;
  std::vector<std::int64_t> newest;
  std::vector<std::shared_ptr<send_pool>> pools;
  std::vector<int> priorities;
  std::vector<std::uint16_t> weights;
  std::shared_ptr<detail::gns_receive_budget> receive_budget;
  HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
  int send_buffer_bytes = 0;
  gns_dispatcher* dispatcher = nullptr;
  std::uint64_t refused_incoming = 0;
};

gns_transport::gns_transport(ISteamNetworkingSockets& sockets, ISteamNetworkingUtils& utils,
                             const gns_transport_config config, const std::span<const gns_lane_config> lanes)
  : impl_(std::make_unique<impl>(sockets, utils, config, lanes)) {}
gns_transport::gns_transport(gns_dispatcher& dispatcher, const gns_transport_config config,
                             const std::span<const gns_lane_config> lanes)
  : gns_transport(dispatcher.impl_->sockets, dispatcher.impl_->utils, config, lanes) {
  const auto slot = std::ranges::find(dispatcher.impl_->transports, nullptr);
  if (slot == dispatcher.impl_->transports.end()) {
    shutdown();
    return;
  }
  *slot = this;
  impl_->dispatcher = &dispatcher;
}
gns_transport::~gns_transport() {
  shutdown();
}
bool gns_transport::ready() const noexcept {
  return impl_->group != k_HSteamNetPollGroup_Invalid;
}

gns_listen_result gns_transport::listen(const SteamNetworkingIPAddr& address,
                                        const std::span<const SteamNetworkingConfigValue_t> options) {
  auto& state = *impl_;
  if (!ready() || !state.dispatcher) return {gns_status::not_ready, {}};
  if (!valid_options(options)) return {gns_status::invalid_options, {}};
  const auto slot = std::ranges::find_if(state.listeners, [](const auto& item) {
    return !item.handle;
  });
  if (slot == state.listeners.end()) return {gns_status::listener_capacity_exceeded, {}};
  std::array<SteamNetworkingConfigValue_t, max_creation_options + 1> values;
  std::ranges::copy(options, values.begin());
  values[options.size()].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                                reinterpret_cast<void*>(&gns_dispatcher::on_connection));
  const auto handle = state.sockets.CreateListenSocketIP(address, int(options.size() + 1), values.data());
  if (!handle) return {gns_status::backend_rejected, {}};
  *slot = {handle, new_generation()};
  return {gns_status::ok, {std::uint32_t(slot - state.listeners.begin()), slot->generation}};
}

gns_status gns_transport::listen_address(const gns_listener listener, SteamNetworkingIPAddr& address) {
  if (listener.slot >= impl_->listeners.size()) return gns_status::invalid_listener;
  const auto& item = impl_->listeners[listener.slot];
  if (!item.handle || item.generation != listener.generation) return gns_status::invalid_listener;
  return impl_->sockets.GetListenSocketAddress(item.handle, &address) ? gns_status::ok : gns_status::backend_rejected;
}

gns_adopt_result gns_transport::connect(const SteamNetworkingIPAddr& address,
                                        const std::span<const SteamNetworkingConfigValue_t> options) {
  auto& state = *impl_;
  if (!ready() || !state.dispatcher) return {gns_status::not_ready, {}};
  if (!valid_options(options)) return {gns_status::invalid_options, {}};
  if (std::ranges::none_of(state.peers, [](const auto& item) {
        return !item.handle;
      }))
    return {gns_status::peer_capacity_exceeded, {}};
  std::array<SteamNetworkingConfigValue_t, max_creation_options + 1> values;
  std::ranges::copy(options, values.begin());
  values[options.size()].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                                reinterpret_cast<void*>(&gns_dispatcher::on_connection));
  const auto handle = state.sockets.ConnectByIPAddress(address, int(options.size() + 1), values.data());
  return handle ? adopt(handle) : gns_adopt_result{gns_status::backend_rejected, {}};
}

bool gns_transport::on_connection(const SteamNetConnectionStatusChangedCallback_t& event) {
  auto& state = *impl_;
  // Existing peers are observed by poll_connections. Native intermediate states
  // coalesce; terminal handles remain owned until the caller explicitly closes.
  for (const auto& peer : state.peers)
    if (peer.handle == event.m_hConn) return true;
  if (!event.m_info.m_hListenSocket) return false;
  const auto listener = std::ranges::find_if(state.listeners, [&](const auto& item) {
    return item.handle == event.m_info.m_hListenSocket;
  });
  if (listener == state.listeners.end()) return false;
  SteamNetConnectionInfo_t current{};
  // A Connecting callback may already be stale, including after local close.
  if (!state.sockets.GetConnectionInfo(event.m_hConn, &current)) return true;
  if (current.m_eState != k_ESteamNetworkingConnectionState_Connecting) {
    state.sockets.CloseConnection(event.m_hConn, 0, "incoming connection ended before admission", false);
    ++state.refused_incoming;
    return true;
  }
  const auto result = adopt(event.m_hConn);
  if (result.status != gns_status::ok) {
    ++state.refused_incoming;
    return true;
  }
  auto& peer = state.peers[result.peer.slot];
  peer.listener = listener->handle;
  peer.needs_accept = true;
  return true;
}

gns_status gns_transport::accept(const gns_peer peer) {
  auto* item = impl_->find(peer);
  if (!item) return gns_status::invalid_peer;
  if (!item->needs_accept) return gns_status::invalid_state;
  if (impl_->sockets.AcceptConnection(item->handle) != k_EResultOK) return gns_status::backend_rejected;
  item->needs_accept = false;
  return gns_status::ok;
}

gns_status gns_transport::close_listener(const gns_listener listener) {
  if (listener.slot >= impl_->listeners.size()) return gns_status::invalid_listener;
  auto& item = impl_->listeners[listener.slot];
  if (!item.handle || item.generation != listener.generation) return gns_status::invalid_listener;
  for (auto& peer : impl_->peers) {
    if (!peer.handle || peer.listener != item.handle) continue;
    impl_->sockets.CloseConnection(peer.handle, 0, "listener close", false);
    peer = {};
  }
  impl_->sockets.CloseListenSocket(item.handle);
  item = {};
  return gns_status::ok;
}

void gns_transport::shutdown() {
  auto& state = *impl_;
  if (state.dispatcher) {
    for (auto& item : state.dispatcher->impl_->transports)
      if (item == this) item = nullptr;
    state.dispatcher = nullptr;
  }
  for (auto& peer : state.peers) {
    if (peer.handle) state.sockets.CloseConnection(peer.handle, 0, "transport shutdown", false);
    peer = {};
  }
  for (auto& listener : state.listeners) {
    if (listener.handle) state.sockets.CloseListenSocket(listener.handle);
    listener = {};
  }
  if (state.group) state.sockets.DestroyPollGroup(state.group);
  state.group = k_HSteamNetPollGroup_Invalid;
}

std::uint64_t gns_transport::refused_incoming_count() const noexcept {
  return impl_->refused_incoming;
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
    output[count++] = {{std::uint32_t(i), peer.generation}, info, peer.needs_accept && info.m_eState == k_ESteamNetworkingConnectionState_Connecting};
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
