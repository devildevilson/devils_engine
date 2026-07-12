#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <devils_engine/aesthetics/world.h>
#include <devils_engine/act/registry.h>
#include <devils_engine/act/function.h>

#include "core/prefab.h"

// Обкатка prefab_registry: три формы компонента (data/list/callback) + on_construct-хук + спавн в мир
// + наследование (component-level: наследник переопределяет одноимённый компонент базы).

using namespace devils_engine;
using namespace tile_frontier::core;

namespace {
struct t_stats { int64_t hp = 0; int64_t atk = 0; };          // data
struct t_flags { std::vector<std::string> names; };            // list
struct t_on_pickup { uint64_t fn = 0; };                       // callback
struct t_spawned { bool ok = false; };                         // derived (construct-хук)

void effect_grab(const act::exec_context&) {}                  // натив для резолва callback-имени
}

TEST_CASE("prefab_registry: data/list/callback + construct + наследование [prefab]") {
  act::registry fns;
  fns.reg("grab", std::make_unique<act::native_function<void>>(&effect_grab));

  prefab_registry reg;
  reg.data<t_stats>("stats", component_flag::required);
  reg.list<t_flags, std::string>("flags");
  reg.callback<t_on_pickup>("on_pickup");
  reg.on_construct([](aes::entityid_t id, aes::world& w) { w.create<t_spawned>(id, t_spawned{ true }); });

  prefab_load_context lc{ &fns };

  REQUIRE(reg.add_prefab("goblin",
    "stats = { hp = 10, atk = 3 }\n"
    "flags = [ small, green ]\n"
    "on_pickup = grab\n", lc));
  REQUIRE(reg.add_prefab("ogre",
    "base = goblin\n"
    "stats = { hp = 40, atk = 12 }\n", lc)); // переопределяет stats; flags/on_pickup наследует

  aes::world w;
  const auto id = reg.spawn("ogre", w);

  const auto* st = w.get<t_stats>(id);
  REQUIRE(st != nullptr);
  CHECK(st->hp == 40);   // из ogre (переопределение)
  CHECK(st->atk == 12);

  const auto* fl = w.get<t_flags>(id);
  REQUIRE(fl != nullptr);              // унаследован от goblin
  REQUIRE(fl->names.size() == 2);
  CHECK(fl->names[0] == "small");
  CHECK(fl->names[1] == "green");

  const auto* op = w.get<t_on_pickup>(id);
  REQUIRE(op != nullptr);              // унаследован от goblin
  CHECK(op->fn == utils::string_hash("grab"));

  const auto* sp = w.get<t_spawned>(id);
  REQUIRE(sp != nullptr);              // добавлен on_construct-хуком
  CHECK(sp->ok == true);
}
