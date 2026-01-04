#ifndef DEVILS_ENGINE_THREAD_STACK_POOL_H
#define DEVILS_ENGINE_THREAD_STACK_POOL_H

#include <cstdint>
#include <cstddef>
#include <queue>
#include <mutex>
#include <future>
#include <type_traits>
#include "devils_engine/utils/stack_allocator.h"

namespace devils_engine {
namespace thread {

// решаем проблемный кейс связанный с std function
// гарантируем что не будем вызывать выделение памяти 
// (аргументы мы все равно можем захотеть передать как значение)
// сложность только одна: правильно выбрать размер стака
// неидеальное решение ... какое решение лучше?
// нам тут ничего не стоит использовать memory_pool
// легко возвратим память обратно
class stack_pool {
public:
  class arbitrary_job {
  public:
    virtual ~arbitrary_job() noexcept = default;
    virtual void execute() const noexcept = 0;
  };

  stack_pool(const size_t stack_size, const size_t workers_count);
  ~stack_pool() noexcept;

  template <typename T, typename... Args>
    requires(std::derived_from<T, arbitrary_job>)
  void submit(Args&&... args) {
    std::unique_lock l(mutex);
    if (stop) return;
    auto ptr = stack.allocate(sizeof(T));
    if (ptr == nullptr) utils::error{}("Could not allocate {} bytes from stack. Allocated already {}. Forgot to reset?", sizeof(T), stack.size());
    auto final_ptr = new (ptr) T(std::forward<Args>(args)...);
    queue.push(final_ptr);
  }

  template <typename F>
  void submit(F f) {
    struct func_job : public arbitrary_job {
      F f;
      func_job(F f) noexcept : f(std::move(f)) {}
      void execute() const override {
        std::invoke(std::move(f));
      }
    };

    submit<func_job>(std::move(f));
  }

  template <typename F, typename... Args>
  void submit(F f, Args&&... args) {
    struct func_job : public arbitrary_job {
      F f;
      std::tuple<Args...> args;
      func_job(F f, Args&&... args) noexcept : f(std::move(f)), args(std::forward<Args>(args)...) {}
      void execute() const override {
        std::apply(std::move(f), std::move(args));
      }
    };

    submit<func_job>(std::move(f), std::forward<Args>(args)...);
  }

  template <typename F>
  auto submit_r(F f) -> std::future<typename std::invoke_result_t<F>> {
    struct func_job : public arbitrary_job {
      F f;
      std::promise<typename std::invoke_result_t<F>> promise;
      func_job(F f) noexcept : f(std::move(f)) {}
      func_job(func_job&& move) noexcept = default;
      func_job & operator=(func_job&& move) noexcept = default;
      func_job(const func_job &copy) noexcept = delete;
      func_job & operator=(const func_job& copy) noexcept = delete;
      void execute() const override {
        if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
          std::invoke(std::move(f));
          promise.set_value();
        } else {
          promise = std::invoke(std::move(f));
        }
      }
    };

    func_job j(std::move(f));
    auto future = j.promise.get_future();
    submit<func_job>(std::move(j));
    return future;
  }

  template <typename F, typename... Args>
  auto submit_r(F f, Args&&... args) -> std::future<typename std::invoke_result_t<F, Args&&...>> {
    struct func_job : public arbitrary_job {
      F f;
      std::tuple<Args...> args;
      std::promise<typename std::invoke_result_t<F, Args&&...>> promise;
      func_job(F f, Args&&... args) noexcept : f(std::move(f)), args(std::forward<Args>(args)...) {}
      func_job(func_job&& move) noexcept = default;
      func_job & operator=(func_job&& move) noexcept = default;
      func_job(const func_job &copy) noexcept = delete;
      func_job & operator=(const func_job& copy) noexcept = delete;
      void execute() const override {
        if constexpr (std::is_void_v<typename std::invoke_result_t<F, Args&&...>>) {
          std::apply(std::move(f), std::move(args));
          promise.set_value();
        } else {
          promise = std::apply(std::move(f), std::move(args));
        }
      }
    };

    func_job j(std::move(f), std::forward<Args>(args)...);
    auto future = j.promise.get_future();
    submit<func_job>(std::move(j));
    return future;
  }

  void compute() noexcept;
  void compute(const size_t count) noexcept;
  void wait() noexcept; // просто ждет всех
  void wait_and_reset() noexcept;

  bool is_dependent(const std::thread::id id) const noexcept;
  uint32_t thread_index(const std::thread::id id) const noexcept;

  // нужно вызвать уже после выполнения всех задач
  void reset();
  size_t size() const noexcept;
  size_t tasks_count() const noexcept;
  size_t busy_workers_count() const noexcept;
private:
  mutable std::mutex mutex;
  utils::stack_allocator stack; // все равно желательно ринг буфер 
  std::queue<arbitrary_job*> queue;
  std::vector<std::thread> workers;
  std::condition_variable condition;
  std::condition_variable finish;
  size_t busy_count;
  bool stop;
};
}
}

#endif