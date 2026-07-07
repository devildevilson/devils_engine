#ifndef DEVILS_ENGINE_SIMUL_WRITE_BUFFER_CHANNEL_H
#define DEVILS_ENGINE_SIMUL_WRITE_BUFFER_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include <devils_engine/thread/payload_channel.h>

namespace devils_engine {
namespace simul {

struct wb_msg {
  uint64_t name_hash;
  int64_t  pos;
  uint32_t size;
};

struct write_buffer_channel {
  thread::payload_channel<wb_msg> chan;

  write_buffer_channel(const size_t msg_capacity, const size_t arena_bytes)
    : chan(msg_capacity, arena_bytes) {}

  bool write(const uint64_t name_hash, const std::span<const uint8_t> data) {
    return chan.write(data.size(), [&](std::span<std::byte> region, const int64_t pos) {
      if (!data.empty()) std::memcpy(region.data(), data.data(), data.size());
      return wb_msg{ name_hash, pos, static_cast<uint32_t>(data.size()) };
    });
  }

  bool write(const uint64_t name_hash, const std::span<const uint8_t> a, const std::span<const uint8_t> b) {
    return chan.write(a.size() + b.size(), [&](std::span<std::byte> region, const int64_t pos) {
      if (!a.empty()) std::memcpy(region.data(), a.data(), a.size());
      if (!b.empty()) std::memcpy(region.data() + a.size(), b.data(), b.size());
      return wb_msg{ name_hash, pos, static_cast<uint32_t>(a.size() + b.size()) };
    });
  }

  template <typename Handler>
  void drain(Handler&& handler) { chan.drain(std::forward<Handler>(handler)); }
};

}
}

#endif
