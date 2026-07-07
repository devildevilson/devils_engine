#include <doctest/doctest.h>

#include <memory>
#include <vector>
#include <array>

#include "devils_engine/acumen/system.h"
#include "devils_engine/act/registry.h"
#include "devils_engine/act/function.h"

using namespace devils_engine;

namespace {
// предикаты с фиксированным результатом (ctx игнорят) — детерминированный тест compute_state.
bool pred_false(const act::exec_context&) { return false; }
bool pred_true(const act::exec_context&)  { return true; }
void effect_noop(const act::exec_context&) {}
}

TEST_CASE("Acumen GOAP under act registry [acumen::system]") {
  // общий реестр act: метрики = предикаты, эффекты действий = effect-функции.
  act::registry reg;
  reg.reg("has_weapon", std::make_unique<act::native_function<bool>>(&pred_false));
  reg.reg("enemy_dead", std::make_unique<act::native_function<bool>>(&pred_false));
  reg.reg("enemy_near", std::make_unique<act::native_function<bool>>(&pred_true));
  reg.reg("draw_weapon", std::make_unique<act::native_function<void>>(&effect_noop));
  reg.reg("attack",      std::make_unique<act::native_function<void>>(&effect_noop));

  // флаги-биты = индекс метрики в массиве (плотная нумерация).
  enum flag : size_t { has_weapon = 0, enemy_dead = 1, enemy_near = 2 };

  std::vector<acumen::state_metric> metrics = {
    acumen::state_metric("has_weapon"),
    acumen::state_metric("enemy_dead"),
    acumen::state_metric("enemy_near"),
  };

  // draw_weapon: без требований, ставит has_weapon.
  acumen::scoped_state draw_next; draw_next.set(has_weapon, true);
  // attack: требует has_weapon, ставит enemy_dead.
  acumen::scoped_state atk_req;  atk_req.set(has_weapon, true);
  acumen::scoped_state atk_next; atk_next.set(enemy_dead, true);

  std::vector<acumen::action> actions = {
    acumen::action("draw_weapon", acumen::scoped_state{}, draw_next, acumen::scoped_state{}),
    acumen::action("attack",      atk_req,                atk_next,  acumen::scoped_state{}),
  };

  acumen::scoped_state goal_state; goal_state.set(enemy_dead, true);
  std::vector<acumen::goal> goals = { acumen::goal{ "kill", acumen::scoped_state{}, goal_state } };

  acumen::system sys(&reg, metrics, goals, actions);

  SUBCASE("compute_state runs registry predicates into bits") {
    const act::exec_context ctx{};
    const auto s = sys.compute_state(ctx);
    REQUIRE(s.test(has_weapon) == false);
    REQUIRE(s.test(enemy_dead) == false);
    REQUIRE(s.test(enemy_near) == true);
  }

  // decide без кеша (params.cache == nullptr) — живой A* напрямую.
  SUBCASE("decide without cache reaches the goal and uses the prerequisite action") {
    const act::exec_context ctx{};
    const auto start = sys.compute_state(ctx); // has_weapon/enemy_dead false, enemy_near true

    astar<acumen::astar_data>::container c;
    std::array<const acumen::action*, 8> buf{};
    acumen::decide_params dp;
    dp.start = start; dp.goal = goal_state; dp.scratch = &c; // cache == nullptr
    const size_t n = sys.decide(dp, buf);

    // точный план: сначала достать оружие, потом ударить (без ведущего nullptr, с финальным attack).
    REQUIRE(n == 2);
    REQUIRE(buf[0] != nullptr);
    REQUIRE(buf[0]->name == "draw_weapon");
    REQUIRE(buf[1] != nullptr);
    REQUIRE(buf[1]->name == "attack");
  }

  SUBCASE("start already satisfies the goal => empty plan") {
    acumen::state already; already.set(enemy_dead, true);
    astar<acumen::astar_data>::container c;
    std::array<const acumen::action*, 8> buf{};
    acumen::decide_params dp;
    dp.start = already; dp.goal = goal_state; dp.scratch = &c;
    const size_t n = sys.decide(dp, buf);
    REQUIRE(n == 0); // действий не нужно
  }

  SUBCASE("undersized buffer: full length returned, prefix written, no overflow") {
    const act::exec_context ctx{};
    const auto start = sys.compute_state(ctx);
    astar<acumen::astar_data>::container c;
    std::array<const acumen::action*, 1> small{};
    acumen::decide_params dp;
    dp.start = start; dp.goal = goal_state; dp.scratch = &c;
    const size_t n = sys.decide(dp, small);
    REQUIRE(n == 2); // полная длина плана, хотя буфер на 1
    REQUIRE(small[0] != nullptr);
    REQUIRE(small[0]->name == "draw_weapon"); // записан только префикс
  }

  SUBCASE("missing predicate in registry is a load-time error") {
    act::registry empty;
    std::vector<acumen::state_metric> bad = { acumen::state_metric("no_such_predicate") };
    REQUIRE_THROWS(acumen::system(&empty, bad, {}, {}));
  }
}

TEST_CASE("Acumen solution memoization [acumen::solution_cache]") {
  // тот же крошечный GOAP: draw_weapon -> attack, цель enemy_dead.
  act::registry reg;
  reg.reg("has_weapon", std::make_unique<act::native_function<bool>>(&pred_false));
  reg.reg("enemy_dead", std::make_unique<act::native_function<bool>>(&pred_false));
  reg.reg("enemy_near", std::make_unique<act::native_function<bool>>(&pred_true));
  reg.reg("draw_weapon", std::make_unique<act::native_function<void>>(&effect_noop));
  reg.reg("attack",      std::make_unique<act::native_function<void>>(&effect_noop));

  enum flag : size_t { has_weapon = 0, enemy_dead = 1, enemy_near = 2 };

  std::vector<acumen::state_metric> metrics = {
    acumen::state_metric("has_weapon"),
    acumen::state_metric("enemy_dead"),
    acumen::state_metric("enemy_near"),
  };
  acumen::scoped_state draw_next; draw_next.set(has_weapon, true);
  acumen::scoped_state atk_req;   atk_req.set(has_weapon, true);
  acumen::scoped_state atk_next;  atk_next.set(enemy_dead, true);
  std::vector<acumen::action> actions = {
    acumen::action("draw_weapon", acumen::scoped_state{}, draw_next, acumen::scoped_state{}),
    acumen::action("attack",      atk_req,                atk_next,  acumen::scoped_state{}),
  };
  acumen::scoped_state goal_state; goal_state.set(enemy_dead, true);
  std::vector<acumen::goal> goals = { acumen::goal{ "kill", acumen::scoped_state{}, goal_state } };

  acumen::system sys(&reg, metrics, goals, actions);

  astar<acumen::astar_data>::container scratch;
  acumen::solution_cache cache;
  std::array<const acumen::action*, 8> buf{};
  const uint64_t goal_id = 1;

  // общий вызов decide: цель/scratch фиксированы, варьируем старт, кеш и выходной буфер.
  auto solve = [&](const acumen::state& start, acumen::solution_cache* cache_ptr,
                   std::span<const acumen::action*> out) {
    acumen::decide_params dp;
    dp.start = start; dp.goal = goal_state; dp.goal_id = goal_id;
    dp.scratch = &scratch; dp.cache = cache_ptr;
    return sys.decide(dp, out);
  };

  SUBCASE("relevant_mask covers only plan-affecting bits") {
    REQUIRE(sys.relevant_mask().test(has_weapon));
    REQUIRE(sys.relevant_mask().test(enemy_dead));
    REQUIRE(sys.relevant_mask().test(enemy_near) == false); // не влияет на план
  }

  SUBCASE("first decide misses, fills cache, returns the full plan") {
    acumen::state start; // всё false
    const size_t n = solve(start, &cache, buf);
    REQUIRE(n == 2);
    REQUIRE(buf[0]->name == "draw_weapon");
    REQUIRE(buf[1]->name == "attack");
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.misses() == 1);
    REQUIRE(cache.hits() == 0);
  }

  SUBCASE("second decide with same key hits and returns the same plan") {
    acumen::state start;
    solve(start, &cache, buf);            // miss
    std::array<const acumen::action*, 8> buf2{};
    const size_t n = solve(start, &cache, buf2); // hit
    REQUIRE(n == 2);
    REQUIRE(buf2[0]->name == "draw_weapon");
    REQUIRE(buf2[1]->name == "attack");
    REQUIRE(cache.hits() == 1);
    REQUIRE(cache.size() == 1);
  }

  SUBCASE("irrelevant bit collapses to the same key (enemy_near ignored)") {
    acumen::state a;                          // enemy_near = false
    acumen::state b; b.set(enemy_near, true); // отличается ТОЛЬКО незначащим битом
    solve(a, &cache, buf);            // miss
    const size_t n = solve(b, &cache, buf); // hit
    REQUIRE(n == 2);
    REQUIRE(cache.hits() == 1);
    REQUIRE(cache.size() == 1); // одна запись на оба состояния
  }

  SUBCASE("relevant bit changes the key (has_weapon distinguishes plans)") {
    acumen::state a;                          // has_weapon false -> [draw_weapon, attack]
    acumen::state b; b.set(has_weapon, true); // has_weapon true  -> [attack]
    const size_t na = solve(a, &cache, buf);
    REQUIRE(na == 2);
    std::array<const acumen::action*, 8> buf2{};
    const size_t nb = solve(b, &cache, buf2);
    REQUIRE(nb == 1);
    REQUIRE(buf2[0]->name == "attack");
    REQUIRE(cache.size() == 2); // два разных ключа
  }

  SUBCASE("budget full: insert skipped, decide still solves correctly") {
    acumen::solution_cache tiny(acumen::solution_cache::entry_bytes); // ~1 запись
    REQUIRE(tiny.capacity_entries() == 1);
    acumen::state a; solve(a, &tiny, buf); // занял слот
    REQUIRE(tiny.size() == 1);
    acumen::state b; b.set(has_weapon, true);
    const size_t nb = solve(b, &tiny, buf); // miss, кешировать некуда
    REQUIRE(nb == 1);                 // всё равно верно (живой решатель)
    REQUIRE(buf[0]->name == "attack");
    REQUIRE(tiny.size() == 1);        // бюджет не превышен
  }

  SUBCASE("merge folds warm entries from another cache") {
    acumen::solution_cache c1, c2;
    acumen::state a; solve(a, &c1, buf);
    acumen::state b; b.set(has_weapon, true); solve(b, &c2, buf);
    REQUIRE(c1.size() == 1);
    REQUIRE(c2.size() == 1);
    c1.merge(c2);
    REQUIRE(c1.size() == 2); // влились записи из c2
  }
}
