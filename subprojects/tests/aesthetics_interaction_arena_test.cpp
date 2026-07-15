#include <doctest/doctest.h>

#include <devils_engine/aesthetics/common.h>
#include <devils_engine/aesthetics/interaction_arena.h>
#include <devils_engine/thread/atomic_pool.h>

using devils_engine::aesthetics::entityid_t;
using devils_engine::aesthetics::interaction_arena;
using devils_engine::aesthetics::make_entityid;

namespace {
constexpr uint64_t eat = 0x1;   // произвольные id типов взаимодействий
constexpr uint64_t grab = 0x2;
} // namespace

TEST_CASE("interaction_arena elects the smallest claimant and gates by intent-beats-grab") {
  interaction_arena a;
  a.ensure(eat);
  a.reset(64);

  const auto prey = make_entityid(9, 0);
  a.claim(eat, prey, make_entityid(5, 0));
  a.claim(eat, prey, make_entityid(2, 0)); // наименьший id → победитель
  a.claim(eat, prey, make_entityid(8, 0));

  // Победитель — наименьший претендент; добыча prey сама не инициатор ⇒ won истинно для победителя.
  CHECK(a.won(eat, prey, make_entityid(2, 0)));
  CHECK_FALSE(a.won(eat, prey, make_entityid(5, 0)));
  CHECK(a.is_initiator(make_entityid(2, 0))); // claimant помечен инициатором
  CHECK_FALSE(a.is_initiator(prey));          // за prey не заявляли — не инициатор
}

TEST_CASE("interaction_arena keeps interaction types independent") {
  interaction_arena a;
  a.ensure(eat);
  a.ensure(grab);
  a.reset(64);

  const auto t = make_entityid(10, 0);
  a.claim(eat, t, make_entityid(3, 0));
  a.claim(grab, t, make_entityid(1, 0));

  CHECK(a.won(eat, t, make_entityid(3, 0)));  // в eat победил 3
  CHECK(a.won(grab, t, make_entityid(1, 0))); // в grab — 1; каналы независимы
  CHECK_FALSE(a.won(eat, t, make_entityid(1, 0)));
}

TEST_CASE("interaction_arena resolves cascade: initiator prey is protected") {
  // A хочет съесть B, B хочет съесть C. B — сам инициатор ⇒ A его НЕ получает («intent бьёт grab»),
  // а B съедает пассивного C. Каскад режется без приоритетов и резолв-прохода.
  interaction_arena a;
  a.ensure(eat);
  a.reset(64);

  const auto A = make_entityid(1, 0), B = make_entityid(2, 0), C = make_entityid(3, 0);
  a.claim(eat, B, A); // A → B
  a.claim(eat, C, B); // B → C

  CHECK_FALSE(a.won(eat, B, A)); // B сам инициатор → A пасует
  CHECK(a.won(eat, C, B));       // C пассивен → B съедает
}

TEST_CASE("interaction_arena resolves symmetry to a stalemate") {
  // A↔B едят друг друга: оба инициаторы ⇒ оба пасуют (детерминированный стейт-мейт).
  interaction_arena a;
  a.ensure(eat);
  a.reset(64);

  const auto A = make_entityid(1, 0), B = make_entityid(2, 0);
  a.claim(eat, B, A);
  a.claim(eat, A, B);

  CHECK_FALSE(a.won(eat, B, A));
  CHECK_FALSE(a.won(eat, A, B));
}

TEST_CASE("interaction_arena elects deterministically under concurrent claims") {
  // n претендентов [0..n) бьют по m целям [base..base+m); target-пространство отделено от claimant'ов,
  // чтобы цели не были инициаторами. Победитель цели base+j — наименьший претендент j. Не зависит от
  // порядка потоков; два прогона дают тот же результат.
  constexpr size_t n = 3000;
  constexpr size_t m = 200;
  constexpr size_t base = 4096; // индексы целей выше индексов инициаторов

  const auto run = [](interaction_arena& a) {
    a.reset(base + m + 16);
    devils_engine::thread::atomic_pool pool(4);
    pool.distribute1(n, [&a](const size_t start, const size_t count) {
      for (size_t i = start; i < start + count; ++i) {
        a.claim(eat, make_entityid(base + (i % m), 0), make_entityid(i, 0));
      }
    });
    pool.compute();
    pool.wait();
  };

  interaction_arena a;
  a.ensure(eat);
  run(a);
  bool ok = true;
  for (size_t j = 0; j < m; ++j) {
    ok = ok && a.won(eat, make_entityid(base + j, 0), make_entityid(j, 0)); // победитель = наименьший j
  }
  CHECK(ok);

  interaction_arena b;
  b.ensure(eat);
  run(b);
  bool same = true;
  for (size_t j = 0; j < m; ++j) {
    same = same && (b.won(eat, make_entityid(base + j, 0), make_entityid(j, 0)));
  }
  CHECK(same); // тот же победитель в независимом прогоне (детерминизм)
}
