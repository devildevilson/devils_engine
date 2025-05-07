#include "atomic_pool.h"

namespace devils_engine {
namespace thread {

atomic_pool::atomic_pool(const size_t thread_count) : busy_count(0) {
  for (size_t i = 0; i < thread_count; ++i) {
    workers.emplace_back([this] (std::stop_token stoken) {
      while (!stoken.stop_requested()) {
        task_t task;

        while (!queue.try_pop(task) && !stoken.stop_requested()) {
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        }

        // вот тут может быть ситуация когда последний поток взял в работу задачу
        // но еще не пометил себя как работающий, насколько это для меня вообще пробелма?
        // вообще ничего не мешает использовать какой нибудь глобальный счетчик задач
        // который уменьшаться будет только с окончанием этого блока
        busy_count.fetch_add(1ull, std::memory_order::relaxed);
        if (task) task();
        busy_count.fetch_add(-1ll, std::memory_order::relaxed);
      }
    }, stop_source.get_token());
  }
}

atomic_pool::~atomic_pool() noexcept {
  stop_source.request_stop();
}

void atomic_pool::submitbase(task_t f) noexcept {
  if (stop_source.stop_requested()) return;
  queue.push(std::move(f));
}

void atomic_pool::compute() {
  while (!queue.was_empty()) {
    task_t task;

    while (!queue.try_pop(task) && !queue.was_empty()) {
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }

    if (task) task();
  }
}

void atomic_pool::compute(const size_t count) {
  for (size_t i = 0; i < count && !queue.was_empty(); ++i) {
    task_t task;

    while (!queue.try_pop(task) && !queue.was_empty()) {
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }

    if (task) task();
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
size_t atomic_pool::working_count() const noexcept { return busy_count.load(std::memory_order::relaxed); }
size_t atomic_pool::queue_capacity() const noexcept { return queue.capacity(); }

}
}