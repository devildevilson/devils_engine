#ifndef DEVILS_ENGINE_UTILS_PERF_H
#define DEVILS_ENGINE_UTILS_PERF_H

#include <chrono>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace devils_engine {
namespace utils {
// perf — замер времени одного вызова. Зовёт fn(args...) через std::invoke и ВОЗВРАЩАЕТ замер
// рядом с результатом; что с ним делать (логировать/аккумулировать/игнорить) решает вызывающий.
//   void-возврат      → возвращает длительность (perf_clock::duration);
//   не-void возврат    → std::tuple<RetT, perf_clock::duration>.
// Лейбла нет намеренно — это инструмент, а не логгер. Длительность отдаётся в нативных тиках
// часов; вызывающий сам делает duration_cast в нужные единицы.
using perf_clock = std::chrono::steady_clock;

template <typename F, typename... Args>
[[nodiscard]] auto perf(F&& fn, Args&&... args) {
  using ret_t = std::invoke_result_t<F, Args...>;
  const auto t0 = perf_clock::now();
  if constexpr (std::is_void_v<ret_t>) {
    std::invoke(std::forward<F>(fn), std::forward<Args>(args)...);
    return perf_clock::now() - t0;
  } else {
    ret_t r = std::invoke(std::forward<F>(fn), std::forward<Args>(args)...);
    const auto dt = perf_clock::now() - t0;
    return std::tuple<ret_t, perf_clock::duration>(std::move(r), dt);
  }
}

}
}

#endif
