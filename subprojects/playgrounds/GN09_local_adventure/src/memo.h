#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>

namespace gn09 {

// Общий кэш с одним строителем на ключ (как у станций): ожидающие получают тот же
// результат, давно не нужные готовые записи вытесняются.
template <typename Value>
class memo {
public:
  explicit memo(const size_t capacity) : capacity_(capacity) {}
  template <typename Build>
  Value get(const std::array<uint64_t, 3>& key, Build&& build) {
    std::promise<Value> promise;
    std::shared_future<Value> future;
    bool owner = false;
    {
      std::lock_guard guard(lock_);
      const auto found = cache_.find(key);
      if (found != cache_.end()) {
        future = found->second.future;
        found->second.used = ++tick_;
      } else {
        if (cache_.size() >= capacity_) {
          auto oldest = cache_.end();
          for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            if (oldest == cache_.end() || it->second.used < oldest->second.used) oldest = it;
          }
          if (oldest != cache_.end()) cache_.erase(oldest);
        }
        future = promise.get_future().share();
        cache_.emplace(key, entry{future, ++tick_});
        owner = true;
      }
    }
    if (owner) promise.set_value(build());
    return future.get();
  }
private:
  struct entry { std::shared_future<Value> future; uint64_t used = 0; };
  size_t capacity_;
  std::mutex lock_;
  std::map<std::array<uint64_t, 3>, entry> cache_;
  uint64_t tick_ = 0;
};

} // namespace gn09
