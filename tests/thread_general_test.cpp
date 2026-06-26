#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "devils_engine/thread/atomic.h"
#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/thread/lock.h"
#include "devils_engine/thread/queue1.h"

using namespace devils_engine;

TEST_CASE("atomic_min and atomic_max update only in the requested direction [thread::atomic]") {
  std::atomic<int> value = 10;

  thread::atomic_max(value, 9);
  CHECK(value.load() == 10);
  thread::atomic_max(value, 12);
  CHECK(value.load() == 12);

  thread::atomic_min(value, 13);
  CHECK(value.load() == 12);
  thread::atomic_min(value, 3);
  CHECK(value.load() == 3);
}

TEST_CASE("queue1 preserves FIFO order and reports full or empty states [thread::queue1]") {
  thread::queue1<int, 4> queue;
  int out = -1;

  CHECK_FALSE(queue.dequeue(out));
  CHECK(out == -1);

  CHECK(queue.enqueue(1));
  CHECK(queue.enqueue(2));
  CHECK(queue.enqueue(3));
  CHECK(queue.enqueue(4));
  CHECK_FALSE(queue.enqueue(5));

  for (int expected = 1; expected <= 4; ++expected) {
    REQUIRE(queue.dequeue(out));
    CHECK(out == expected);
  }

  CHECK_FALSE(queue.dequeue(out));

  CHECK(queue.enqueue(6));
  REQUIRE(queue.dequeue(out));
  CHECK(out == 6);
}

TEST_CASE("spin_mutex serializes concurrent updates [thread::spin_mutex]") {
  thread::spin_mutex mutex;
  int counter = 0;
  std::vector<std::thread> workers;

  for (size_t i = 0; i < 4; ++i) {
    workers.emplace_back([&] {
      for (size_t j = 0; j < 1000; ++j) {
        mutex.lock();
        counter += 1;
        mutex.unlock();
      }
    });
  }

  for (auto& worker : workers) worker.join();
  CHECK(counter == 4000);
}

TEST_CASE("light_spin_mutex supports try_lock and unlock [thread::light_spin_mutex]") {
  thread::light_spin_mutex mutex;

  CHECK(mutex.try_lock());
  CHECK_FALSE(mutex.try_lock());
  mutex.unlock();
  CHECK(mutex.try_lock());
  mutex.unlock();
}

TEST_CASE("atomic_pool executes submitted tasks and waits for completion [thread::atomic_pool]") {
  thread::atomic_pool pool(2);
  std::atomic<int> counter = 0;

  for (int i = 0; i < 64; ++i) {
    pool.submit([&counter] {
      counter.fetch_add(1, std::memory_order_relaxed);
    });
  }

  pool.wait();
  CHECK(counter.load(std::memory_order_relaxed) == 64);
  CHECK(pool.working_count() == 0);
}

TEST_CASE("atomic_pool can distribute contiguous work ranges [thread::atomic_pool]") {
  thread::atomic_pool pool(3);
  std::vector<std::atomic<int>> visits(25);

  pool.distribute(visits.size(), [&visits] (const size_t start, const size_t count) {
    for (size_t i = start; i < start + count; ++i) {
      visits[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  pool.wait();

  for (const auto& visit : visits) {
    CHECK(visit.load(std::memory_order_relaxed) == 1);
  }
}

TEST_CASE("atomic_pool supports explicit main-thread compute with zero workers [thread::atomic_pool]") {
  thread::atomic_pool pool(0);
  std::atomic<int> counter = 0;

  for (int i = 0; i < 8; ++i) {
    pool.submit([&counter] {
      counter.fetch_add(1, std::memory_order_relaxed);
    });
  }

  CHECK(pool.tasks_count() == 8);
  pool.compute();
  CHECK(counter.load(std::memory_order_relaxed) == 8);
  CHECK(pool.tasks_count() == 0);
}
