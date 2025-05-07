#ifndef DEVILS_ENGINE_UTILS_SHARED_DATA_SYSTEM_H
#define DEVILS_ENGINE_UTILS_SHARED_DATA_SYSTEM_H

#include <cstdint>
#include <cstddef>
#include <shared_mutex>

namespace devils_engine {
namespace utils {

// таких систем будет видимо пара штук, например для графики и для звука

template <typename... Args>
class shared_data_system {
public:
  constexpr const size_t args_count = sizeof...(Args);

  shared_data_system(Args&&... args) noexcept : buffers(std::make_tuple(std::forward<Args>(args)...)) {}

  // set? да поди повторить почти полностью интерфейс

  template <size_t N>
  const auto & get() const {
    return std::get<N>(buffers);
  }

  template <size_t N>
  auto& get() {
    return std::get<N>(buffers);
  }

  void swap() {
    std::lock_guard<std::shared_mutex> l(mutex);
    swap<0>();
  }

  void consume() {
    std::lock_guard<std::shared_mutex> l(mutex);
    consume<0>();
  }
private:
  mutable std::shared_mutex mutex;
  std::tuple<Args...> buffers;

  template <size_t N>
  void swap() {
    if constexpr (N >= args_count) return;
    std::get<N>(buffers).swap();
    swap<N+2>();
  }

  template <size_t N>
  void consume() {
    if constexpr (N >= args_count) return;
    std::get<N>(buffers).call(std::get<N+1>(buffers));
    consume<N+2>();
  }
};
}
}

#endif