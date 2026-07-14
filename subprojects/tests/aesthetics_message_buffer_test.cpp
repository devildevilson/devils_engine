#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include <devils_engine/aesthetics/common.h>
#include <devils_engine/aesthetics/message_buffer.h>
#include <devils_engine/thread/atomic_pool.h>

using devils_engine::aesthetics::entityid_t;
using devils_engine::aesthetics::get_entityid_index;
using devils_engine::aesthetics::make_entityid;
using devils_engine::aesthetics::message_buffer;

namespace {
struct msg {
  int32_t value = 0;
};

// Собрать (index, value) присутствующих слотов в порядке обхода — для проверки детерминированного
// порядка И того, что for_each отдаёт полный entityid (индекс из него совпадает со слотом).
std::vector<std::pair<size_t, int32_t>> drain(const message_buffer<msg>& mb) {
  std::vector<std::pair<size_t, int32_t>> out;
  mb.for_each([&](const entityid_t id, const msg& m) {
    out.push_back({get_entityid_index(id), m.value});
  });
  return out;
}
} // namespace

TEST_CASE("message_buffer starts empty and reports capacity") {
  message_buffer<msg> mb;
  CHECK(mb.capacity() == 0);
  CHECK(mb.empty());
  CHECK(mb.size() == 0);

  mb.reset(8);
  CHECK(mb.capacity() == 8);
  CHECK(mb.empty());
  CHECK(mb.size() == 0);
}

TEST_CASE("message_buffer stores sparsely and drains in ascending index order") {
  message_buffer<msg> mb;
  mb.reset(16);

  // Пишем в перемешанном порядке; версии ненулевые — проверяем, что слот = именно индекс.
  mb.store(make_entityid(9, 3), msg{90});
  mb.store(make_entityid(2, 7), msg{20});
  mb.store(make_entityid(14, 1), msg{140});
  mb.store(make_entityid(2, 7), msg{22}); // перезапись того же слота — last-write-wins

  CHECK(mb.size() == 3);
  CHECK_FALSE(mb.empty());
  CHECK(mb.has(make_entityid(9, 3)));
  CHECK_FALSE(mb.has(make_entityid(5, 0)));

  const auto* found = mb.find(make_entityid(2, 7));
  REQUIRE(found != nullptr);
  CHECK(found->value == 22);
  CHECK(mb.find(make_entityid(5, 0)) == nullptr);

  // Обход детерминирован по возрастанию индекса независимо от порядка записи.
  const auto out = drain(mb);
  const std::vector<std::pair<size_t, int32_t>> expected{{2, 22}, {9, 90}, {14, 140}};
  CHECK(out == expected);
}

TEST_CASE("message_buffer clear drops presence but keeps capacity") {
  message_buffer<msg> mb;
  mb.reset(4);
  mb.store(make_entityid(1, 0), msg{1});
  mb.store(make_entityid(3, 0), msg{3});
  REQUIRE(mb.size() == 2);

  mb.clear();
  CHECK(mb.empty());
  CHECK(mb.size() == 0);
  CHECK(mb.capacity() == 4);
  CHECK(drain(mb).empty());
}

TEST_CASE("message_buffer reset clears presence and can grow") {
  message_buffer<msg> mb;
  mb.reset(4);
  mb.store(make_entityid(2, 0), msg{2});
  REQUIRE(mb.size() == 1);

  mb.reset(4); // тот же размер — просто очистка
  CHECK(mb.empty());

  mb.reset(64); // рост ёмкости
  CHECK(mb.capacity() == 64);
  CHECK(mb.empty());
  mb.store(make_entityid(50, 0), msg{500});
  CHECK(mb.size() == 1);
  CHECK(mb.has(make_entityid(50, 0)));
}

TEST_CASE("message_buffer accepts lock-free disjoint writes from a thread pool") {
  constexpr size_t n = 2000;
  message_buffer<msg> mb;
  mb.reset(n); // сайзинг ДО параллельной фазы — реаллокаций нет

  devils_engine::thread::atomic_pool pool(4);
  // Каждый воркер пишет СВОЙ диапазон слотов (непересекающиеся индексы ⇒ без локов, без гонок).
  pool.distribute1(n, [&mb](const size_t start, const size_t count) {
    for (size_t i = start; i < start + count; ++i) {
      mb.store(make_entityid(i, uint32_t(i % 7)), msg{int32_t(i * 10)});
    }
  });
  pool.compute();
  pool.wait();

  REQUIRE(mb.size() == n);
  // Все n сообщений присутствуют, в порядке индекса, значение соответствует записанному.
  const auto out = drain(mb);
  REQUIRE(out.size() == n);
  bool ok = true;
  for (size_t i = 0; i < n; ++i) {
    ok = ok && out[i].first == i && out[i].second == int32_t(i * 10);
  }
  CHECK(ok);
}
