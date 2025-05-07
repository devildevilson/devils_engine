#include "stack_pool.h"

namespace devils_engine {
namespace thread {
stack_pool::stack_pool(const size_t stack_size, const size_t workers_count) :
  stack(stack_size, 16), busy_count(workers_count), stop(false)
{
  for (size_t i = 0; i < workers_count; ++i) {
    workers.emplace_back([this]() {
      while (true) {
        arbitrary_job* p = nullptr;

        {
          std::unique_lock lock(this->mutex);
          busy_count -= 1;

          if (queue.empty()) finish.notify_one();

          condition.wait(lock, [this] () {
            return stop || !queue.empty();
          });

          if (stop) return;

          p = queue.front();
          queue.pop();

          busy_count += 1;
        }

        p->execute();
        p->~arbitrary_job();
      }
    });
  }
}

stack_pool::~stack_pool() noexcept {
  {
    std::unique_lock l(mutex);
    stop = true;
    condition.notify_all();
  }

  for(auto &worker : workers) {
    if (worker.joinable()) worker.join();
  }
}

void stack_pool::compute() noexcept {
  while (true) {
    arbitrary_job* p = nullptr;

    {
      std::unique_lock<std::mutex> lock(this->mutex);
      if (this->queue.empty()) return;

      p = this->queue.front();
      this->queue.pop();
    }

    p->execute();
    p->~arbitrary_job();
  }
}

void stack_pool::compute(const size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    arbitrary_job* p = nullptr;

    {
      std::unique_lock<std::mutex> lock(mutex);
      if (queue.empty()) return;

      p = queue.front();
      queue.pop();
    }

    p->execute();
    p->~arbitrary_job();
  }
}

void stack_pool::wait() noexcept {
  if (is_dependent(std::this_thread::get_id())) return;

  std::unique_lock<std::mutex> lock(mutex);

  finish.wait(lock, [this] () {
    return queue.empty() && (busy_count == 0);
  });
}

void stack_pool::wait_and_reset() noexcept {
  wait();
  reset();
}

bool stack_pool::is_dependent(const std::thread::id id) const noexcept {
  for (size_t i = 0; i < workers.size(); ++i) {
    if (workers[i].get_id() == id) return true;
  }

  return false;
}

uint32_t stack_pool::thread_index(const std::thread::id id) const noexcept {
  for (uint32_t i = 0; i < workers.size(); ++i) {
    if (workers[i].get_id() == id) return i+1;
  }

  return 0;
}

void stack_pool::reset() {
  std::unique_lock l(mutex);
  stack.clear();
}

size_t stack_pool::size() const noexcept {
  std::unique_lock l(mutex);
  return workers.size();
}

size_t stack_pool::tasks_count() const noexcept {
  std::unique_lock l(mutex);
  return queue.size();
}

size_t stack_pool::busy_workers_count() const noexcept {
  std::unique_lock l(mutex);
  return busy_count;
}
}
}