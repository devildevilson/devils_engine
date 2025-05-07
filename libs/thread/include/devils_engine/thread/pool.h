#ifndef DEVILS_ENGINE_THREAD_POOL_H
#define DEVILS_ENGINE_THREAD_POOL_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <future>
#include <type_traits>
#include <memory>
#include <queue>
#include <thread>
#include <cmath>

namespace devils_engine {
namespace thread {
class pool {
public:
  using task_t = std::function<void()>;

  pool(const size_t thread_count) noexcept;
  ~pool() noexcept;

  void submitbase(task_t f) noexcept;

  template<class F, class... Args>
  void submit(F&& f, Args&&... args) noexcept {
    // может ли сыграть то что неправильно муваю аргументы?
    task_t task = [f = std::move(f), largs = std::make_tuple(std::forward<Args>(args)...)] () mutable {
      return std::apply(std::move(f), std::move(largs));
    };

    submitbase(std::move(task));
  }

  template<class F, class... Args>
  auto submit_future(F&& f, Args&&... args) noexcept -> std::future<typename std::invoke_result_t<F(Args...)>> {
    using return_type = typename std::invoke_result_t<F(Args...)>;
      
    // тут лучше не придумали
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
    if (count == 0 || stop) return;

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
    if (count == 0 || stop) return;

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

  void compute() noexcept;
  void compute(const size_t count) noexcept;
  void wait() noexcept; // просто ждет всех

  bool is_dependent(const std::thread::id id) const noexcept;
  uint32_t thread_index(const std::thread::id id) const noexcept;

  size_t size() const noexcept;
  size_t tasks_count() const noexcept;
  size_t working_count() const noexcept;
private:
  bool stop;

  std::vector<std::thread> workers;
  std::queue<std::function<void()>> tasks;

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::condition_variable finish;
  size_t busy_count;
};
}
}

#endif