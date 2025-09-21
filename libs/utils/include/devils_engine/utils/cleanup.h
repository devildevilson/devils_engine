#ifndef DEVILS_ENGINE_UTILS_CLEANUP_H
#define DEVILS_ENGINE_UTILS_CLEANUP_H

#include <type_traits>
#include <tuple>
#include <functional>

namespace devils_engine {
namespace utils {

template <typename Callback, typename... Args>
  requires(std::is_void_v<std::invoke_result_t<Callback, Args...>>)
class cleanup {
public:
  cleanup(Callback callback, Args&&... args) noexcept : callback(std::move(callback)), args(std::forward<Args>(args)...) {}
  ~cleanup() noexcept { invoke(); }

  cleanup(const cleanup &copy) noexcept = delete;
  cleanup(cleanup &&move) noexcept = default;
  cleanup & operator=(const cleanup &copy) noexcept = delete;
  cleanup & operator=(cleanup &&move) noexcept = default;

  void cancel() noexcept { callback = nullptr; }
  void invoke() noexcept {
    if (!callback) return;
    std::apply(callback, args);
    cancel();
  }

  operator bool () { return bool(callback); }
private:
  std::function<void(Args...)> callback;
  std::tuple<Args...> args;
};

}
}

#endif
