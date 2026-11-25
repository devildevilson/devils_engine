#ifndef FRONTIER_ONLINE_NET_AUTHORITY_H
#define FRONTIER_ONLINE_NET_AUTHORITY_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "net/link.h"
#include "net/network_settings.h"
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

// Подтверждённое клиентом состояние, с которого он просит продолжить: тик и корень после него.
struct resume_anchor {
  uint64_t tick = 0;
  uint64_t root = 0;
};

// Снимок причинного состояния на границе тика. Интерфейс, а не std::function, по той же причине,
// что и `tick_tail_reader`: у снимка есть владелец со своими буферами, и он тут виден.
class checkpoint_source {
public:
  virtual ~checkpoint_source() = default;
  // Заполнить заголовок и документ. false = снять сейчас нельзя (это не ошибка протокола).
  [[nodiscard]] virtual bool capture(checkpoint_begin& header,
                                     std::vector<std::byte>& document) = 0;
  // Откуда догонять вернувшегося участника. `journal`: якорь найден в истории, `header` — его тик
  // и корень, документ пуст. `checkpoint`: снимок K из кольца и тики K+1..N в истории. false —
  // догнать не из чего (тогда вход только новый). Источник без истории (только присоединение)
  // продолжений не обслуживает.
  [[nodiscard]] virtual bool resume(uint64_t logical_peer, const std::optional<resume_anchor>& anchor,
                                    resume_mode& mode, checkpoint_begin& header,
                                    std::vector<std::byte>& document) {
    static_cast<void>(logical_peer);
    static_cast<void>(anchor);
    static_cast<void>(mode);
    static_cast<void>(header);
    static_cast<void>(document);
    return false;
  }
};

struct authority_config {
  // 0.0.0.0 — все интерфейсы, но только при объявленном порте. При port == 0 (тесты в одном
  // процессе) host == 0 означает петлю: незачем открывать наружу порт, который знает только тест.
  uint32_t host = 0;
  uint16_t port = 0;    // 0 — свободный порт (закреплённая GNS не принимает 0: перебор диапазона)
  uint32_t max_peers = 8;
  bool verbose = true;
  bool keep_joined_connection = false; // the join-only harness still closes after its report

  // СРОКИ ФАЗ. Участник, не дошедший до следующей фазы вовремя, закрывается: молчащее
  // соединение иначе держит слот вечно (keepalive транспорта его не рвёт). 0 — срока нет.
  // Срок отчёта растёт с размером состояния: 5 с плюс время передачи на объявленной
  // минимальной скорости. Живая сессия своего срока тишины не имеет: участник без ввода
  // законен, мёртвое соединение закрывает транспорт, а нечитающий отстаёт за историю.
  // Умолчания — из network_settings.h (один источник); `network.tavl` переопределяет через apply().
  std::chrono::milliseconds hello_timeout{default_network_settings.authority.hello_ms};
  std::chrono::milliseconds response_timeout{default_network_settings.authority.response_ms};
  std::chrono::milliseconds report_timeout{default_network_settings.authority.report_ms};
  uint32_t report_min_bytes_per_second = default_network_settings.authority.report_min_bytes_per_second;

  // НАРУШЕНИЯ И КВОТЫ — на участника, а не на всех: общий бюджет позволил бы одному
  // заглушить остальных. Сообщение сверх квоты не разбирается и считается нарушением.
  uint32_t violation_limit = 16;
  uint32_t messages_per_step = 32;
  uint32_t intents_per_step = 4;

  // Сколько живёт соединение после последнего слова авторитета (отказа): close без linger
  // съел бы саму причину.
  std::chrono::milliseconds close_linger{default_network_settings.authority.close_linger_ms};

  // ПОВТОРНЫЙ ВХОД С ПРОДОЛЖЕНИЕМ. Обрыв, срок фазы или отставание за историю у участника в мире
  // не убирают его игрока сразу: слот держится `hold_window` (игрок стоит без вводов и занимает
  // место в `max_peers`), и вернувшийся с ключом остаётся тем же игроком. Нарушения, несогласие и
  // shutdown слот освобождают сразу. Билет живёт `ticket_lifetime` и переиздаётся на половине.
  std::chrono::milliseconds hold_window{default_network_settings.authority.hold_ms};
  std::chrono::milliseconds ticket_lifetime{default_network_settings.authority.ticket_ms};

  void apply(const network_settings& settings) noexcept {
    hello_timeout = std::chrono::milliseconds(settings.authority.hello_ms);
    response_timeout = std::chrono::milliseconds(settings.authority.response_ms);
    report_timeout = std::chrono::milliseconds(settings.authority.report_ms);
    report_min_bytes_per_second = settings.authority.report_min_bytes_per_second;
    close_linger = std::chrono::milliseconds(settings.authority.close_linger_ms);
    hold_window = std::chrono::milliseconds(settings.authority.hold_ms);
    ticket_lifetime = std::chrono::milliseconds(settings.authority.ticket_ms);
  }
};

struct authority_join {
  uint64_t peer_id = 0;
  join_report report;
  bool agreed = false; // корень клиента совпал с объявленным
  bool resumed = false; // вернулся тем же участником (продолжение), а не вошёл заново
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
  // Опознание процесса авторитета (лог, `listen.session`). У каждого участника своя сессия —
  // `session_of`: на неё выдаётся ключ повторного входа.
  [[nodiscard]] uint64_t session() const noexcept;
  // Сессия участника в мире (живого или удерживаемого); 0 — такого нет.
  [[nodiscard]] uint64_t session_of(uint64_t logical_peer) const noexcept;

  // Один круг обслуживания.
  void step(checkpoint_source& source);

  [[nodiscard]] uint32_t connected_peers() const noexcept;
  [[nodiscard]] uint32_t completed_joins() const noexcept;
  // Согласованные продолжения (вернулся тот же участник); в completed_joins не входят.
  [[nodiscard]] uint32_t resumed_joins() const noexcept;
  [[nodiscard]] uint32_t refused_joins() const noexcept;
  // Записи присоединения живых участников (ушедшие выбрасываются, когда журнал упирается в предел
  // 4 * max_peers + 16). Число согласованных присоединений за всё время — completed_joins().
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
  // Слот участника удерживается после обрыва: его игрок остаётся в мире, канонического ухода нет.
  // Истёкшее окно снимается на круге `step`.
  [[nodiscard]] bool is_held(uint64_t logical_peer) const noexcept;
  // Нарушение, найденное ПОТРЕБИТЕЛЕМ (например, непринятое предложение). Считается туда же,
  // куда и транспортные; порог закрывает участника.
  void note_violation(uint64_t logical_peer);
  // Прекратить обслуживание участника с причиной (например, отстал за историю).
  void terminate(uint64_t logical_peer, termination_reason reason);
  // Последние исходы (не больше 4 * max_peers + 16); число всех — termination_count().
  [[nodiscard]] const std::vector<authority_termination>& terminations() const noexcept;
  [[nodiscard]] uint64_t termination_count() const noexcept;
  [[nodiscard]] uint64_t violations() const noexcept;

  void close_all();

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace frontier_online::net

#endif
