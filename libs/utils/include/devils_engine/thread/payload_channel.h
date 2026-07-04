#ifndef DEVILS_ENGINE_THREAD_PAYLOAD_CHANNEL_H
#define DEVILS_ENGINE_THREAD_PAYLOAD_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "spsc_queue.h"
#include "byte_ring.h"

namespace devils_engine {
namespace thread {

// Reliable-FIFO канал брокера с payload'ом: spsc_queue<Msg> (синхронизация) + byte_ring (память
// payload'а, FIFO-reclaim курсором). Бюджеты ФИКСИРОВАНЫ конструктором (преаллокация, роста нет).
// SPSC: один продюсер, один консьюмер.
//
// Msg — POD-сообщение, ДОЛЖНО нести поля `int64_t pos; uint32_t size;` (позиция и размер payload'а
// в арене). Всё остальное в Msg — доменное (напр. name_hash / tag) и задаётся в fill-колбэке.
template <typename Msg>
class payload_channel {
public:
  payload_channel(const size_t msg_capacity, const size_t arena_bytes)
    : queue_(msg_capacity), arena_(arena_bytes) {}

  // ПРОДЮСЕР. Зарезервировать size байт, вызвать fill(region, pos) -> Msg (заполнить байты и собрать
  // Msg с pos/size), запушить Msg. true — принято; false — drop (очередь/арена переполнены).
  // ПОРЯДОК важен: сначала full-check очереди (single-producer ⇒ !full ⇒ push потом гарантирован),
  // потом alloc — иначе при неудачном push место в арене утекло бы (консьюмер не сделал бы release).
  // fill может делать несколько memcpy в region (scatter) — layout payload'а на усмотрение вызова.
  template <typename Fill>
  bool write(const size_t size, Fill&& fill) {
    if (queue_.full()) return false;
    std::span<std::byte> region;
    const int64_t pos = arena_.alloc(size, region);
    if (pos < 0) return false;
    Msg msg = fill(region, pos);
    return queue_.try_push(std::move(msg));
  }

  // КОНСЬЮМЕР. Дренит все сообщения по порядку: handler(const Msg&, std::span<const std::byte>
  // payload), затем FIFO-release. handler ОБЯЗАН потребить payload синхронно — он валиден только до
  // release (нужен дольше — скопировать наружу).
  template <typename Handler>
  void drain(Handler&& handler) {
    Msg m;
    while (queue_.try_pop(m)) {
      handler(static_cast<const Msg&>(m), arena_.at(m.pos, m.size));
      arena_.release(m.pos + m.size);
    }
  }

  size_t arena_used_approx() const noexcept { return arena_.used_approx(); }
  size_t queue_size_approx() const noexcept { return queue_.size_approx(); }

private:
  spsc_queue<Msg> queue_;
  byte_ring arena_;
};

}
}

#endif
