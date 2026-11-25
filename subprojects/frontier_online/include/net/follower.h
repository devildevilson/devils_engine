#ifndef FRONTIER_ONLINE_NET_FOLLOWER_H
#define FRONTIER_ONLINE_NET_FOLLOWER_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "net/link.h"
#include "net/network_settings.h"
#include "net/protocol.h"

// КЛИЕНТ ПРИСОЕДИНЯЕТСЯ.
//
// Порядок обратный авторитетскому и с одной важной особенностью: клиент проверяет совместимость
// САМ, а не ждёт отказа. Иначе он ответил бы на вызов установки, с которой всё равно не сможет
// считать один мир, и узнал бы причину только из чужого отказа — а этого может и не случиться,
// если соединение просто оборвётся.
//
// Земля не едет. Клиент получает ПАРАМЕТРЫ мира и считает его сам, после чего сверяет две
// величины: отпечаток генератора (тот же ли конфиг) и корень пробного чанка (та же ли сборка
// считает по этому конфигу). Только потом принимается состояние.

namespace frontier_online::net {

// Приёмник причинного состояния. Владелец решает, куда его класть, и возвращает корень того, что
// получилось: сравнивать корень обязана сторона, которая его ПОСЧИТАЛА заново, а не пересказала.
class checkpoint_sink {
public:
  virtual ~checkpoint_sink() = default;
  [[nodiscard]] virtual bool load(const checkpoint_begin& header,
                                  std::span<const std::byte> document, uint64_t& root) = 0;
};

enum class follower_phase : uint8_t {
  connecting,
  handshaking,
  awaiting_world,
  receiving_state,
  joined,
  failed,
};

// Почему ведомый прекратил сессию по СВОЕЙ инициативе. Локальная причина, на провод не идёт:
// авторитету о неудачном присоединении сообщает `join_report`.
enum class follower_fault : uint8_t {
  none,
  connect_timeout,      // соединение не установилось в срок
  handshake_timeout,    // рукопожатие не завершилось в срок
  world_timeout,        // объявление мира не пришло в срок
  transfer_stalled,     // передача состояния остановилась: нет новых байт дольше срока
  confirmation_timeout, // после отчёта нет подтверждения присоединения
  live_silence,         // живая сессия: авторитет молчит дольше срока
  connection_closed,    // авторитет закрыл соединение
  protocol_violation,   // авторитет прислал сообщение не своей фазы
  insane_authority,     // авторитет прислал противоречивое: session 0, тик состояния не тот
  terminated,           // авторитет прекратил обслуживание и назвал причину (см. termination())
};

[[nodiscard]] const char* describe(follower_fault fault) noexcept;

// Заявка на продолжение: ключ, выданный авторитетом (`reconnect_grant`), и подтверждённое состояние
// клиента — тик и корень после него. Авторитет догонит от него или от снимка своего кольца.
struct resume_claim {
  reconnect_grant grant;
  uint64_t tick = 0;
  uint64_t root = 0;
};

struct follower_config {
  uint32_t host = 0x7f000001;
  uint16_t port = 0;
  bool verbose = true;
  // Испортить одно поле совместимости НАРОЧНО. Существует ради проверки, что отказ приходит с
  // ПРИЧИНОЙ: «другая сборка» не должна значить «молча не работает».
  uint32_t protocol_version_override = 0;
  bool corrupt_content_root = false;
  bool corrupt_intent_schema = false;
  bool corrupt_component_schema = false;

  // СРОКИ — базовая проверка на вшивость авторитета: сервер, который замолчал посреди фазы, не
  // должен держать клиента вечно. 0 — срока нет. Авторитет шлёт bundle каждый тик, поэтому
  // секунды тишины в живой сессии — это уже беда, а не задержка.
  // Умолчания — из network_settings.h (один источник); `network.tavl` переопределяет через apply().
  std::chrono::milliseconds connect_timeout{default_network_settings.follower.connect_ms};
  std::chrono::milliseconds handshake_timeout{default_network_settings.follower.handshake_ms};
  std::chrono::milliseconds world_timeout{default_network_settings.follower.world_ms};
  std::chrono::milliseconds transfer_stall_timeout{default_network_settings.follower.transfer_stall_ms};
  std::chrono::milliseconds confirmation_timeout{default_network_settings.follower.confirmation_ms};
  std::chrono::milliseconds live_silence_timeout{default_network_settings.follower.live_silence_ms};

  // Есть — рукопожатие с продолжением той же сессии тем же участником; нет — новый вход.
  std::optional<resume_claim> resume;

  // Сколько канонических тиков ведомый держит непрочитанными. Это ёмкость, а не предел ошибки:
  // заполнив её, ведомый перестаёт забирать сообщения из транспорта, и они ждут в очереди GNS.
  size_t canonical_capacity = 32;

  void apply(const network_settings& settings) noexcept {
    connect_timeout = std::chrono::milliseconds(settings.follower.connect_ms);
    handshake_timeout = std::chrono::milliseconds(settings.follower.handshake_ms);
    world_timeout = std::chrono::milliseconds(settings.follower.world_ms);
    transfer_stall_timeout = std::chrono::milliseconds(settings.follower.transfer_stall_ms);
    confirmation_timeout = std::chrono::milliseconds(settings.follower.confirmation_ms);
    live_silence_timeout = std::chrono::milliseconds(settings.follower.live_silence_ms);
  }
};

class follower {
public:
  follower(net_runtime& runtime, const core::causal_content& content, follower_config config);
  ~follower();
  follower(const follower&) = delete;
  follower& operator=(const follower&) = delete;

  [[nodiscard]] bool start();
  void step(checkpoint_sink& sink);
  // Дослать и дождаться. `try_send` только КОПИРУЕТ байты в слот — уходят они позже, на прокачке.
  // Процесс, вышедший сразу после «присоединился», унёс бы свой отчёт с собой, и авторитет
  // записал бы пропавшего участника. Возвращает true, когда авторитет закрыл соединение, то есть
  // отчёт дошёл.
  [[nodiscard]] bool drain();
  // Уйти самому (только в живой сессии): авторитет освобождает слот сразу и закрывает соединение;
  // дождаться закрытия — drain(). false — уходить не из чего.
  [[nodiscard]] bool leave();

  [[nodiscard]] follower_phase phase() const noexcept;
  [[nodiscard]] bool finished() const noexcept;
  [[nodiscard]] const join_report& report() const noexcept;
  [[nodiscard]] network::session_refusal_reason refusal() const noexcept;
  [[nodiscard]] const world_declaration& world() const noexcept;
  [[nodiscard]] uint64_t session() const noexcept;
  [[nodiscard]] uint64_t peer_id() const noexcept;
  [[nodiscard]] uint32_t received_bytes() const noexcept;
  [[nodiscard]] uint32_t expected_bytes() const noexcept;

  // After join, the same GNS connection carries unreliable proposals upstream and
  // reliable canonical ticks downstream. Returned bytes own their receive storage.
  [[nodiscard]] send_result send_intent(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::vector<std::byte>> take_canonical_ticks();
  [[nodiscard]] bool live_ready() const noexcept;
  [[nodiscard]] bool live_terminal() const noexcept;
  [[nodiscard]] bool live_fault() const noexcept;
  [[nodiscard]] follower_fault fault() const noexcept;
  // Причина, которую авторитет назвал в `session_terminated`, если назвал.
  [[nodiscard]] std::optional<termination_reason> termination() const noexcept;
  [[nodiscard]] uint64_t catchup_tick() const noexcept;
  // Рукопожатие уже идёт (транспорт соединился) / сессия установлена.
  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] bool established() const noexcept;
  // Последний ключ повторного входа от авторитета (после подтверждения; обновляется).
  [[nodiscard]] const std::optional<reconnect_grant>& grant() const noexcept;
  // План продолжения, если это рукопожатие с продолжением и план пришёл.
  [[nodiscard]] const std::optional<resume_plan>& plan() const noexcept;

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace frontier_online::net

#endif
