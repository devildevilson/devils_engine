#ifndef FRONTIER_ONLINE_NET_LINK_H
#define FRONTIER_ONLINE_NET_LINK_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

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
inline constexpr uint16_t lane_control = 0; // рукопожатие, объявление мира, отчёты — надёжно
inline constexpr uint16_t lane_bulk = 1;    // чек-пойнт: надёжно, но ЯВНО ниже приоритетом
inline constexpr uint16_t lane_intent = 2;  // интенты: ненадёжно-последовательно

inline constexpr size_t control_message_bytes = 1024;
inline constexpr size_t chunk_payload_bytes = 4096;
// Заголовок куска: тип (1) + смещение (4) + длина (4).
inline constexpr size_t chunk_message_bytes = chunk_payload_bytes + 9;

inline constexpr size_t max_message_bytes =
  chunk_message_bytes > control_message_bytes ? chunk_message_bytes : control_message_bytes;

// Разбор "host:port" и "port". Возвращает false на любом мусоре: адрес, молча превратившийся в
// дефолтный, соединяет не с тем и объясняется потом часами.
[[nodiscard]] bool parse_endpoint(std::string_view text, uint32_t& host, uint16_t& port);
[[nodiscard]] bool parse_port(std::string_view text, uint16_t& port);

// Инициализация GNS и общий диспетчер. Ровно один на процесс.
class net_runtime {
public:
  explicit net_runtime(size_t endpoints);
  ~net_runtime();
  net_runtime(const net_runtime&) = delete;
  net_runtime& operator=(const net_runtime&) = delete;

  // Прокачать колбэки интерфейса. Зовётся из ЕДИНСТВЕННОГО потока-владельца, один раз за круг,
  // за все концы сразу.
  void pump();

  network::gns_dispatcher& dispatcher() noexcept {
    return *dispatcher_;
  }

private:
  struct state;
  std::unique_ptr<state> state_;
  network::gns_dispatcher* dispatcher_ = nullptr;
};

enum class send_result : uint8_t { sent, no_peer, backpressure, refused };

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
  };

  // Слитые наблюдения, ровно как обещает адаптер: это взгляд на ТЕКУЩЕЕ состояние, а не журнал
  // колбэков без потерь.
  size_t poll(std::span<observation> out);

  network::gns_status accept(network::gns_peer peer);
  network::gns_status close(network::gns_peer peer);

  send_result send(network::gns_peer peer, uint16_t lane, std::span<const std::byte> bytes);

  // Consumer(peer, lane, payload). Аренда освобождается сразу после возврата потребителя, поэтому
  // содержимое не переживает вызов: кому нужно дольше — копирует.
  template <class Consumer>
  size_t receive(Consumer&& consumer);

  uint64_t superseded() const noexcept {
    return superseded_;
  }

  struct conditions {
    bool available = false;
    int ping_ms = 0;
    float quality_local = 0, quality_remote = 0;
    int pending_reliable_bytes = 0;
  };
  conditions measure(network::gns_peer peer);

private:
  size_t receive_batch(void* consumer, void (*invoke)(void*, network::gns_peer, uint16_t,
                                                      std::span<const std::byte>));

  struct state;
  std::unique_ptr<state> state_;
  network::gns_peer pending_;
  uint64_t superseded_ = 0;
};

template <class Consumer>
size_t net_link::receive(Consumer&& consumer) {
  const auto invoke = [](void* context, const network::gns_peer peer, const uint16_t lane,
                         const std::span<const std::byte> payload) {
    (*static_cast<std::remove_reference_t<Consumer>*>(context))(peer, lane, payload);
  };
  return receive_batch(static_cast<void*>(&consumer), invoke);
}

} // namespace frontier_online::net

#endif
