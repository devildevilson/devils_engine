#ifndef TILE_FRONTIER_CORE_WRITE_BUFFER_CHANNEL_H
#define TILE_FRONTIER_CORE_WRITE_BUFFER_CHANNEL_H

#include <cstdint>
#include <cstring>
#include <span>

#include <devils_engine/thread/spsc_queue.h>
#include <devils_engine/thread/byte_ring.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// POD-сообщение канала записи буферов: имя-хеш ресурса + позиция/размер payload'а в арене канала.
// Никаких std::string/std::vector — payload лежит в byte_ring, имя резолвится хешем на рендере.
struct wb_msg {
  uint64_t name_hash;
  int64_t  pos;   // монотонная позиция в арене (см. byte_ring)
  uint32_t size;
};

// Первый живой SPSC-канал брокера (main → render), вертикальный срез: spsc_queue<wb_msg> +
// byte_ring под сырые байты. Бюджеты ФИКСИРОВАНЫ конструктором (преаллокация, рантайм-роста нет).
// Семантика write_buffer — latest-wins на буфер (кадровые данные): overflow ⇒ drop (устаревание
// на кадр допустимо). SPSC: единственный продюсер — main, единственный консьюмер — render.
struct write_buffer_channel {
  thread::spsc_queue<wb_msg> queue;
  thread::byte_ring arena;

  write_buffer_channel(const size_t msg_capacity, const size_t arena_bytes)
    : queue(msg_capacity), arena(arena_bytes) {}

  // ПРОДЮСЕР (main). true — принято; false — drop (очередь/арена переполнены). Порядок важен:
  // сначала проверяем очередь (single-producer ⇒ !full ⇒ push гарантирован), потом alloc — иначе
  // при неудачном push место в арене утекло бы (консьюмер не узнал бы pos и не сделал release).
  bool write(const uint64_t name_hash, const std::span<const uint8_t> data) {
    if (queue.full()) return false;
    std::span<std::byte> region;
    const int64_t pos = arena.alloc(data.size(), region);
    if (pos < 0) return false;
    std::memcpy(region.data(), data.data(), data.size());
    return queue.try_push(wb_msg{ name_hash, pos, static_cast<uint32_t>(data.size()) });
  }

  // Scatter-запись: склеивает a‖b в один непрерывный payload (напр. [count]‖[тело] для ui_commands),
  // без промежуточного буфера на стороне продюсера.
  bool write(const uint64_t name_hash, const std::span<const uint8_t> a, const std::span<const uint8_t> b) {
    if (queue.full()) return false;
    const size_t total = a.size() + b.size();
    std::span<std::byte> region;
    const int64_t pos = arena.alloc(total, region);
    if (pos < 0) return false;
    std::memcpy(region.data(), a.data(), a.size());
    std::memcpy(region.data() + a.size(), b.data(), b.size());
    return queue.try_push(wb_msg{ name_hash, pos, static_cast<uint32_t>(total) });
  }
};

}
}

#endif
