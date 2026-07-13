#include <cstdint>

#include <devils_engine/act/stat_accessors.h>
#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>
#include <doctest/doctest.h>

// register_stats статической рефлексией генерирует scope getter + локальные field/add_field функции.

using namespace devils_engine::act;

namespace {
struct test_stats {
  int64_t strength = 0;
  float agility = 0.0f;
  int32_t luck = 0;
};

enum class stat_test_domain : uint32_t { stats = 1 }; // домен catalogue для эффектов add_<field>

struct needs_stats {
  int32_t morale = 0;
  double energy = 0.0;
};

struct defaulted_stats {
  int32_t health = 100;
  float stamina = 25.0f;
  int64_t reputation = -3;
};

struct combined_scope {
  test_stats* primary = nullptr;
  needs_stats* needs = nullptr;
  bool valid() const noexcept {
    return primary != nullptr || needs != nullptr;
  }
};

test_stats* get_primary(combined_scope s) noexcept {
  return s.primary;
}
needs_stats* get_needs(combined_scope s) noexcept {
  return s.needs;
}

enum class multi_stat_domain : uint32_t { primary = 11,
                                          needs = 12 };

void register_test_stats(devils_script::system& sys) {
  register_stats<test_stats, combined_scope, &get_primary, stat_test_domain::stats>(sys, "stats");
}
} // namespace

static_assert(numeric_stats_aggregate<test_stats>);
static_assert(numeric_stats_aggregate<needs_stats>);
static_assert(numeric_stats_aggregate<defaulted_stats>);
static_assert(!numeric_stats_aggregate<std::string>);

TEST_CASE("stat_accessors: рефлексия генерирует ds-чтение полей [stats][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  register_test_stats(sys);

  test_stats st{10, 2.0f, 5};
  const combined_scope scope{&st, nullptr};

  const auto c = sys.parse<int64_t, combined_scope>("s", "stats.strength + stats.luck");
  devils_script::context vm;
  vm.set_arg(0, scope);
  c.process(&vm);
  CHECK(vm.get_return<int64_t>() == 15); // 10 + 5
}

TEST_CASE("stat_accessors: add_<field> мутирует компонент [stats][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  register_test_stats(sys);

  test_stats st{10, 2.0f, 5};
  const combined_scope scope{&st, nullptr};

  const auto c = sys.parse<void, combined_scope>("a", "stats = { add_strength(5) }");
  devils_script::context vm;
  vm.set_arg(0, scope);
  c.process(&vm);
  CHECK(st.strength == 15); // 10 += 5
}

TEST_CASE("generic stats initialize every numeric aggregate field [stats]") {
  const auto stats = initialize_stats<test_stats>([](auto Index, std::string_view) {
    return double(decltype(Index)::value + 1) * 2.0;
  });
  CHECK(stats.strength == 2);
  CHECK(stats.agility == doctest::Approx(4.0f));
  CHECK(stats.luck == 6);

  const auto direct = make_stats<needs_stats>(7, 3.5);
  CHECK(direct.morale == 7);
  CHECK(direct.energy == doctest::Approx(3.5));
}

TEST_CASE("generic stats let projects choose C++ defaults or reflected initialization [stats]") {
  const auto defaults = initialize_stats<defaulted_stats>();
  CHECK(defaults.health == 100);
  CHECK(defaults.stamina == doctest::Approx(25.0f));
  CHECK(defaults.reputation == -3);

  const auto reflected = initialize_stats<defaulted_stats>([](auto Index, std::string_view) {
    return double(decltype(Index)::value + 1) * 10.0;
  });
  CHECK(reflected.health == 10);
  CHECK(reflected.stamina == doctest::Approx(20.0f));
  CHECK(reflected.reputation == 30);
}

TEST_CASE("two stats aggregates coexist in one ds scope [stats][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  register_stats<test_stats, combined_scope, &get_primary, multi_stat_domain::primary>(sys, "stats");
  register_stats<needs_stats, combined_scope, &get_needs, multi_stat_domain::needs>(sys, "combat_stats");

  test_stats primary{10, 2.0f, 5};
  needs_stats needs{3, 8.0};
  const combined_scope scope{&primary, &needs};

  const auto read = sys.parse<double, combined_scope>("read_two", "stats.strength + combat_stats.energy");
  devils_script::context vm;
  vm.set_arg(read.find_arg("root"), scope);
  read.process(&vm);
  CHECK(vm.get_return<double>() == doctest::Approx(18.0));

  const auto add = sys.parse<void, combined_scope>("add_stats", "stats = { add_strength(2) }");
  vm.clear();
  vm.set_arg(add.find_arg("root"), scope);
  add.process(&vm);

  const auto add_combat = sys.parse<void, combined_scope>("add_combat", "combat_stats = { add_morale(4) }");
  vm.clear();
  vm.set_arg(add_combat.find_arg("root"), scope);
  add_combat.process(&vm);
  CHECK(primary.strength == 12);
  CHECK(needs.morale == 7);
}
