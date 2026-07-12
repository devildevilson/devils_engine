#include <doctest/doctest.h>

#include <cstdint>

#include <devils_script/system.h>
#include <devils_script/context.h>
#include <devils_script/container.h>

#include "core/stat_accessors.h"

// Обкатка generic-характеристик: register_stat_accessors<StatsT> статической рефлексией
// автогенерирует ds-аксессоры чтения (<field>) и прибавления (add_<field>) на каждое поле.

using namespace tile_frontier::core;

namespace {
struct test_stats {
  int64_t strength = 0;
  float   agility  = 0.0f;
  int32_t luck     = 0;
};

enum class stat_test_domain : uint32_t { stats = 1 }; // домен catalogue для эффектов add_<field>
}

TEST_CASE("stat_accessors: рефлексия генерирует ds-чтение полей [stats][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  register_stat_accessors<test_stats, stat_test_domain::stats>(sys);

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
  register_stat_accessors<test_stats, stat_test_domain::stats>(sys);

  test_stats st{ 10, 2.0f, 5 };
  const stat_scope<test_stats> scope{ 0, &st };

  const auto c = sys.parse<void, stat_scope<test_stats>>("a", "add_strength(5)");
  devils_script::context vm;
  vm.set_arg(0, scope);
  c.process(&vm);
  CHECK(st.strength == 15); // 10 += 5
}
