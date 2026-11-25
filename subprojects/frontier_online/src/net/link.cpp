#include "net/link.h"

#include <algorithm>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <devils_engine/utils/core.h>

namespace frontier_online {
namespace net {

namespace utils = devils_engine::utils;

namespace {

// Слоты полосы у адаптера общие для всех участников. Участник, убитый без закрытия, ещё ~10 с
// числится соединённым и не отпускает ничего; без предела на соединение его bundles выбирали весь
// пул, и ответ рукопожатия новичку не уходил. Поэтому управляющая полоса заводит по 32 слота на
// каждое соединение, которое транспорт может держать, и тот же предел на одно соединение
// (`peer_send_slots`) — его держит адаптер.
inline constexpr size_t control_slots_per_peer = 32;

std::array<network::gns_lane_config, 3> declared_lanes(const size_t bulk_slots,
                                                       const size_t connections) {
  std::array<network::gns_lane_config, 3> lanes{};
  const size_t control_slots = control_slots_per_peer * connections;
  // Бюджет в байтах не может быть больше, чем удержат объявленные слоты: слоты И ЕСТЬ бюджет,
  // и просить байт больше, чем слотов, — противоречие, которое адаптер отвергает при постройке.
  lanes[lane_control] = {network::gns_delivery::reliable_ordered, 0, 1, control_slots,
                         control_message_bytes, control_slots * control_message_bytes,
                         control_slots_per_peer};
  lanes[lane_bulk] = {network::gns_delivery::reliable_ordered, 2, 1, bulk_slots,
                      chunk_message_bytes, bulk_slots * chunk_message_bytes};
  // Ненадёжно-последовательные: для этого класса восстановление — это избыточность в окне тиков,
  // а не повторная отправка.
  lanes[lane_intent] = {network::gns_delivery::unreliable_sequenced, 1, 1, 32,
                        control_message_bytes, 32 * control_message_bytes};
  return lanes;
}

} // namespace

struct net_link::state {
  std::array<network::gns_lane_config, 3> lanes;
  network::gns_transport transport;
  network::gns_listener listener;
  std::vector<std::byte> scratch;

  // Слоты отправки возвращаются адаптеру только когда владелец увидел их освобождение.
  void drain_releases() {
    std::array<network::gns_send_release, 32> released;
    while (transport.poll_send_releases(released) != 0) {}
  }

  state(network::gns_dispatcher& dispatcher, const size_t peers)
    : lanes(declared_lanes(256, peers + 2)),
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
  const auto result = network::gns_listen_on(state_->transport, host, port);
  if (result.status != network::gns_status::ok) {
    utils::error{}("frontier_online net: listen on port {} refused: {}", port, network::describe(result.status));
  }
  state_->listener = result.listener;
  return {result.port, result.ephemeral};
}

net_link::listen_outcome net_link::listen_any(const uint32_t host) {
  // Закреплённая сборка GNS не принимает нулевой порт: запасной путь — перебор объявленного
  // диапазона (network::gns_listen_any).
  const auto result = network::gns_listen_any(state_->transport, host, 41300, 256);
  if (result.status != network::gns_status::ok) {
    utils::error{}("frontier_online net: no listen port in [41300, 41556): {}", network::describe(result.status));
  }
  state_->listener = result.listener;
  return {result.port, result.ephemeral};
}

bool net_link::connect_to(const uint32_t host, const uint16_t port) {
  auto options = network::gns_unauthenticated_options();
  SteamNetworkingIPAddr address{};
  address.SetIPv4(host, port);
  const auto result = state_->transport.connect(address, options);
  if (result.status != network::gns_status::ok) return false;
  pending_ = result.peer;
  return true;
}

size_t net_link::poll(const std::span<observation> out) {
  state_->drain_releases();
  // Событие, взятое у транспорта, обратно не вернуть: брать ровно столько, сколько есть куда
  // положить, остальное дождётся следующего вызова.
  size_t written = 0;
  while (!invalid_peers_.empty() && written < out.size()) {
    observation value{invalid_peers_.back()};
    value.terminal = true;
    value.invalid = true;
    out[written++] = value;
    invalid_peers_.pop_back();
  }
  std::array<network::gns_connection_event, 8> events;
  const size_t count = state_->transport.poll_connections(
    std::span(events).first(std::min(events.size(), out.size() - written)));
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
  auto& self = *state_;
  auto result = self.transport.try_send(peer, lane, bytes);
  if (result.status == network::gns_status::peer_budget_exceeded ||
      result.status == network::gns_status::count_budget_exceeded ||
      result.status == network::gns_status::byte_budget_exceeded) {
    self.drain_releases();
    result = self.transport.try_send(peer, lane, bytes);
  }
  return network::classify_send(result.status);
}

size_t net_link::receive_batch(void* consumer,
                               void (*invoke)(void*, network::gns_peer, uint16_t,
                                              std::span<const std::byte>),
                               const size_t max_messages) {
  size_t total = 0;
  while (total < max_messages) {
    std::array<network::gns_received_message, 8> input;
    const size_t want = std::min(input.size(), max_messages - total);
    const auto result = state_->transport.receive(std::span(input).first(want));
    superseded_ += result.superseded;
    // Полученное обрабатывается ВСЕГДА, даже вместе с не-ok: адаптер обещает, что `count`
    // сообщений уже арендованы, и бросить их значило бы потерять надёжные байты.
    for (size_t i = 0; i < result.count; ++i) {
      invoke(consumer, input[i].peer(), input[i].lane(), input[i].payload());
      ++total;
    }
    switch (result.status) {
      case network::gns_status::ok:
        break;
      case network::gns_status::count_budget_exceeded:
        return total; // аренды заняты: обратное давление, следующий круг
      case network::gns_status::invalid_message:
        // Удалённая сторона нарушила контракт полос; адаптер уже закрыл ЕЁ соединение. Это
        // поведение участника, а не повод останавливать процесс: о закрытии сообщит poll
        // (наблюдение terminal + invalid), GNS сам о своём закрытии не сообщает.
        ++invalid_closed_;
        if (result.closed.slot != UINT32_MAX) invalid_peers_.push_back(result.closed);
        continue;
      default:
        utils::error{}("frontier_online net: receive refused, status {}",
                       unsigned(result.status));
    }
    if (result.count < want) break;
  }
  return total;
}


} // namespace net
} // namespace frontier_online
