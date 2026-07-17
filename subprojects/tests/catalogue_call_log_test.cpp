#include <cstdint>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include <devils_engine/catalogue/call_log.h>
#include <devils_engine/thread/atomic_pool.h>

using devils_engine::catalogue::call_log;
using devils_engine::catalogue::call_record;

namespace {
// Собрать (primary, fn) всех вызовов в порядке dispatch (= возрастание индекса слота).
std::vector<std::pair<uint32_t, uint64_t>> drain(const call_log& l) {
  std::vector<std::pair<uint32_t, uint64_t>> out;
  l.dispatch([&](const call_record& r) { out.push_back({r.primary, r.fn}); });
  return out;
}
} // namespace

TEST_CASE("call_log starts empty and reports capacity") {
  call_log l;
  CHECK(l.capacity() == 0);
  CHECK(l.empty());
  CHECK(l.size() == 0);

  l.reset(8);
  CHECK(l.capacity() == 8);
  CHECK(l.empty());
  CHECK_FALSE(l.has(3));
}

TEST_CASE("call_log records sparsely and dispatches in ascending slot order") {
  call_log l;
  l.reset(16);

  l.record(9, call_record{90u, 9u, 0xffffffffu});
  l.record(2, call_record{20u, 2u, 0xffffffffu});
  l.record(14, call_record{140u, 14u, 5u});
  // last-write-wins по слоту.
  l.record(2, call_record{22u, 2u, 7u});

  CHECK(l.size() == 3);
  CHECK_FALSE(l.empty());
  CHECK(l.has(9));
  CHECK_FALSE(l.has(5));

  const auto out = drain(l);
  const std::vector<std::pair<uint32_t, uint64_t>> expected{{2, 22u}, {9, 90u}, {14, 140u}};
  CHECK(out == expected); // порядок по индексу слота, значение — последняя запись слота 2
}

TEST_CASE("call_log clear drops records but keeps capacity") {
  call_log l;
  l.reset(4);
  l.record(1, call_record{1u, 1u, 0u});
  l.record(3, call_record{3u, 3u, 0u});
  REQUIRE(l.size() == 2);

  l.clear();
  CHECK(l.empty());
  CHECK(l.capacity() == 4);
  CHECK(drain(l).empty());
}

TEST_CASE("call_log reset clears records and can grow") {
  call_log l;
  l.reset(4);
  l.record(2, call_record{2u, 2u, 0u});
  REQUIRE(l.size() == 1);

  l.reset(4); // тот же размер — просто очистка
  CHECK(l.empty());

  l.reset(64); // рост
  CHECK(l.capacity() == 64);
  l.record(50, call_record{500u, 50u, 0u});
  CHECK(l.has(50));
}

TEST_CASE("call_log accepts lock-free disjoint records from a thread pool") {
  constexpr size_t n = 2000;
  call_log l;
  l.reset(n); // сайзинг ДО параллельной фазы

  devils_engine::thread::atomic_pool pool(4);
  pool.distribute1(n, [&l](const size_t start, const size_t count) {
    for (size_t i = start; i < start + count; ++i) {
      l.record(i, call_record{uint64_t(i * 10), uint32_t(i), 0xffffffffu});
    }
  });
  pool.compute();
  pool.wait();

  REQUIRE(l.size() == n);
  const auto out = drain(l);
  REQUIRE(out.size() == n);
  bool ok = true;
  for (size_t i = 0; i < n; ++i) {
    ok = ok && out[i].first == uint32_t(i) && out[i].second == uint64_t(i * 10);
  }
  CHECK(ok); // все записаны, порядок dispatch = возрастание индекса
}
