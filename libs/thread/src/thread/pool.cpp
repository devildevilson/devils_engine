#include "pool.h"

namespace devils_engine {
namespace thread {
pool::pool(const size_t thread_count) noexcept : stop(false), busy_count(thread_count) {
  for (size_t i = 0; i < thread_count; ++i) {
    workers.emplace_back([this]() {
      while (true) {
        task_t task;

        {
          std::unique_lock<std::mutex> lock(this->mutex);
          busy_count -= 1;

          if (tasks.empty()) finish.notify_one();

          condition.wait(lock, [this] () {
            return stop || !tasks.empty();
          });

          if (stop) return;

          task = std::move(tasks.front());
          tasks.pop();

          busy_count += 1;
        }

        task();
      }
    });
  }
}

pool::~pool() noexcept {
  {
    std::unique_lock<std::mutex> lock(this->mutex);
    stop = true;
    condition.notify_all();
  }

  for(auto &worker : workers) {
    if (worker.joinable()) worker.join();
  }
}

void pool::submitbase(task_t f) noexcept {
  std::unique_lock<std::mutex> lock(mutex);
  if (stop) return;
  tasks.emplace(std::move(f));
  condition.notify_one();
}

void pool::compute() noexcept {
  while (true) {
    task_t task;

    {
      std::unique_lock<std::mutex> lock(this->mutex);
      if (this->tasks.empty()) return;

      task = std::move(this->tasks.front());
      this->tasks.pop();
    }

    task();
  }
}

void pool::compute(const size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    std::function<void()> task;

    {
      std::unique_lock<std::mutex> lock(mutex);
      if (tasks.empty()) return;

      task = std::move(tasks.front());
      tasks.pop();
    }

    task();
  }
}

void pool::wait() noexcept {
  if (is_dependent(std::this_thread::get_id())) return;

  std::unique_lock<std::mutex> lock(mutex);

  finish.wait(lock, [this] () {
    return tasks.empty() && (busy_count == 0);
  });
}

bool pool::is_dependent(const std::thread::id id) const noexcept {
  for (size_t i = 0; i < workers.size(); ++i) {
    if (workers[i].get_id() == id) return true;
  }

  return false;
}

uint32_t pool::thread_index(const std::thread::id id) const noexcept {
  for (uint32_t i = 0; i < workers.size(); ++i) {
    if (workers[i].get_id() == id) return i+1;
  }

  return 0;
}

size_t pool::size() const noexcept { return workers.size(); }
size_t pool::tasks_count() const noexcept {
  std::unique_lock<std::mutex> lock(mutex);
  return tasks.size();
}

size_t pool::working_count() const noexcept {
  std::unique_lock<std::mutex> lock(mutex);
  return busy_count;
}
}
}