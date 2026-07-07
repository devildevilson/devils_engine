#ifndef TILE_FRONTIER_CORE_MESSAGE_DISPATCHER_H
#define TILE_FRONTIER_CORE_MESSAGE_DISPATCHER_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <devils_engine/utils/actor_ref.h>

// Базовая реализация почтового ящика актора: mutex + vector.
// API намеренно оформлен так, чтобы выбранные диспетчеры позже можно было
// заменить на SPSC-очередь без правки вызывающего кода (send/consume_all/...).

namespace tile_frontier {
namespace core {

using namespace devils_engine;

struct message_dispatcher_stats {
  size_t sent = 0;
  size_t consumed = 0;
  size_t dropped = 0;
  size_t high_watermark = 0;
  size_t capacity = std::numeric_limits<size_t>::max();
};

template <typename T>
class message_dispatcher : public utils::message_reciever<T> {
public:
  message_dispatcher() noexcept = default;
  message_dispatcher(const size_t reserved, const size_t capacity = std::numeric_limits<size_t>::max()) noexcept :
    capacity(capacity)
  {
    messages.reserve(reserved);
  }

  utils::send_status send(T msg) override {
    const std::lock_guard l(mutex);
    if (messages.size() >= capacity) {
      stats_data.dropped += 1;
      return utils::send_status::mailbox_full;
    }

    messages.push_back(std::move(msg));
    on_sent(1);
    return utils::send_status::ok;
  }

  utils::send_status send(std::vector<T>& msg) override {
    if (msg.empty()) return utils::send_status::ok;

    const std::lock_guard l(mutex);
    const size_t available = capacity - messages.size();
    if (msg.size() > available) {
      stats_data.dropped += msg.size();
      return utils::send_status::backpressure;
    }

    const size_t count = msg.size();
    messages.insert(
      messages.end(),
      std::make_move_iterator(msg.begin()),
      std::make_move_iterator(msg.end())
    );
    msg.clear();
    on_sent(count);
    return utils::send_status::ok;
  }

  void reserve(const size_t count) {
    const std::lock_guard l(mutex);
    messages.reserve(count);
  }

  void set_capacity(const size_t value) {
    const std::lock_guard l(mutex);
    capacity = value;
    stats_data.capacity = value;
  }

  void consume_all(std::vector<T>& msg) {
    const std::lock_guard l(mutex);
    std::swap(messages, msg);
    stats_data.consumed += msg.size();
  }

  std::vector<T> consume_all() {
    std::vector<T> msg;
    consume_all(msg);
    return msg;
  }

  bool consume_last(std::vector<T>& cache) {
    consume_all(cache);
    if (cache.empty()) return false;
    return true;
  }

  size_t pending() const {
    const std::lock_guard l(mutex);
    return messages.size();
  }

  message_dispatcher_stats stats() const {
    const std::lock_guard l(mutex);
    return stats_data;
  }
private:
  mutable std::mutex mutex;
  std::vector<T> messages;
  size_t capacity = std::numeric_limits<size_t>::max();
  message_dispatcher_stats stats_data;

  void on_sent(const size_t count) noexcept {
    stats_data.sent += count;
    stats_data.high_watermark = std::max(stats_data.high_watermark, messages.size());
  }
};

template <typename T>
struct cached_message_dispatcher {
  message_dispatcher<T> dis;
  std::vector<T> cache;

  cached_message_dispatcher() noexcept = default;
  cached_message_dispatcher(const size_t reserved) noexcept : dis(reserved) {
    cache.reserve(reserved);
  }
};

template <typename T, typename F>
void dispatcher_consume(message_dispatcher<T>& dis, std::vector<T>& arr, F f) {
  dis.consume_all(arr);
  for (const auto& cmd : arr) { std::invoke(f, cmd); }
  arr.clear();
}

template <typename T, typename F>
void dispatcher_consume_last(message_dispatcher<T>& dis, std::vector<T>& arr, F f) {
  if (dis.consume_last(arr)) { std::invoke(f, arr.back()); }
  arr.clear();
}

template <typename T, typename F>
void dispatcher_consume(cached_message_dispatcher<T>& ced, F f) {
  dispatcher_consume<T>(ced.dis, ced.cache, std::move(f));
}

template <typename T, typename F>
void dispatcher_consume_last(cached_message_dispatcher<T>& ced, F f) {
  dispatcher_consume_last<T>(ced.dis, ced.cache, std::move(f));
}

}
}

#endif
