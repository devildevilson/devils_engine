#include "atomic_pool.h"

constexpr auto ATOMIC_ONLY = std::memory_order_relaxed;
constexpr auto PUBLISH = std::memory_order_release;
constexpr auto CONSUME = std::memory_order_acquire;
constexpr auto PUB_CONSUME = std::memory_order_acq_rel;

// MSVC alignof(std::max_align_t) == alignof(double), WTF dude?
constexpr size_t standart_aligment = 16;

namespace devils_engine {
namespace thread {

atomic_pool::atomic_pool(const size_t thread_count) : stack_pool(MAXIMUM_TASK_SIZE * MAXIMUM_TASK_COUNT, MAXIMUM_TASK_SIZE, standart_aligment), busy_count(0) {
  for (size_t i = 0; i < thread_count; ++i) {
    workers.emplace_back([this] (std::stop_token stoken) {
      size_t spins = 0;

      while (!stoken.stop_requested()) {
        task_interface* task = nullptr;

        if (queue.try_pop(task)) {
          assert(task != nullptr);
          spins = 0;
          busy_count.fetch_add(1ull, PUB_CONSUME);
          task->process();
          stack_pool.destroy(task); // вернем память
          busy_count.fetch_sub(1ull, PUB_CONSUME);
          continue;
        }

        if (spins < WORKER_SPIN_COUNT) {
          spins += 1;
          std::this_thread::yield();
          continue;
        }

        {
          std::unique_lock lk(cv_mtx);
          cv.wait(lk, [&] { return queue.try_pop(task) || !stoken.stop_requested(); });
        }

        spins = 0;

        if (task != nullptr) {
          busy_count.fetch_add(1ull, PUB_CONSUME);
          task->process();
          stack_pool.destroy(task); // вернем память
          busy_count.fetch_sub(1ull, PUB_CONSUME);
          continue;
        }
      }
    }, stop_source.get_token());
  }
}

atomic_pool::~atomic_pool() noexcept {
  stop_source.request_stop();
  cv.notify_all();
}

//void atomic_pool::submitbase(task_t f) noexcept {
//  if (stop_source.stop_requested()) return;
//  queue.push(std::move(f));
//}

void atomic_pool::submitbase(task_interface* t) noexcept {
  if (stop_source.stop_requested()) return;
  queue.push(t);
  cv.notify_one();
}

void atomic_pool::compute() {
  size_t spins = 0;

  while (!queue.was_empty()) {
    task_interface* task = nullptr;

    if (queue.try_pop(task)) {
      assert(task != nullptr);
      spins = 0;
      task->process();
      stack_pool.destroy(task);
      continue;
    }

    if (spins < WORKER_SPIN_COUNT) {
      spins += 1;
      std::this_thread::yield();
      continue;
    }
  }
}

void atomic_pool::compute(const size_t count) {
  size_t spins = 0;

  for (size_t i = 0; i < count && !queue.was_empty(); ++i) {
    task_interface* task = nullptr;

    if (queue.try_pop(task)) {
      assert(task != nullptr);
      spins = 0;
      task->process();
      stack_pool.destroy(task);
      continue;
    }

    if (spins < WORKER_SPIN_COUNT) {
      spins += 1;
      std::this_thread::yield();
      continue;
    }
  }
}

void atomic_pool::wait() noexcept {
  while (!queue.was_empty() || working_count() != 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}

bool atomic_pool::is_dependent(const std::thread::id id) const noexcept {
  return thread_index(id) != 0;
}

uint32_t atomic_pool::thread_index(const std::thread::id id) const noexcept {
  uint32_t i = 0;
  for (; i < workers.size(); ++i) {
    if (id == workers[i].get_id()) return i + 1;
  }

  return 0;
}

size_t atomic_pool::size() const noexcept { return workers.size(); }
size_t atomic_pool::tasks_count() const noexcept { return queue.was_size(); }
size_t atomic_pool::working_count() const noexcept { return busy_count.load(CONSUME); }
size_t atomic_pool::queue_capacity() const noexcept { return queue.capacity(); }

}
}