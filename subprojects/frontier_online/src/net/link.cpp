#include "net/link.h"

#include <charconv>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <devils_engine/utils/core.h>

namespace frontier_online {
namespace net {

namespace utils = devils_engine::utils;

namespace {

std::array<network::gns_lane_config, 3> declared_lanes(const size_t bulk_slots) {
  std::array<network::gns_lane_config, 3> lanes{};
  // Бюджет в байтах не может быть больше, чем удержат объявленные слоты: слоты И ЕСТЬ бюджет,
  // и просить байт больше, чем слотов, — противоречие, которое адаптер отвергает при постройке.
  lanes[lane_control] = {network::gns_delivery::reliable_ordered, 0, 1, 32, control_message_bytes,
                         32 * control_message_bytes};
  lanes[lane_bulk] = {network::gns_delivery::reliable_ordered, 2, 1, bulk_slots,
                      chunk_message_bytes, bulk_slots * chunk_message_bytes};
  // Ненадёжно-последовательные: для этого класса восстановление — это избыточность в окне тиков,
  // а не повторная отправка.
  lanes[lane_intent] = {network::gns_delivery::unreliable_sequenced, 1, 1, 32,
                        control_message_bytes, 32 * control_message_bytes};
  return lanes;
}

// У обособленного GNS нет настроенного удостоверяющего центра, поэтому лаборатория говорит
// «без аутентификации» один раз и явно, вместо того чтобы шифрование пакетов принимали за
// подтверждённую личность.
std::array<SteamNetworkingConfigValue_t, 1> unauthenticated_options() {
  std::array<SteamNetworkingConfigValue_t, 1> options{};
  options[0].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
  return options;
}

} // namespace

bool parse_port(const std::string_view text, uint16_t& port) {
  uint32_t value = 0;
  const auto* last = text.data() + text.size();
  const auto result = std::from_chars(text.data(), last, value);
  if (result.ec != std::errc{} || result.ptr != last) return false;
  if (value == 0 || value > 65535) return false;
  port = uint16_t(value);
  return true;
}

bool parse_endpoint(const std::string_view text, uint32_t& host, uint16_t& port) {
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos) return false;
  if (!parse_port(text.substr(colon + 1), port)) return false;

  const auto address = text.substr(0, colon);
  uint32_t octets[4] = {};
  size_t at = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const auto dot = address.find('.', at);
    const auto end = (i == 3) ? address.size() : dot;
    if (i < 3 && dot == std::string_view::npos) return false;
    if (i == 3 && dot != std::string_view::npos) return false;
    const auto part = address.substr(at, end - at);
    if (part.empty()) return false;
    const auto* last = part.data() + part.size();
    const auto result = std::from_chars(part.data(), last, octets[i]);
    if (result.ec != std::errc{} || result.ptr != last || octets[i] > 255) return false;
    at = end + 1;
  }
  host = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
  return true;
}

struct net_runtime::state {
  SteamNetworkingErrMsg error{};
  bool initialized = false;
  std::unique_ptr<network::gns_dispatcher> dispatcher;

  explicit state(const size_t endpoints) {
    initialized = GameNetworkingSockets_Init(nullptr, error);
    if (!initialized) utils::error{}("frontier_online net: GNS init failed: {}", error);
    dispatcher = std::make_unique<network::gns_dispatcher>(*SteamNetworkingSockets(),
                                                           *SteamNetworkingUtils(), endpoints);
  }
  ~state() {
    dispatcher.reset();
    if (initialized) GameNetworkingSockets_Kill();
  }
};

net_runtime::net_runtime(const size_t endpoints) : state_(std::make_unique<state>(endpoints)) {
  dispatcher_ = state_->dispatcher.get();
}

net_runtime::~net_runtime() = default;

void net_runtime::pump() {
  dispatcher_->pump();
}

struct net_link::state {
  std::array<network::gns_lane_config, 3> lanes;
  network::gns_transport transport;
  network::gns_listener listener;
  std::vector<std::byte> scratch;

  state(network::gns_dispatcher& dispatcher, const size_t peers)
    : lanes(declared_lanes(256)),
      transport(dispatcher,
                network::gns_transport_config{
                  // На один слот больше объявленного числа участников: переподключение
                  // ПЕРЕКРЫВАЕТСЯ с соединением, которое оно заменяет, а запас — это то, что
                  // позволяет отказать сверхштатному присоединению ПРИЧИНОЙ на уровне сессии, а
                  // не молчанием на уровне транспорта.
                  .peers = peers + 2,
                  .receive_leases = 16,
                  .max_receive_bytes = 256 * 1024,
                  .backend_receive_messages = 512,
                  .backend_receive_bytes = 8 * 1024 * 1024,
                  .listeners = 1,
                },
                lanes) {
    if (!transport.ready()) utils::error{}("frontier_online net: transport not ready");
    scratch.reserve(max_message_bytes);
  }
};

net_link::net_link(net_runtime& runtime, const size_t peers)
  : state_(std::make_unique<state>(runtime.dispatcher(), peers)) {}

net_link::~net_link() = default;

net_link::listen_outcome net_link::listen_on(const uint16_t port, const uint32_t host) {
  auto options = unauthenticated_options();
  SteamNetworkingIPAddr address{};
  address.SetIPv4(host, port);
  const auto result = state_->transport.listen(address, options);
  if (result.status != network::gns_status::ok) {
    utils::error{}("frontier_online net: listen on port {} refused, status {}", port,
                   unsigned(result.status));
  }
  state_->listener = result.listener;
  return {port, false};
}

net_link::listen_outcome net_link::listen_any(const uint32_t host) {
  auto options = unauthenticated_options();
  SteamNetworkingIPAddr address{};
  address.SetIPv4(host, 0);
  auto result = state_->transport.listen(address, options);
  if (result.status == network::gns_status::ok) {
    state_->listener = result.listener;
    SteamNetworkingIPAddr bound{};
    if (state_->transport.listen_address(state_->listener, bound) == network::gns_status::ok &&
        bound.m_port != 0) {
      return {bound.m_port, true};
    }
    (void)state_->transport.close_listener(state_->listener);
  }
  // Закреплённая сборка GNS не принимает нулевой порт. Это свойство бэкенда, а не деталь стенда,
  // поэтому запасной путь — перебор объявленного диапазона, записанный, а не спрятанный.
  for (uint16_t port = 41300; port < 41556; ++port) {
    address.m_port = port;
    result = state_->transport.listen(address, options);
    if (result.status == network::gns_status::ok) {
      state_->listener = result.listener;
      return {port, false};
    }
    if (result.status != network::gns_status::backend_rejected) {
      utils::error{}("frontier_online net: listen refused, status {}", unsigned(result.status));
    }
  }
  utils::error{}("frontier_online net: no free port in the declared range");
  return {};
}

bool net_link::connect_to(const uint32_t host, const uint16_t port) {
  auto options = unauthenticated_options();
  SteamNetworkingIPAddr address{};
  address.SetIPv4(host, port);
  const auto result = state_->transport.connect(address, options);
  if (result.status != network::gns_status::ok) return false;
  pending_ = result.peer;
  return true;
}

size_t net_link::poll(const std::span<observation> out) {
  std::array<network::gns_send_release, 32> released;
  while (state_->transport.poll_send_releases(released) != 0) {}
  std::array<network::gns_connection_event, 8> events;
  const size_t count = state_->transport.poll_connections(events);
  size_t written = 0;
  for (size_t i = 0; i < count && written < out.size(); ++i) {
    const auto& event = events[i];
    observation value{event.peer};
    value.needs_accept = event.needs_accept;
    value.connected = event.info.m_eState == k_ESteamNetworkingConnectionState_Connected;
    value.terminal = event.info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                     event.info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
    out[written++] = value;
  }
  return written;
}

network::gns_status net_link::accept(const network::gns_peer peer) {
  return state_->transport.accept(peer);
}

network::gns_status net_link::close(const network::gns_peer peer) {
  return state_->transport.close(peer);
}

send_result net_link::send(const network::gns_peer peer, const uint16_t lane,
                           const std::span<const std::byte> bytes) {
  const auto result = state_->transport.try_send(peer, lane, bytes);
  switch (result.status) {
    case network::gns_status::ok: return send_result::sent;
    case network::gns_status::invalid_peer:
    case network::gns_status::invalid_state: return send_result::no_peer;
    case network::gns_status::count_budget_exceeded:
    case network::gns_status::byte_budget_exceeded: return send_result::backpressure;
    default: return send_result::refused;
  }
}

size_t net_link::receive_batch(void* consumer,
                               void (*invoke)(void*, network::gns_peer, uint16_t,
                                              std::span<const std::byte>)) {
  size_t total = 0;
  for (unsigned round = 0; round < 8; ++round) {
    std::array<network::gns_received_message, 8> input;
    const auto result = state_->transport.receive(input);
    if (result.status != network::gns_status::ok) {
      utils::error{}("frontier_online net: receive refused, status {}", unsigned(result.status));
    }
    superseded_ += result.superseded;
    if (result.count == 0) break;
    for (size_t i = 0; i < result.count; ++i) {
      invoke(consumer, input[i].peer(), input[i].lane(), input[i].payload());
      ++total;
    }
  }
  return total;
}

net_link::conditions net_link::measure(const network::gns_peer peer) {
  SteamNetConnectionRealTimeStatus_t status{};
  conditions value;
  if (state_->transport.statistics(peer, status) != network::gns_status::ok) return value;
  value.available = true;
  value.ping_ms = status.m_nPing;
  value.quality_local = status.m_flConnectionQualityLocal;
  value.quality_remote = status.m_flConnectionQualityRemote;
  value.pending_reliable_bytes = status.m_cbPendingReliable;
  return value;
}

} // namespace net
} // namespace frontier_online
