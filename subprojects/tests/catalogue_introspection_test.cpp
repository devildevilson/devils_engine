#include <doctest/doctest.h>

#include <vector>

#include "devils_engine/catalogue/introspection.h"

using namespace devils_engine;

namespace devils_engine::catalogue_test_types {
struct very_long_argument_type_name_that_should_not_fit_into_catalogue_value_buffer {
  int value = 0;
};
}

namespace {

namespace domains {
constexpr size_t gameplay = 1;
constexpr size_t service = 2;
}

enum class enum_domain : uint64_t {
  gameplay = 11,
  service = 12
};

static int g_gold = 0;

int add_gold(int amount, int multiplier) {
  g_gold += amount * multiplier;
  return g_gold;
}

void reset_gold() noexcept {
  g_gold = 0;
}

void inspect_long_type(const devils_engine::catalogue_test_types::very_long_argument_type_name_that_should_not_fit_into_catalogue_value_buffer&) {}

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

// Хелпер: сконфигурировать домен на режим statistics с заданным store (невиртуальный switch).
static catalogue::introspection stats_cfg(catalogue::statistics_store& store) {
  return catalogue::introspection{catalogue::introspection_mode::statistics, 0, &store};
}

}

TEST_CASE("catalogue wraps free functions with mirrored arguments") {
  using add_gold_t = catalogue::domain<domains::gameplay>::fn_traits<&add_gold, "add_gold", "amount", "multiplier">;
  constexpr auto add_gold_fn = add_gold_t::fn_ptr;

  static_assert(add_gold_t::argument_count == 2);
  static_assert(add_gold_t::function_id == utils::murmur_hash64A("add_gold"));
  static_assert(std::is_same_v<decltype(add_gold_fn), int(* const)(int, int)>);

  catalogue::statistics_store store(4);
  const auto cfg = stats_cfg(store);
  catalogue::domain<domains::gameplay>::set_introspection(&cfg);
  g_gold = 0;

  const int res = add_gold_fn(5, 3);

  CHECK(res == 15);
  CHECK(g_gold == 15);
  CHECK(store.count() == 1);
  const auto* rec = store.find(add_gold_t::function_id);
  REQUIRE(rec != nullptr);
  CHECK(rec->call_count == 1);
  CHECK(rec->name == "add_gold");

  catalogue::domain<domains::gameplay>::set_introspection(nullptr);
}

TEST_CASE("catalogue domain accepts enum values") {
  using reset_t = catalogue::domain<enum_domain::service>::fn_traits<&reset_gold, "reset_gold">;
  constexpr auto reset_fn = reset_t::fn_ptr;

  static_assert(reset_t::domain_id == static_cast<utils::id>(enum_domain::service));
  static_assert(reset_t::function_id == utils::murmur_hash64A("reset_gold"));
  static_assert(std::is_same_v<decltype(reset_fn), void(* const)()>);

  g_gold = 42;
  reset_fn();
  CHECK(g_gold == 0);
}

TEST_CASE("catalogue wrapper is a direct call when introspection is not set") {
  using add_gold_t = catalogue::domain<domains::gameplay>::fn_traits<&add_gold, "add_gold", "amount", "multiplier">;
  constexpr auto add_gold_fn = add_gold_t::fn_ptr;

  catalogue::domain<domains::gameplay>::set_introspection(nullptr);
  g_gold = 0;

  const int res = add_gold_fn(4, 2);

  CHECK(res == 8);
  CHECK(g_gold == 8);
}

TEST_CASE("catalogue off mode is a direct call") {
  using add_gold_t = catalogue::domain<domains::gameplay>::fn_traits<&add_gold, "add_gold", "amount", "multiplier">;
  constexpr auto add_gold_fn = add_gold_t::fn_ptr;

  const catalogue::introspection cfg{}; // mode == off по умолчанию
  catalogue::domain<domains::gameplay>::set_introspection(&cfg);
  g_gold = 0;

  CHECK(add_gold_fn(4, 2) == 8);
  CHECK(g_gold == 8);

  catalogue::domain<domains::gameplay>::set_introspection(nullptr);
}

TEST_CASE("catalogue loc_fn_t records call site into statistics") {
  using add_gold_t = catalogue::domain<domains::gameplay>::fn_traits<&add_gold, "add_gold", "amount", "multiplier">;
  using add_gold_loc_fn_t = add_gold_t::loc_fn_t;

  catalogue::statistics_store store(4);
  const auto cfg = stats_cfg(store);
  catalogue::domain<domains::gameplay>::set_introspection(&cfg);
  g_gold = 0;

  const uint32_t expected_line = __LINE__ + 1;
  const int res = add_gold_loc_fn_t{}(2, 4);

  CHECK(res == 8);
  const auto* rec = store.find(add_gold_t::function_id);
  REQUIRE(rec != nullptr);
  CHECK(rec->line == expected_line);
  CHECK(rec->file.find("catalogue_introspection_test.cpp") != std::string_view::npos);

  catalogue::domain<domains::gameplay>::set_introspection(nullptr);
}

TEST_CASE("catalogue wraps member functions") {
  using add_t = catalogue::domain<domains::gameplay>::fn_traits<&wallet::add, "wallet.add", "self", "amount">;
  constexpr auto add_fn = add_t::fn_ptr;

  static_assert(std::is_same_v<decltype(add_fn), int(* const)(wallet&, int)>);

  catalogue::statistics_store store(4);
  const auto cfg = stats_cfg(store);
  catalogue::domain<domains::gameplay>::set_introspection(&cfg);

  wallet w;
  const int res = add_fn(w, 7);

  CHECK(res == 7);
  CHECK(w.gold == 7);
  const auto* rec = store.find(add_t::function_id);
  REQUIRE(rec != nullptr);
  CHECK(rec->call_count == 1);

  catalogue::domain<domains::gameplay>::set_introspection(nullptr);
}

TEST_CASE("catalogue wraps const member functions") {
  using get_t = catalogue::domain<domains::gameplay>::fn_traits<&wallet::get, "wallet.get", "self">;
  constexpr auto get_fn = get_t::fn_ptr;

  static_assert(std::is_same_v<decltype(get_fn), int(* const)(const wallet&)>);

  wallet w;
  w.gold = 9;
  CHECK(get_fn(w) == 9);
}

TEST_CASE("catalogue wraps structural functors") {
  using mult_t = catalogue::domain<domains::service>::fn_traits<multiply, "multiply", "a", "b">;
  constexpr auto mult_fn = mult_t::fn_ptr;

  static_assert(std::is_same_v<decltype(mult_fn), int(* const)(int, int)>);
  CHECK(mult_fn(6, 7) == 42);
}

TEST_CASE("catalogue statistics introspection records rolling timings") {
  using reset_t = catalogue::domain<domains::service>::fn_traits<&reset_gold, "reset_gold">;
  constexpr auto reset_fn = reset_t::fn_ptr;

  catalogue::statistics_store stats(4);
  const auto cfg = stats_cfg(stats);
  catalogue::domain<domains::service>::set_introspection(&cfg);

  reset_fn();
  reset_fn();

  CHECK(stats.count() == 2);
  CHECK(stats.function_count() == 1);
  CHECK(stats.average_mcs(reset_t::function_id) >= 0.0);

  const auto* rec = stats.find(reset_t::function_id);
  REQUIRE(rec != nullptr);
  CHECK(rec->call_count == 2);
  CHECK(rec->name == "reset_gold");
  CHECK(rec->filled == 2);
  CHECK(rec->recent_average_mcs() >= 0.0);

  std::vector<uint64_t> ordered;
  rec->ordered_samples(ordered);
  CHECK(ordered.size() == 2);

  catalogue::domain<domains::service>::set_introspection(nullptr);
}

TEST_CASE("catalogue statistics introspection ring buffer wraps and keeps aggregates") {
  using reset_t = catalogue::domain<domains::service>::fn_traits<&reset_gold, "reset_gold">;
  constexpr auto reset_fn = reset_t::fn_ptr;

  catalogue::statistics_store stats(2); // окно из 2 замеров
  const auto cfg = stats_cfg(stats);
  catalogue::domain<domains::service>::set_introspection(&cfg);

  reset_fn();
  reset_fn();
  reset_fn(); // 3 вызова, окно 2 → буфер завёрнут, filled == 2

  const auto* rec = stats.find(reset_t::function_id);
  REQUIRE(rec != nullptr);
  CHECK(rec->call_count == 3);   // агрегат считает все вызовы
  CHECK(rec->filled == 2);       // а кольцо держит только последние 2
  CHECK(stats.count() == 3);

  std::vector<uint64_t> ordered;
  rec->ordered_samples(ordered);
  CHECK(ordered.size() == 2);

  stats.reset();
  CHECK(stats.count() == 0);
  CHECK(stats.function_count() == 0);
  CHECK(stats.find(reset_t::function_id) == nullptr);

  catalogue::domain<domains::service>::set_introspection(nullptr);
}

TEST_CASE("catalogue trace log-level escalates domain introspection to tracing") {
  using reset_t = catalogue::domain<domains::service>::fn_traits<&reset_gold, "reset_gold">;
  constexpr auto reset_fn = reset_t::fn_ptr;

  catalogue::logs().register_domain(catalogue::log_domain::gameplay, "gameplay");
  catalogue::statistics_store store(4);
  // базовый режим statistics, привязка к лог-домену gameplay
  const catalogue::introspection cfg{catalogue::introspection_mode::statistics, catalogue::log_domain::gameplay, &store};
  catalogue::domain<domains::service>::set_introspection(&cfg);

  // домен НЕ на trace → эффективный режим = statistics: замер пишется в store
  catalogue::logs().set_level(catalogue::log_domain::gameplay, catalogue::log_depth::off);
  reset_fn();
  CHECK(store.count() == 1);

  // домен НА trace → эскалация до tracing: статистика больше НЕ пишется (наблюдаемый признак смены режима)
  catalogue::logs().set_level(catalogue::log_domain::gameplay, catalogue::log_depth::trace);
  reset_fn();
  CHECK(store.count() == 1); // не изменилось — режим стал tracing

  catalogue::logs().set_level(catalogue::log_domain::gameplay, catalogue::log_depth::off);
  catalogue::domain<domains::service>::set_introspection(nullptr);
}
