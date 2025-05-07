#ifndef DEVILS_ENGINE_THREAD_ATOMIC_POOL_H
#define DEVILS_ENGINE_THREAD_ATOMIC_POOL_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <stop_token>
#include <thread>
#include <vector>
#include <functional>
#include <future>
#include <memory>

#include "atomic_queue/atomic_queue.h"

// по идее это должна быть версия куда быстрее чем предыдущая
// + потоки тут почти всегда в работе что с одной стороны хуже
// с другой стороны лучше если в течении работы приложения они всегда будут чем то заняты

#ifndef DEVILS_THREAD_ATOMIC_QUEUE_CAPACITY
  #define DEVILS_THREAD_ATOMIC_QUEUE_CAPACITY 128
#endif

namespace devils_engine {
namespace thread {

// для atomic_queue нужно посчитать максимальное количество задач заранее
class atomic_pool {
public:
  using task_t = std::function<void()>;
  using atomic_queue = atomic_queue::AtomicQueue2<task_t, DEVILS_THREAD_ATOMIC_QUEUE_CAPACITY>;

  atomic_pool(const size_t thread_count);
  ~atomic_pool() noexcept;

  void submitbase(task_t f) noexcept;

  template<class F, class... Args>
  void submit(F&& f, Args&&... args) noexcept {
    task_t task = [f = std::move(f), largs = std::make_tuple(std::forward<Args>(args)...)] () mutable {
      return std::apply(std::move(f), std::move(largs));
    };

    submitbase(std::move(task));
  }

  template<class F, class... Args>
  auto submitf(F&& f, Args&&... args) noexcept -> std::future<typename std::invoke_result_t<F(Args...)>> {
    using return_type = typename std::invoke_result_t<F(Args...)>;
      
    // тут лучше не придумали, нафига тут make_unique?
    auto task = std::make_unique<std::packaged_task<return_type()>>(
      [f = std::move(f), largs = std::make_tuple(std::forward<Args>(args)...)] () mutable {
        return std::apply(std::move(f), std::move(largs));
      }
    );

    std::future<return_type> res = task->get_future();
    task_t rtask = [local_task = std::move(task)]() { (*local_task)(); };
    submitbase(std::move(rtask));

    return res;
  }

  template<class F, class... Args>
  void distribute(const size_t count, F&& f, Args&&... args) noexcept {
    if (count == 0 || stop_source.stop_requested()) return;

    const size_t work_count = std::ceil(double(count) / double(size()));
    size_t start = 0;
    for (size_t i = 0; i < size(); ++i) {
      const size_t job_count = std::min(work_count, count-start);
      if (job_count == 0) break;
      
      task_t task = [f, arg = std::make_tuple(start, job_count, args...)]() {
        std::apply(std::move(f), std::move(arg));
      };
      submitbase(std::move(task));

      start += job_count;
    }
  }

  template<class F, class... Args>
  void distribute1(const size_t count, F&& f, Args&&... args) noexcept {
    if (count == 0 || stop_source.stop_requested()) return;

    const size_t work_count = std::ceil(double(count) / double(size()+1));
    size_t start = 0;
    for (size_t i = 0; i < size()+1; ++i) {
      const size_t job_count = std::min(work_count, count-start);
      if (job_count == 0) break;

      task_t task = [f, arg = std::make_tuple(start, job_count, args...)]() {
        std::apply(std::move(f), std::move(arg));
      };
      submitbase(std::move(task));

      start += job_count;
    }
  }

  void compute();
  void compute(const size_t count);
  void wait() noexcept; // просто ждет всех

  bool is_dependent(const std::thread::id id) const noexcept;
  uint32_t thread_index(const std::thread::id id) const noexcept;

  size_t size() const noexcept;
  size_t tasks_count() const noexcept;
  size_t working_count() const noexcept;
  size_t queue_capacity() const noexcept;
private:
  std::stop_source stop_source;
  std::vector<std::jthread> workers;
  atomic_queue queue;
  std::atomic<size_t> busy_count;
};

}
}

#endif