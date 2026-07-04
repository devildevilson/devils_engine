#ifndef TILE_FRONTIER_CORE_WRITE_BUFFER_CHANNEL_H
#define TILE_FRONTIER_CORE_WRITE_BUFFER_CHANNEL_H

#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include <devils_engine/thread/payload_channel.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// POD-сообщение канала записи буферов: имя-хеш ресурса + позиция/размер payload'а в арене (поля
// pos/size — контракт thread::payload_channel). Имя резолвится хешем на рендере.
struct wb_msg {
  uint64_t name_hash;
  int64_t  pos;
  uint32_t size;
};

// Доменная обёртка над обобщённым thread::payload_channel<wb_msg> (main → render): удобные
// продюсер-хелперы write(hash, ...) и drain. Семантика — latest-wins на буфер (кадровые данные):
// overflow ⇒ drop (устаревание на кадр допустимо). Бюджеты фиксируются конструктором.
struct write_buffer_channel {
  thread::payload_channel<wb_msg> chan;

  write_buffer_channel(const size_t msg_capacity, const size_t arena_bytes)
    : chan(msg_capacity, arena_bytes) {}

  // ПРОДЮСЕР. Одиночный payload.
  bool write(const uint64_t name_hash, const std::span<const uint8_t> data) {
    return chan.write(data.size(), [&](std::span<std::byte> region, const int64_t pos) {
      std::memcpy(region.data(), data.data(), data.size());
      return wb_msg{ name_hash, pos, static_cast<uint32_t>(data.size()) };
    });
  }

  // ПРОДЮСЕР. Scatter a‖b в один payload (напр. [count]‖[тело] для ui_commands), без temp-буфера.
  bool write(const uint64_t name_hash, const std::span<const uint8_t> a, const std::span<const uint8_t> b) {
    return chan.write(a.size() + b.size(), [&](std::span<std::byte> region, const int64_t pos) {
      std::memcpy(region.data(), a.data(), a.size());
      std::memcpy(region.data() + a.size(), b.data(), b.size());
      return wb_msg{ name_hash, pos, static_cast<uint32_t>(a.size() + b.size()) };
    });
  }

  // КОНСЬЮМЕР. handler(const wb_msg&, std::span<const std::byte> payload).
  template <typename Handler>
  void drain(Handler&& handler) { chan.drain(std::forward<Handler>(handler)); }
};

}
}

#endif
