#include <doctest/doctest.h>

#include <vector>

#include "devils_engine/catalogue/introspection.h"

using namespace devils_engine;

namespace {

enum class test_domain : uint64_t {
  gameplay = 1,
  service = 2
};

static int g_gold = 0;

int add_gold(int amount, int multiplier) {
  g_gold += amount * multiplier;
  return g_gold;
}

void reset_gold() noexcept {
  g_gold = 0;
}

struct wallet {
  int gold = 0;

  int add(int amount) {
    gold += amount;
    return gold;
  }

  int get() const {
    return gold;
  }
};

struct multiply_functor {
  constexpr int operator()(int a, int b) const {
    return a * b;
  }
};

constexpr multiply_functor multiply{};

struct recording_intro final : catalogue::introspection_interface {
  size_t enter_count = 0;
  size_t exit_count = 0;
  size_t skipped_count = 0;
  std::string_view last_name;
  std::vector<std::string> last_arg_values;
  catalogue::call_decision decision = catalogue::call_decision::execute;

  catalogue::call_decision enter(const catalogue::call_info& info) override {
    ++enter_count;
    last_name = info.function_name;
    last_arg_values.clear();
    for (const auto& arg : info.arguments) {
      last_arg_values.push_back(arg.printable ? arg.value : std::string{"<opaque>"});
    }
    return decision;
  }

  void exit(const catalogue::call_info&, uint64_t) override {
    ++exit_count;
  }

  void skipped(const catalogue::call_info&) override {
    ++skipped_count;
  }
};

}

TEST_CASE("catalogue wraps free functions with mirrored arguments") {
  using add_gold_t = catalogue::domain<test_domain::gameplay>::fn_traits<&add_gold, "add_gold", "amount", "multiplier">;
  constexpr auto add_gold_fn = add_gold_t::fn_ptr;

  static_assert(add_gold_t::argument_count == 2);
  static_assert(add_gold_t::function_id == utils::murmur_hash64A("add_gold"));
  static_assert(std::is_same_v<decltype(add_gold_fn), int(* const)(int, int)>);

  recording_intro intro;
  catalogue::domain<test_domain::gameplay>::set_introspection(&intro);
  g_gold = 0;

  const int res = add_gold_fn(5, 3);

  CHECK(res == 15);
  CHECK(g_gold == 15);
  CHECK(intro.enter_count == 1);
  CHECK(intro.exit_count == 1);
  CHECK(intro.last_name == "add_gold");
  REQUIRE(intro.last_arg_values.size() == 2);
  CHECK(intro.last_arg_values[0] == "5");
  CHECK(intro.last_arg_values[1] == "3");

  catalogue::domain<test_domain::gameplay>::set_introspection(nullptr);
}

TEST_CASE("catalogue wrapper is a direct call when introspection is not set") {
  using add_gold_t = catalogue::domain<test_domain::gameplay>::fn_traits<&add_gold, "add_gold", "amount", "multiplier">;
  constexpr auto add_gold_fn = add_gold_t::fn_ptr;

  catalogue::domain<test_domain::gameplay>::set_introspection(nullptr);
  g_gold = 0;

  const int res = add_gold_fn(4, 2);

  CHECK(res == 8);
  CHECK(g_gold == 8);
}

TEST_CASE("catalogue dry-run skips wrapped free functions") {
  using add_gold_t = catalogue::domain<test_domain::gameplay>::fn_traits<&add_gold, "add_gold", "amount", "multiplier">;
  constexpr auto add_gold_fn = add_gold_t::fn_ptr;

  recording_intro intro;
  intro.decision = catalogue::call_decision::skip;
  catalogue::domain<test_domain::gameplay>::set_introspection(&intro);
  g_gold = 0;

  const int res = add_gold_fn(5, 3);

  CHECK(res == 0);
  CHECK(g_gold == 0);
  CHECK(intro.enter_count == 1);
  CHECK(intro.exit_count == 0);
  CHECK(intro.skipped_count == 1);

  catalogue::domain<test_domain::gameplay>::set_introspection(nullptr);
}

TEST_CASE("catalogue wraps member functions") {
  using add_t = catalogue::domain<test_domain::gameplay>::fn_traits<&wallet::add, "wallet.add", "self", "amount">;
  constexpr auto add_fn = add_t::fn_ptr;

  static_assert(std::is_same_v<decltype(add_fn), int(* const)(wallet&, int)>);

  recording_intro intro;
  catalogue::domain<test_domain::gameplay>::set_introspection(&intro);

  wallet w;
  const int res = add_fn(w, 7);

  CHECK(res == 7);
  CHECK(w.gold == 7);
  CHECK(intro.enter_count == 1);
  REQUIRE(intro.last_arg_values.size() == 2);
  CHECK(intro.last_arg_values[0] == "<opaque>");
  CHECK(intro.last_arg_values[1] == "7");

  catalogue::domain<test_domain::gameplay>::set_introspection(nullptr);
}

TEST_CASE("catalogue wraps const member functions") {
  using get_t = catalogue::domain<test_domain::gameplay>::fn_traits<&wallet::get, "wallet.get", "self">;
  constexpr auto get_fn = get_t::fn_ptr;

  static_assert(std::is_same_v<decltype(get_fn), int(* const)(const wallet&)>);

  wallet w;
  w.gold = 9;
  CHECK(get_fn(w) == 9);
}

TEST_CASE("catalogue wraps structural functors") {
  using mult_t = catalogue::domain<test_domain::service>::fn_traits<multiply, "multiply", "a", "b">;
  constexpr auto mult_fn = mult_t::fn_ptr;

  static_assert(std::is_same_v<decltype(mult_fn), int(* const)(int, int)>);
  CHECK(mult_fn(6, 7) == 42);
}

TEST_CASE("catalogue statistics introspection records rolling timings") {
  using reset_t = catalogue::domain<test_domain::service>::fn_traits<&reset_gold, "reset_gold">;
  constexpr auto reset_fn = reset_t::fn_ptr;

  catalogue::statistics_introspection<4> stats;
  catalogue::domain<test_domain::service>::set_introspection(&stats);

  reset_fn();
  reset_fn();

  CHECK(stats.count() == 2);
  CHECK(stats.average_mcs(reset_t::function_id) >= 0.0);

  catalogue::domain<test_domain::service>::set_introspection(nullptr);
}
