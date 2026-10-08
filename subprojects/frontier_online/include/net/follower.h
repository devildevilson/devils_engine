#ifndef FRONTIER_ONLINE_NET_FOLLOWER_H
#define FRONTIER_ONLINE_NET_FOLLOWER_H

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "net/link.h"
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

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace frontier_online::net

#endif
