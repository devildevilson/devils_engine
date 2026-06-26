#include <doctest/doctest.h>

#include <atomic>
#include <vector>

#include "devils_engine/thread/atomic.h"
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
