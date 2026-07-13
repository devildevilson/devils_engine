#include <doctest/doctest.h>

#include <cstdint>

#include <devils_script/system.h>
#include <devils_script/context.h>
#include <devils_script/container.h>

#include <devils_engine/act/stat_accessors.h>

// Обкатка generic-характеристик: register_stat_accessors<StatsT, Scope, Getter, Domain> статической
// рефлексией автогенерирует ds-аксессоры чтения (<field>) и прибавления (add_<field>) на каждое поле.
// Скоуп по умолчанию — stat_scope<StatsT> (getter = stat_scope_getter, отдаёт .ptr).

using namespace devils_engine::act;

namespace {
struct test_stats {
  int64_t strength = 0;
  float   agility  = 0.0f;
  int32_t luck     = 0;
};

enum class stat_test_domain : uint32_t { stats = 1 }; // домен catalogue для эффектов add_<field>

void register_test_stats(devils_script::system& sys) {
  register_stat_accessors<test_stats, stat_scope<test_stats>,
                          &stat_scope_getter<test_stats>, stat_test_domain::stats>(sys);
}

struct needs_stats {
  int32_t morale = 0;
  double energy = 0.0;
};

struct combined_scope {
  test_stats* primary = nullptr;
  needs_stats* needs = nullptr;
  bool valid() const noexcept { return primary != nullptr || needs != nullptr; }
};

test_stats* get_primary(combined_scope s) noexcept { return s.primary; }
needs_stats* get_needs(combined_scope s) noexcept { return s.needs; }

enum class multi_stat_domain : uint32_t { primary = 11, needs = 12 };
}

static_assert(numeric_stats_aggregate<test_stats>);
static_assert(numeric_stats_aggregate<needs_stats>);
static_assert(!numeric_stats_aggregate<std::string>);

TEST_CASE("stat_accessors: рефлексия генерирует ds-чтение полей [stats][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  register_test_stats(sys);

  test_stats st{ 10, 2.0f, 5 };
  const stat_scope<test_stats> scope{ 0, &st };

  const auto c = sys.parse<int64_t, stat_scope<test_stats>>("s", "strength + luck");
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

  test_stats st{ 10, 2.0f, 5 };
  const stat_scope<test_stats> scope{ 0, &st };

  const auto c = sys.parse<void, stat_scope<test_stats>>("a", "add_strength(5)");
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

TEST_CASE("two stats aggregates coexist in one ds scope [stats][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  register_stat_accessors<test_stats, combined_scope, &get_primary, multi_stat_domain::primary>(sys, "primary_");
  register_stat_accessors<needs_stats, combined_scope, &get_needs, multi_stat_domain::needs>(sys, "needs_");

  test_stats primary{10, 2.0f, 5};
  needs_stats needs{3, 8.0};
  const combined_scope scope{&primary, &needs};

  const auto read = sys.parse<double, combined_scope>("read_two", "primary_strength + needs_energy");
  devils_script::context vm;
  vm.set_arg(read.find_arg("root"), scope);
  read.process(&vm);
  CHECK(vm.get_return<double>() == doctest::Approx(18.0));

  const auto add = sys.parse<void, combined_scope>("add_two", "{ add_primary_strength(2), add_needs_morale(4) }");
  vm.clear();
  vm.set_arg(add.find_arg("root"), scope);
  add.process(&vm);
  CHECK(primary.strength == 12);
  CHECK(needs.morale == 7);
}
