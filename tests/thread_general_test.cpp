#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "devils_engine/thread/atomic.h"
#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/thread/lock.h"
#include "devils_engine/thread/queue1.h"
#include "devils_engine/thread/spsc_queue.h"
#include "devils_engine/thread/byte_ring.h"
#include "devils_engine/thread/payload_channel.h"
#include "devils_engine/thread/mailbox.h"

#include <cstring>
#include <string>

using namespace devils_engine;

namespace {

struct non_default_constructible {
  int value;

  explicit non_default_constructible(const int value_) noexcept : value(value_) {}
  non_default_constructible(const non_default_constructible&) noexcept = default;
  non_default_constructible(non_default_constructible&&) noexcept = default;
  non_default_constructible& operator=(const non_default_constructible&) noexcept = default;
  non_default_constructible& operator=(non_default_constructible&&) noexcept = default;
};

}

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

TEST_CASE("spsc_queue preserves FIFO order and reuses fixed storage [thread::spsc_queue]") {
  CHECK_THROWS_AS(thread::spsc_queue<int>(0), std::invalid_argument);

  thread::spsc_queue<int> queue(3);
  int out = -1;

  CHECK(queue.empty());
  CHECK(queue.capacity() == 3);
  CHECK(queue.size_approx() == 0);
  CHECK_FALSE(queue.try_pop(out));
  CHECK(out == -1);

  CHECK(queue.try_push(1));
  CHECK(queue.try_push(2));
  CHECK(queue.try_push(3));
  CHECK(queue.full());
  CHECK(queue.size_approx() == 3);
  CHECK_FALSE(queue.try_push(4));

  REQUIRE(queue.try_pop(out));
  CHECK(out == 1);
  CHECK(queue.try_push(4));

  for (const int expected : {2, 3, 4}) {
    REQUIRE(queue.try_pop(out));
    CHECK(out == expected);
  }

  CHECK(queue.empty());
  CHECK_FALSE(queue.try_pop(out));
}

TEST_CASE("spsc_queue supports non default constructible values [thread::spsc_queue]") {
  thread::spsc_queue<non_default_constructible> queue(2);
  non_default_constructible out(-1);

  CHECK(queue.emplace(42));
  REQUIRE(queue.try_pop(out));
  CHECK(out.value == 42);
}

TEST_CASE("spsc_queue bulk push and pop return processed element counts [thread::spsc_queue]") {
  thread::spsc_queue<int> queue(5);
  std::array<int, 4> first_batch = {1, 2, 3, 4};
  std::array<int, 4> second_batch = {5, 6, 7, 8};
  std::array<int, 3> first_out = {-1, -1, -1};
  std::array<int, 8> second_out = {-1, -1, -1, -1, -1, -1, -1, -1};

  CHECK(queue.try_push(std::span<const int>(first_batch)) == 4);
  CHECK(queue.try_pop(std::span<int>(first_out)) == 3);
  CHECK(first_out == std::array<int, 3>{1, 2, 3});

  CHECK(queue.try_push(std::span<const int>(second_batch)) == 4);
  CHECK(queue.full());
  CHECK(queue.try_pop(std::span<int>(second_out)) == 5);

  const std::array<int, 8> expected = {4, 5, 6, 7, 8, -1, -1, -1};
  CHECK(second_out == expected);
  CHECK(queue.empty());
}

TEST_CASE("spsc_queue transfers values between one producer and one consumer [thread::spsc_queue]") {
  constexpr int count = 4096;
  thread::spsc_queue<int> queue(64);
  std::atomic<bool> producer_done = false;
  std::array<int, 17> batch = {};
  std::vector<int> consumed;
  consumed.reserve(count);

  std::thread producer([&] {
    for (int i = 0; i < count;) {
      const int batch_count = std::min<int>(static_cast<int>(batch.size()), count - i);
      for (int j = 0; j < batch_count; ++j) {
        batch[static_cast<size_t>(j)] = i + j;
      }

      const size_t pushed = queue.try_push(std::span<const int>(batch.data(), static_cast<size_t>(batch_count)));
      if (pushed != 0) {
        i += static_cast<int>(pushed);
      } else {
        std::this_thread::yield();
      }
    }

    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    int value = -1;
    while (!producer_done.load(std::memory_order_acquire) || !queue.empty()) {
      if (queue.try_pop(value)) {
        consumed.push_back(value);
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  REQUIRE(consumed.size() == static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    CHECK(consumed[static_cast<size_t>(i)] == i);
  }
  CHECK(queue.empty());
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

// ── byte_ring (SPSC байт-арена под payload сообщений) ──────────────────────────

namespace {
struct payload_msg { int64_t pos; uint32_t size; uint32_t tag; };

// записать байты в выданную область
static void fill(std::span<std::byte> region, const uint8_t value) {
  std::memset(region.data(), value, region.size());
}
static bool all_equal(std::span<const std::byte> region, const uint8_t value) {
  for (auto b : region) if (std::to_integer<uint8_t>(b) != value) return false;
  return true;
}
}

TEST_CASE("byte_ring FIFO round-trip paired with spsc_queue [thread::byte_ring]") {
  thread::byte_ring arena(64);
  thread::spsc_queue<payload_msg> q(8);

  // продюсер: три payload'а разного размера
  const std::pair<uint32_t, uint8_t> items[] = { {10, 0xAA}, {20, 0xBB}, {8, 0xCC} };
  for (uint32_t i = 0; i < 3; ++i) {
    std::span<std::byte> region;
    const int64_t pos = arena.alloc(items[i].first, region);
    REQUIRE(pos >= 0);
    fill(region, items[i].second);
    REQUIRE(q.try_push(payload_msg{ pos, items[i].first, i }));
  }

  // консьюмер: читает в порядке отправки, проверяет данные, реклеймит
  payload_msg m{};
  for (uint32_t i = 0; i < 3; ++i) {
    REQUIRE(q.try_pop(m));
    CHECK(m.tag == i);
    CHECK(m.size == items[i].first);
    CHECK(all_equal(arena.at(m.pos, m.size), items[i].second));
    arena.release(m.pos + m.size);
  }
  CHECK(arena.used_approx() == 0);
}

TEST_CASE("byte_ring wraps with padding, positions stay monotonic [thread::byte_ring]") {
  thread::byte_ring arena(16);

  std::span<std::byte> r;
  const int64_t p0 = arena.alloc(6, r); REQUIRE(p0 == 0); fill(r, 1);
  const int64_t p1 = arena.alloc(6, r); REQUIRE(p1 == 6); fill(r, 2);

  // хвоста (idx 12, осталось 4) не хватает под 6 contiguous, пока не реклеймнем
  std::span<std::byte> r2;
  CHECK(arena.alloc(6, r2) == -1); // overflow: заняты 12 из 16, +паддинг не влезает

  // реклеймим первые два — освобождаем место
  arena.release(6);
  arena.release(12);

  // теперь alloc заворачивается: позиция монотонна (16 = 12 + паддинг 4), память с начала
  const int64_t p2 = arena.alloc(6, r2);
  REQUIRE(p2 == 16);                       // монотонно, НЕ 0
  CHECK(arena.at(p2, 6).data() == arena.at(0, 6).data()); // физически с начала буфера (idx 0)
  fill(r2, 3);
  CHECK(all_equal(arena.at(p2, 6), 3));
  arena.release(p2 + 6);
  CHECK(arena.used_approx() == 0);
}

TEST_CASE("byte_ring overflow returns -1, frees after release [thread::byte_ring]") {
  thread::byte_ring arena(8);

  std::span<std::byte> r;
  CHECK(arena.alloc(0, r) == -1);   // нулевой размер
  CHECK(arena.alloc(9, r) == -1);   // больше ёмкости — никогда не влезет
  CHECK(r.empty());

  REQUIRE(arena.alloc(5, r) >= 0);  // заняли 5/8
  CHECK(arena.alloc(4, r) == -1);   // не хватает (5 + паддинг/4)

  arena.release(5);                 // освободили
  CHECK(arena.alloc(4, r) >= 0);    // теперь влезает (с заворотом)
}

// ── payload_channel (spsc_queue<Msg> + byte_ring) ──────────────────────────────

namespace {
struct pc_msg { uint64_t tag; int64_t pos; uint32_t size; }; // pos/size — контракт payload_channel
}

TEST_CASE("payload_channel writes payload via fill and drains in FIFO order [thread::payload_channel]") {
  thread::payload_channel<pc_msg> ch(8, 256);

  const std::pair<uint64_t, uint8_t> items[] = { {100, 0x11}, {200, 0x22}, {300, 0x33} };
  for (const auto& [tag, val] : items) {
    const bool ok = ch.write(16, [&](std::span<std::byte> region, int64_t pos) {
      std::memset(region.data(), val, region.size());
      return pc_msg{ tag, pos, static_cast<uint32_t>(region.size()) };
    });
    REQUIRE(ok);
  }

  size_t i = 0;
  ch.drain([&](const pc_msg& m, std::span<const std::byte> payload) {
    CHECK(m.tag == items[i].first);
    CHECK(payload.size() == 16);
    CHECK(all_equal(payload, items[i].second));
    ++i;
  });
  CHECK(i == 3);
  CHECK(ch.arena_used_approx() == 0); // всё реклеймнуто после drain
}

TEST_CASE("payload_channel write returns false on arena overflow [thread::payload_channel]") {
  thread::payload_channel<pc_msg> ch(64, 32); // маленькая арена

  int written = 0;
  while (ch.write(16, [](std::span<std::byte> r, int64_t pos) {
    return pc_msg{ 0, pos, static_cast<uint32_t>(r.size()) }; })) {
    ++written;
  }
  CHECK(written == 2); // 32 / 16

  // после дренажа снова можно писать
  ch.drain([](const pc_msg&, std::span<const std::byte>) {});
  CHECK(ch.write(16, [](std::span<std::byte> r, int64_t pos) {
    return pc_msg{ 0, pos, static_cast<uint32_t>(r.size()) }; }));
}

// ── mailbox (latest-wins triple-buffer) ────────────────────────────────────────

TEST_CASE("mailbox delivers latest value and drops older, reuses slots [thread::mailbox]") {
  thread::mailbox<std::string> mb;

  CHECK(mb.consume() == nullptr); // ничего не опубликовано

  mb.write_slot() = "first";
  mb.publish();
  { const std::string* v = mb.consume(); REQUIRE(v != nullptr); CHECK(*v == "first"); }
  CHECK(mb.consume() == nullptr); // с прошлого consume нового нет

  // три публикации без чтения — консьюмер получит ТОЛЬКО последнюю (drop-oldest)
  mb.write_slot() = "a"; mb.publish();
  mb.write_slot() = "b"; mb.publish();
  mb.write_slot() = "c"; mb.publish();
  { const std::string* v = mb.consume(); REQUIRE(v != nullptr); CHECK(*v == "c"); }
  CHECK(mb.consume() == nullptr);
}

TEST_CASE("mailbox reuses slot capacity across frames [thread::mailbox]") {
  thread::mailbox<std::vector<int>> mb;

  // Прогреваем все три слота ёмкостью >=1000 (triple-buffer ⇒ продюсер циклит по 3 слотам).
  for (int i = 0; i < 3; ++i) {
    mb.write_slot().assign(1000, 7);
    mb.publish();
    REQUIRE(mb.consume() != nullptr);
  }

  // После прогрева слот-продюсер уже имеет ёмкость — заполнение НА МЕСТЕ не реаллоцирует.
  auto& slot = mb.write_slot();
  CHECK(slot.capacity() >= 1000); // ёмкость переиспользована (слот из прогретого цикла)
  slot.assign(500, 3);
  mb.publish();
  const auto* v = mb.consume();
  REQUIRE(v != nullptr);
  CHECK(v->size() == 500);
}
