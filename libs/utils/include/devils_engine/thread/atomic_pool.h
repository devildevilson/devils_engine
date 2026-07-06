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
#include <cmath>
#include <tuple>
#include <type_traits>

#include "atomic_queue/atomic_queue.h"
#include "devils_engine/utils/stack_allocator.h"

// по идее это должна быть версия куда быстрее чем предыдущая
// + потоки тут почти всегда в работе что с одной стороны хуже
// с другой стороны лучше если в течении работы приложения они всегда будут чем то заняты

#ifndef DEVILS_THREAD_ATOMIC_QUEUE_CAPACITY
  #define DEVILS_THREAD_ATOMIC_QUEUE_CAPACITY 256
#endif

namespace devils_engine {
namespace thread {

constexpr size_t MAXIMUM_TASK_SIZE = 128;
constexpr size_t MAXIMUM_TASK_COUNT = DEVILS_THREAD_ATOMIC_QUEUE_CAPACITY;
// Сколько раз воркер делает yield() при пустой очереди перед сном на cv. Спин перекрывает
// короткие паузы ВНУТРИ батча (следующая задача вот-вот придёт) без дорогого cv-wakeup. После
// батча — быстро засыпать (незачем жечь ядро: следующий dispatch на порядки позже). Баланс
// латентность/энергопотребление; тюнить под самый частый паттерн dispatch.
constexpr size_t WORKER_SPIN_COUNT = 256;

class atomic_pool {
public:
  class task_interface {
  public:
    virtual ~task_interface() noexcept = default;
    virtual void process() const = 0;
  };

  template <typename F, typename Args_t>
  class task_t : public task_interface {
  public:
    F fn;
    Args_t args;

    template <typename Fn, typename... Args>
    task_t(Fn&& fn_, Args&&... args_) noexcept : fn(std::forward<Fn>(fn_)), args(std::forward<Args>(args_)...) {}
    void process() const override {
      std::apply(fn, args);
    }
  };

  using atomic_queue = atomic_queue::AtomicQueue<task_interface*, DEVILS_THREAD_ATOMIC_QUEUE_CAPACITY>;

  atomic_pool(const size_t thread_count);
  ~atomic_pool() noexcept;

  atomic_pool(const atomic_pool& copy) noexcept = delete;
  atomic_pool(atomic_pool&& move) noexcept = default;
  atomic_pool& operator=(const atomic_pool& copy) noexcept = delete;
  atomic_pool& operator=(atomic_pool&& move) noexcept = default;

  void submitbase(task_interface* t) noexcept;

  template<class F, class... Args>
  void submit(F&& f, Args&&... args) noexcept {
    using local_task_t = task_t<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>;
    static_assert(sizeof(local_task_t) <= MAXIMUM_TASK_SIZE);
    auto ptr = stack_pool.create<local_task_t>(std::forward<F>(f), std::forward<Args>(args)...);
    submitbase(ptr);
  }

  template<class F, class... Args>
  void distribute(const size_t count, F&& f, Args&&... args) noexcept {
    if (count == 0 || stop_source.stop_requested()) return;

    const size_t work_count = std::ceil(double(count) / double(size()));
    size_t start = 0;
    for (size_t i = 0; i < size(); ++i) {
      const size_t job_count = std::min(work_count, count-start);
      if (job_count == 0) break;

      submit(std::forward<F>(f), start, job_count, std::forward<Args>(args)...);

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

      submit(std::forward<F>(f), start, job_count, std::forward<Args>(args)...);

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
  utils::fixed_pool_mt stack_pool;
  std::atomic<size_t> busy_count; // задачи В ПРОЦЕССЕ исполнения (working_count)
  // задачи, СУБМИТНУТЫЕ но не завершённые (в очереди ИЛИ в работе). Инкремент в submitbase ДО
  // queue.push, декремент после process — закрывает TOCTOU-окно между try_pop и busy_count++,
  // из-за которого wait() мог вернуться раньше, чем воркер начнёт задачу. wait() ждёт именно его.
  std::atomic<size_t> pending;
  std::condition_variable cv;
  std::mutex cv_mtx;
};

}
}

#endif
