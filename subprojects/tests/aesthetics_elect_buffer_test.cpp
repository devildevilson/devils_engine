#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include <devils_engine/aesthetics/common.h>
#include <devils_engine/aesthetics/elect_buffer.h>
#include <devils_engine/thread/atomic_pool.h>

using devils_engine::aesthetics::elect_buffer;
using devils_engine::aesthetics::entityid_t;
using devils_engine::aesthetics::get_entityid_index;
using devils_engine::aesthetics::invalid_entityid;
using devils_engine::aesthetics::make_entityid;

namespace {
// Собрать (target_index, winner_index) всех победителей в порядке обхода — проверка
// детерминированного порядка и того, что for_each_winner отдаёт полные entityid.
std::vector<std::pair<size_t, size_t>> winners(const elect_buffer& eb) {
  std::vector<std::pair<size_t, size_t>> out;
  eb.for_each_winner([&](const entityid_t target, const entityid_t winner) {
    out.push_back({get_entityid_index(target), get_entityid_index(winner)});
  });
  return out;
}
} // namespace

TEST_CASE("elect_buffer starts empty and reports capacity") {
  elect_buffer eb;
  CHECK(eb.capacity() == 0);
  CHECK(eb.empty());
  CHECK(eb.size() == 0);

  eb.reset(8);
  CHECK(eb.capacity() == 8);
  CHECK(eb.empty());
  CHECK(eb.size() == 0);
  CHECK_FALSE(eb.has_winner(make_entityid(3, 0)));
  CHECK(eb.winner(make_entityid(3, 0)) == invalid_entityid);
}

TEST_CASE("elect_buffer picks the smallest claimant id per target") {
  elect_buffer eb;
  eb.reset(16);

  const auto prey = make_entityid(9, 2);
  // Претенденты приходят в перемешанном порядке; побеждает наименьший id (индекс в старших битах).
  eb.claim(prey, make_entityid(5, 0));
  eb.claim(prey, make_entityid(2, 1)); // наименьший индекс ⇒ победитель
  eb.claim(prey, make_entityid(8, 0));

  CHECK(eb.has_winner(prey));
  CHECK(eb.winner(prey) == make_entityid(2, 1));
  CHECK(eb.won(prey, make_entityid(2, 1)));
  CHECK_FALSE(eb.won(prey, make_entityid(5, 0)));
  CHECK(eb.size() == 1);
  CHECK_FALSE(eb.empty());

  // Претензия с БОЛЬШИМ id не сдвигает уже установленного минимума.
  eb.claim(prey, make_entityid(12, 0));
  CHECK(eb.winner(prey) == make_entityid(2, 1));
}

TEST_CASE("elect_buffer keeps targets independent and drains in ascending target order") {
  elect_buffer eb;
  eb.reset(16);

  eb.claim(make_entityid(14, 0), make_entityid(7, 0));
  eb.claim(make_entityid(3, 0), make_entityid(6, 0));
  eb.claim(make_entityid(3, 0), make_entityid(1, 0)); // победитель цели 3
  eb.claim(make_entityid(9, 0), make_entityid(4, 0));

  CHECK(eb.size() == 3);
  CHECK(eb.winner(make_entityid(3, 0)) == make_entityid(1, 0));
  CHECK(eb.winner(make_entityid(9, 0)) == make_entityid(4, 0));
  CHECK(eb.winner(make_entityid(14, 0)) == make_entityid(7, 0));
  CHECK_FALSE(eb.has_winner(make_entityid(5, 0)));

  // Обход по возрастанию индекса цели независимо от порядка claim.
  const auto out = winners(eb);
  const std::vector<std::pair<size_t, size_t>> expected{{3, 1}, {9, 4}, {14, 7}};
  CHECK(out == expected);
}

TEST_CASE("elect_buffer clear drops winners but keeps capacity") {
  elect_buffer eb;
  eb.reset(4);
  eb.claim(make_entityid(1, 0), make_entityid(0, 0));
  eb.claim(make_entityid(3, 0), make_entityid(2, 0));
  REQUIRE(eb.size() == 2);

  eb.clear();
  CHECK(eb.empty());
  CHECK(eb.size() == 0);
  CHECK(eb.capacity() == 4);
  CHECK(winners(eb).empty());
}

TEST_CASE("elect_buffer reset clears winners and can grow") {
  elect_buffer eb;
  eb.reset(4);
  eb.claim(make_entityid(2, 0), make_entityid(0, 0));
  REQUIRE(eb.size() == 1);

  eb.reset(4); // тот же размер — просто очистка
  CHECK(eb.empty());

  eb.reset(64); // рост ёмкости
  CHECK(eb.capacity() == 64);
  CHECK(eb.empty());
  eb.claim(make_entityid(50, 0), make_entityid(10, 0));
  CHECK(eb.winner(make_entityid(50, 0)) == make_entityid(10, 0));
}

TEST_CASE("elect_buffer elects deterministically under lock-free concurrent claims") {
  // n претендентов бьют по m целям: claimant i заявляет цель (i % m). Наименьший i с i%m==j — это
  // сам j (j < m), поэтому winner(target j) == make_entityid(j). Ожидание НЕ зависит от порядка потоков.
  constexpr size_t n = 4000;
  constexpr size_t m = 256;

  const auto run = [] {
    elect_buffer eb;
    eb.reset(m); // сайзинг ДО параллельной фазы — реаллокаций нет
    devils_engine::thread::atomic_pool pool(4);
    pool.distribute1(n, [&eb](const size_t start, const size_t count) {
      for (size_t i = start; i < start + count; ++i) {
        eb.claim(make_entityid(i % m, 0), make_entityid(i, 0));
      }
    });
    pool.compute();
    pool.wait();
    return winners(eb);
  };

  const auto a = run();
  const auto b = run();

  REQUIRE(a.size() == m); // каждая из m целей оспорена
  // Победитель цели j — наименьший претендент j; порядок обхода — по возрастанию индекса цели.
  bool ok = true;
  for (size_t j = 0; j < m; ++j) {
    ok = ok && a[j].first == j && a[j].second == j;
  }
  CHECK(ok);
  // Детерминизм: два независимых прогона дают побайтово тот же результат.
  CHECK(a == b);
}
