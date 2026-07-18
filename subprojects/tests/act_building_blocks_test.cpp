#include <cstdint>
#include <string>

#include <devils_engine/act/building_blocks.h>
#include <devils_engine/act/packer.h>
#include <devils_engine/catalogue/deferred.h>
#include <devils_engine/utils/string_id.h>
#include <devils_script/context.h>
#include <devils_script/system.h>
#include <doctest/doctest.h>

using namespace devils_engine;

namespace {

// value-скоуп в стиле entity_scope: ≤16 байт, valid(), id для ключей catalogue.
struct block_scope {
  const int* data = nullptr;
  uint32_t id = 0;
  bool valid() const noexcept {
    return data != nullptr;
  }
};

void effect_touch(block_scope, double) {}
bool scope_even(const block_scope s) {
  return (s.id % 2u) == 0u;
}

bool native_ready(const act::entity_handle) noexcept {
  return true;
}
void native_grab(const act::entity_handle, const act::entity_handle) noexcept {}

struct block_domain_id {};
using block_strategy = catalogue::mt::preset::parallel_collect<0>;
using block_domain = catalogue::mt::domain<block_domain_id, block_strategy>;
using touch_deferred = block_domain::fn_traits<&effect_touch, "touch", "self", "value">;

struct grab_domain_id {};
using grab_strategy = catalogue::mt::preset::serial_elect<1>;
using grab_domain = catalogue::mt::domain<grab_domain_id, grab_strategy>;
using grab_deferred = grab_domain::fn_traits<&native_grab, "grab", "self", "target">;

const act::building_blocks& test_blocks() {
  static const act::building_blocks blocks = [] {
    act::building_blocks b;
    b.effect<touch_deferred>();          // ds-словарь: deferred-эффект
    b.pure<&scope_even>("even");         // ds-словарь: чистый предикат
    b.effect_native<grab_deferred>();    // исключение: act-only эффект
    b.native<&native_ready>("is_ready"); // исключение: act-only предикат (FSM-гвард)
    b.reg_interaction("grab", act::interaction{});
    return b;
  }();
  return blocks;
}

} // namespace

TEST_CASE("building blocks register the ds vocabulary once [act][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  test_blocks().register_ds(sys);

  // deferred-эффект виден парсеру под сигнатурой обычной C++-функции.
  const auto effect = sys.parse<void, block_scope>("touch_script", "touch = 2.5");
  CHECK(!effect.cmds.empty());

  // чистый предикат реально исполняется на value-скоупе.
  const auto pred = sys.parse<bool, block_scope>("even_script", "even");
  const int payload = 1;
  devils_script::context ctx;
  ctx.set_arg(0, block_scope{&payload, 4});
  pred.process(&ctx);
  CHECK(ctx.get_return<bool>());
  ctx.clear();
  ctx.set_arg(0, block_scope{&payload, 5});
  pred.process(&ctx);
  CHECK_FALSE(ctx.get_return<bool>());
}

TEST_CASE("building blocks install replays act natives and interactions [act]") {
  // install() повторяем: registry пересоздаётся на init/load, список — нет.
  for (int pass = 0; pass < 2; ++pass) {
    act::registry reg;
    test_blocks().install(reg);

    CHECK(reg.effect(utils::string_hash("grab")) != nullptr);
    CHECK(reg.predicate(utils::string_hash("is_ready")) != nullptr);
    // ds-only записи в act не попадают: семантические имена дают конфиг-скрипты.
    CHECK(reg.get(utils::string_hash("touch")) == nullptr);
    CHECK(reg.get(utils::string_hash("even")) == nullptr);

    const auto* desc = reg.interaction_of(utils::string_hash("grab"));
    REQUIRE(desc != nullptr);
    CHECK(desc->rule == act::arbitration::exclusive);
    CHECK(desc->target_scope == 1);
    CHECK(desc->self_claim);
  }
}

TEST_CASE("building blocks collide loudly with same-name registrations [act]") {
  act::registry reg;
  // конфиг-скрипт занял семантическое имя ⇒ native-запись обязана упасть, не подменить.
  reg.reg("grab", act::pack<&native_grab>());
  CHECK_THROWS(test_blocks().install(reg));
}
