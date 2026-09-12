#ifndef FRONTIER_ONLINE_NET_AUTHORITY_H
#define FRONTIER_ONLINE_NET_AUTHORITY_H

#include <cstdint>
#include <memory>
#include <vector>

#include "net/link.h"
#include "net/protocol.h"

// АВТОРИТЕТ СЛУШАЕТ.
//
// Обслуживание присоединения и симуляция — разные вещи, и здесь только первая: `step()` зовётся
// раз в тик, но сам тик ей не принадлежит. Авторитет не останавливает мир ради присоединяющегося
// и не ждёт его: он снимает чек-пойнт на границе тика и отдаёт этот СНИМОК, продолжая считать.
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
};

struct authority_join {
  uint64_t peer_id = 0;
  join_report report;
  bool agreed = false; // корень клиента совпал с объявленным
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

  void close_all();

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace frontier_online::net

#endif
