#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include <devils_engine/aesthetics/collect_buffer.h>
#include <devils_engine/thread/atomic_pool.h>

using devils_engine::aesthetics::collect_buffer;

namespace {
struct hit {
  uint32_t target = 0;
  uint32_t source = 0;
  int32_t dmg = 0;
};

// Полный порядок по (target, source) — уникальный ключ на сообщение ⇒ детерминированная сортировка.
constexpr auto by_target_source = [](const hit& a, const hit& b) {
  if (a.target != b.target) {
    return a.target < b.target;
  }
  return a.source < b.source;
};
constexpr auto same_target = [](const hit& a, const hit& b) {
  return a.target == b.target;
};
} // namespace

TEST_CASE("collect_buffer starts empty and reports capacity") {
  collect_buffer<hit> c;
  CHECK(c.capacity() == 0);
  CHECK(c.empty());
  CHECK(c.size() == 0);

  c.reset(8);
  CHECK(c.capacity() == 8);
  CHECK(c.empty());
}

TEST_CASE("collect_buffer collects flat and sorts deterministically by key") {
  collect_buffer<hit> c;
  c.reset(16);
  // Дописываем в перемешанном порядке.
  c.push(hit{5, 2, 10});
  c.push(hit{1, 9, 3});
  c.push(hit{5, 0, 7});
  c.push(hit{1, 4, 1});
  CHECK(c.size() == 4);
  CHECK_FALSE(c.empty());

  c.sort(by_target_source);
  const auto d = c.data();
  REQUIRE(d.size() == 4);
  // Порядок: (1,4),(1,9),(5,0),(5,2) — по (target, source).
  CHECK(d[0].target == 1);
  CHECK(d[0].source == 4);
  CHECK(d[1].target == 1);
  CHECK(d[1].source == 9);
  CHECK(d[2].target == 5);
  CHECK(d[2].source == 0);
  CHECK(d[3].target == 5);
  CHECK(d[3].source == 2);
}

TEST_CASE("collect_buffer groups sorted messages with a sliding window") {
  collect_buffer<hit> c;
  c.reset(16);
  c.push(hit{5, 2, 10});
  c.push(hit{1, 9, 3});
  c.push(hit{5, 0, 7});
  c.push(hit{1, 4, 1});
  c.push(hit{5, 8, 5});
  c.sort(by_target_source);

  std::vector<std::pair<uint32_t, int32_t>> group_sums; // (target, суммарный урон)
  c.for_each_group(same_target, [&](std::span<const hit> g) {
    int32_t sum = 0;
    for (const auto& h : g) {
      sum += h.dmg;
    }
    group_sums.push_back({g.front().target, sum});
  });
  const std::vector<std::pair<uint32_t, int32_t>> expected{{1, 4}, {5, 22}};
  CHECK(group_sums == expected); // цель 1: 3+1; цель 5: 10+7+5
}

TEST_CASE("collect_buffer overflow is a loud error (must not drop)") {
  collect_buffer<hit> c;
  c.reset(2);
  c.push(hit{1, 1, 1});
  c.push(hit{2, 2, 2});
  CHECK_THROWS(c.push(hit{3, 3, 3})); // сверх ёмкости — громкая ошибка, не тихий дроп
}

TEST_CASE("collect_buffer collects lock-free from a thread pool and sorts deterministically") {
  constexpr size_t n = 4000;
  constexpr uint32_t m = 128; // число целей

  const auto run = [](collect_buffer<hit>& c) {
    c.reset(n);
    devils_engine::thread::atomic_pool pool(4);
    pool.distribute1(n, [&c](const size_t start, const size_t count) {
      for (size_t i = start; i < start + count; ++i) {
        c.push(hit{uint32_t(i) % m, uint32_t(i), int32_t(i)}); // цель = source % m, ключ (target,source) уникален
      }
    });
    pool.compute();
    pool.wait();
    c.sort(by_target_source);
  };

  collect_buffer<hit> a;
  run(a);
  REQUIRE(a.size() == n); // НИЧЕГО не потеряно

  // Сумма урона по всем целям = сумма 0..n-1 (все сообщения дошли).
  int64_t total = 0;
  a.for_each_group(same_target, [&](std::span<const hit> g) {
    for (const auto& h : g) {
      total += h.dmg;
    }
  });
  CHECK(total == int64_t(n) * (n - 1) / 2);

  // Детерминизм: независимый прогон даёт побайтово тот же отсортированный буфер.
  collect_buffer<hit> b;
  run(b);
  REQUIRE(b.size() == n);
  const auto da = a.data();
  const auto db = b.data();
  bool identical = da.size() == db.size();
  for (size_t i = 0; identical && i < da.size(); ++i) {
    identical = da[i].target == db[i].target && da[i].source == db[i].source && da[i].dmg == db[i].dmg;
  }
  CHECK(identical);
}
