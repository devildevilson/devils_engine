#ifndef FRONTIER_ONLINE_NET_LINK_H
#define FRONTIER_ONLINE_NET_LINK_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include <devils_engine/network/endpoint.h>
#include <devils_engine/network/gns_runtime.h>
#include <devils_engine/network/gns_transport.h>

// ТРАНСПОРТ. Знает про байты, полосы и соединения — и не знает ни про тик, ни про состояние мира,
// ни про сессию. Переподключение здесь — это НОВОЕ соединение и новый `peer`; оживлять старый
// транспорт не будет, потому что тождество игрока принадлежит сессии, а не сокету.
//
// Взято с уже отработавшего стенда NET-LAB-01 с одной правкой по контракту библиотеки:
// диспетчер здесь ОБЩИЙ и качается отдельно. У GNS один диспетчер на интерфейс и один
// поток-владелец, поэтому два конца в одном процессе (а это ровно то, что нужно для теста
// присоединения) обязаны делить его и качаться из одного места.

namespace frontier_online::net {

namespace network = devils_engine::network;

// Полосы. Разделение не косметическое: передача чек-пойнта не имеет права заткнуть собой
// канонические сообщения, а интенты не имеют права ждать повторной отправки.
inline constexpr uint16_t lane_control = 0; // handshake, join confirmation, canonical ticks — reliable
inline constexpr uint16_t lane_bulk = 1;    // чек-пойнт: надёжно, но ЯВНО ниже приоритетом
inline constexpr uint16_t lane_intent = 2;  // интенты: ненадёжно-последовательно

inline constexpr size_t control_message_bytes = 1024;
inline constexpr size_t chunk_payload_bytes = 4096;
// Заголовок куска: тип (1) + смещение (4) + длина (4).
inline constexpr size_t chunk_message_bytes = chunk_payload_bytes + 9;

inline constexpr size_t max_message_bytes =
  chunk_message_bytes > control_message_bytes ? chunk_message_bytes : control_message_bytes;

// Разбор "HOST:PORT" — network::parse_endpoint (network/endpoint.h).

// Инициализация GNS (Init/Kill со счётчиком владельцев) и общий диспетчер — движковые
// (network/gns_runtime.h). pump() зовётся из ЕДИНСТВЕННОГО потока-владельца, один раз за круг, за все
// концы сразу.
// Отдельный класс, а не псевдоним: заголовки проекта объявляют `class net_runtime;` заранее.
class net_runtime : public network::gns_runtime {
public:
  using network::gns_runtime::gns_runtime;
};

using send_result = network::gns_send_outcome;

class net_link {
public:
  net_link(net_runtime& runtime, size_t peers);
  ~net_link();
  net_link(const net_link&) = delete;
  net_link& operator=(const net_link&) = delete;

  struct listen_outcome {
    uint16_t port = 0;
    bool ephemeral = false;
  };

  // Объявленный порт: отказ, а не подмена. Оператор, назвавший порт и молча получивший другой,
  // направит клиентов не туда.
  listen_outcome listen_on(uint16_t port, uint32_t host = 0);
  // Порт у операционной системы. Нужен тесту: два конца в одном процессе не должны драться за
  // чей-то занятый номер.
  listen_outcome listen_any(uint32_t host = 0x7f000001);

  [[nodiscard]] bool connect_to(uint32_t host, uint16_t port);
  [[nodiscard]] network::gns_peer pending() const noexcept {
    return pending_;
  }

  struct observation {
    network::gns_peer peer;
    bool connected = false;
    bool terminal = false;
    bool needs_accept = false;
    // terminal из-за нарушения контракта полос: адаптер закрыл соединение сам (чужая полоса,
    // другая надёжность, размер). Это поведение участника, а не обрыв: слот не удерживается.
    bool invalid = false;
  };

  // Слитые наблюдения, ровно как обещает адаптер: это взгляд на ТЕКУЩЕЕ состояние, а не журнал
  // колбэков без потерь.
  size_t poll(std::span<observation> out);

  network::gns_status accept(network::gns_peer peer);
  network::gns_status close(network::gns_peer peer);

  send_result send(network::gns_peer peer, uint16_t lane, std::span<const std::byte> bytes);

  // Consumer(peer, lane, payload). Аренда освобождается сразу после возврата потребителя, поэтому
  // содержимое не переживает вызов: кому нужно дольше — копирует.
  //
  // `max_messages` — бюджет ЭТОГО вызова. Непрочитанное остаётся в очереди GNS: так получатель,
  // которому некуда класть, создаёт обратное давление, а не теряет сообщения и не падает.
  template <class Consumer>
  size_t receive(Consumer&& consumer, size_t max_messages = default_receive_budget);

  static constexpr size_t default_receive_budget = 64;

  uint64_t superseded() const noexcept {
    return superseded_;
  }

  // Сколько соединений адаптер закрыл сам за сообщение не по контракту полос (чужая полоса,
  // другая надёжность, превышен размер). Это поведение удалённой стороны, а не ошибка процесса.
  uint64_t invalid_closed() const noexcept {
    return invalid_closed_;
  }


private:
  size_t receive_batch(void* consumer, void (*invoke)(void*, network::gns_peer, uint16_t,
                                                      std::span<const std::byte>),
                       size_t max_messages);

  struct state;
  std::unique_ptr<state> state_;
  network::gns_peer pending_;
  uint64_t superseded_ = 0;
  uint64_t invalid_closed_ = 0;
  // Закрытые адаптером за нарушение полос, ещё не отданные poll: своё закрытие GNS событием не
  // сообщает.
  std::vector<network::gns_peer> invalid_peers_;
};

template <class Consumer>
size_t net_link::receive(Consumer&& consumer, const size_t max_messages) {
  const auto invoke = [](void* context, const network::gns_peer peer, const uint16_t lane,
                         const std::span<const std::byte> payload) {
    (*static_cast<std::remove_reference_t<Consumer>*>(context))(peer, lane, payload);
  };
  return receive_batch(static_cast<void*>(&consumer), invoke, max_messages);
}

} // namespace frontier_online::net

#endif
