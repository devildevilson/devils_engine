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

  SUBCASE("find_solution reaches the goal and uses the prerequisite action") {
    const act::exec_context ctx{};
    const auto start = sys.compute_state(ctx); // has_weapon/enemy_dead false, enemy_near true

    astar<acumen::astar_data>::container c;
    std::array<const acumen::action*, 8> buf{};
    const size_t n = acumen::find_solution(&sys, &c, start, goal_state, buf);

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
    const size_t n = acumen::find_solution(&sys, &c, already, goal_state, buf);
    REQUIRE(n == 0); // действий не нужно
  }

  SUBCASE("undersized buffer: full length returned, prefix written, no overflow") {
    const act::exec_context ctx{};
    const auto start = sys.compute_state(ctx);
    astar<acumen::astar_data>::container c;
    std::array<const acumen::action*, 1> small{};
    const size_t n = acumen::find_solution(&sys, &c, start, goal_state, small);
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
