#ifndef FRONTIER_ONLINE_NET_AUTHORITY_H
#define FRONTIER_ONLINE_NET_AUTHORITY_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "net/link.h"
#include "net/protocol.h"

// АВТОРИТЕТ СЛУШАЕТ.
//
// Обслуживание присоединения и симуляция — разные вещи: `step()` принимает байты, но сам тик ему
// не принадлежит. Обычный join снимает checkpoint при продолжающемся мире; live consumer сам
// хранит нужные bundles для догоняющего клиента. После join этот объект маршрутизирует bounded
// intent packets и canonical ticks, не исполняя project gameplay.
//
// Что происходит с присоединяющимся, по порядку:
//   1. рукопожатие (`network::session_wire`) — совместимость проверяется ДО единого байта состояния;
//   2. объявление мира — параметры земли, которую клиент посчитает сам;
//   3. чек-пойнт — полное причинное состояние на тике K, кусками по объёмной полосе;
//   4. отчёт клиента — каким корнем обернулась загрузка.
//
// Шаг 4 существует затем, чтобы сравнение двух процессов делал АВТОРИТЕТ. Человек, сличающий два
// числа в двух окнах, — это не проверка.

namespace frontier_online::net {

// Снимок причинного состояния на границе тика. Интерфейс, а не std::function, по той же причине,
// что и `tick_tail_reader`: у снимка есть владелец со своими буферами, и он тут виден.
class checkpoint_source {
public:
  virtual ~checkpoint_source() = default;
  // Заполнить заголовок и документ. false = снять сейчас нельзя (это не ошибка протокола).
  [[nodiscard]] virtual bool capture(checkpoint_begin& header,
                                     std::vector<std::byte>& document) = 0;
};

struct authority_config {
  uint32_t host = 0;    // 0.0.0.0 — слушать на всех интерфейсах
  uint16_t port = 0;    // 0 — спросить порт у операционной системы
  uint32_t max_peers = 8;
  bool verbose = true;
  bool keep_joined_connection = false; // the join-only harness still closes after its report

  // СРОКИ ФАЗ. Участник, не дошедший до следующей фазы вовремя, закрывается: молчащее
  // соединение иначе держит слот вечно (keepalive транспорта его не рвёт). 0 — срока нет.
  // Срок отчёта растёт с размером состояния: 5 с плюс время передачи на объявленной
  // минимальной скорости. Живая сессия своего срока тишины не имеет: участник без ввода
  // законен, мёртвое соединение закрывает транспорт, а нечитающий отстаёт за историю.
  std::chrono::milliseconds hello_timeout{2000};
  std::chrono::milliseconds response_timeout{2000};
  std::chrono::milliseconds report_timeout{5000};
  uint32_t report_min_bytes_per_second = 64 * 1024;

  // НАРУШЕНИЯ И КВОТЫ — на участника, а не на всех: общий бюджет позволил бы одному
  // заглушить остальных. Сообщение сверх квоты не разбирается и считается нарушением.
  uint32_t violation_limit = 16;
  uint32_t messages_per_step = 32;
  uint32_t intents_per_step = 4;

  // Сколько живёт соединение после последнего слова авторитета (отказа): close без linger
  // съел бы саму причину.
  std::chrono::milliseconds close_linger{500};
};

struct authority_join {
  uint64_t peer_id = 0;
  join_report report;
  bool agreed = false; // корень клиента совпал с объявленным
};

// Почему и кого авторитет закрыл после рукопожатия. Журнал исходов, а не счётчик: «кто-то
// отвалился» не объясняет, кто и за что.
struct authority_termination {
  uint64_t peer_id = 0;
  termination_reason reason = termination_reason::violations;
};

// Owned bytes: the GNS receive lease ends before the simulation admits the proposal.
struct live_intent_packet {
  uint64_t logical_peer = 0;
  std::vector<std::byte> bytes;
};

class authority {
public:
  authority(net_runtime& runtime, const core::causal_content& content,
            const world_declaration& world, authority_config config);
  ~authority();
  authority(const authority&) = delete;
  authority& operator=(const authority&) = delete;

  [[nodiscard]] uint16_t port() const noexcept;
  [[nodiscard]] uint64_t session() const noexcept;

  // Один круг обслуживания.
  void step(checkpoint_source& source);

  [[nodiscard]] uint32_t connected_peers() const noexcept;
  [[nodiscard]] uint32_t completed_joins() const noexcept;
  [[nodiscard]] uint32_t refused_joins() const noexcept;
  [[nodiscard]] const std::vector<authority_join>& joins() const noexcept;
  // Байт состояния, отправленных по объёмной полосе. Цена присоединения, измеренная, а не
  // предположенная.
  [[nodiscard]] uint64_t checkpoint_bytes_sent() const noexcept;

  // The established connection is only a route. Its logical peer is assigned by the
  // handshake and never read from an intent packet. Both calls are owner-thread only.
  [[nodiscard]] std::vector<live_intent_packet> take_live_intents();
  [[nodiscard]] send_result send_canonical(uint64_t logical_peer,
                                           std::span<const std::byte> bytes);
  [[nodiscard]] send_result send_catchup(uint64_t logical_peer, uint64_t tick);
  [[nodiscard]] uint64_t dropped_live_intents() const noexcept;

  // Участник присоединился, подтверждён и всё ещё обслуживается.
  [[nodiscard]] bool is_live_member(uint64_t logical_peer) const noexcept;
  // Нарушение, найденное ПОТРЕБИТЕЛЕМ (например, непринятое предложение). Считается туда же,
  // куда и транспортные; порог закрывает участника.
  void note_violation(uint64_t logical_peer);
  // Прекратить обслуживание участника с причиной (например, отстал за историю).
  void terminate(uint64_t logical_peer, termination_reason reason);
  [[nodiscard]] const std::vector<authority_termination>& terminations() const noexcept;
  [[nodiscard]] uint64_t violations() const noexcept;

  void close_all();

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace frontier_online::net

#endif
