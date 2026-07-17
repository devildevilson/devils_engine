#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <devils_engine/act/function.h>
#include <devils_engine/act/registry.h>
#include <devils_engine/aesthetics/world.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/prefab/prefab_registry.h>
#include <devils_engine/prefab/resource.h>
#include <doctest/doctest.h>

// Обкатка prefab_registry (движковый механизм): формы компонента (data/list/callback/reference/custom)
// + on_construct-хук (получает SpawnArgs → spawn_at в точке) + FIELD-LEVEL наследование (наследник
// перекрывает только заданные поля data-компонента; list/callback/reference заменяются целиком).
// Второй кейс — что реестр СПОСОБЕН описать простого актора: GOAP по имени (reference) и inline-ds/имя
// (custom), проверяем без затягивания demiurg/ds — резолверы замыкают заглушки проекта.

using namespace devils_engine;
using namespace devils_engine::prefab;

namespace {
struct t_stats {
  int64_t hp = 0;
  int64_t atk = 0;
}; // data
struct t_flags {
  std::vector<std::string> names;
}; // list
struct t_on_pickup {
  uint64_t fn = 0;
}; // callback
struct t_spawned {
  bool ok = false;
}; // derived (construct-хук)

void effect_grab(const act::exec_context&) {} // натив для резолва callback-имени

// «актор»: GOAP по имени (reference) + мысль-скрипт inline-ds ИЛИ по имени (custom).
struct t_goap_ref {
  uint64_t config = 0;
}; // reference: имя → id конфига (заглушка)
struct t_think {
  std::string source;
  bool is_inline = false;
  uint64_t fn = 0;
}; // custom: блок vs имя
struct t_pos {
  float x = 0.0f, y = 0.0f;
}; // derived: точка спавна (из SpawnArgs)

struct spawn_at_args {
  float x = 0.0f, y = 0.0f;
}; // проектный SpawnArgs (точка спавна)
} // namespace

TEST_CASE("prefab_registry: data/list/callback + construct + наследование [prefab]") {
  act::registry fns;
  fns.reg("grab", std::make_unique<act::native_function<void>>(&effect_grab));

  prefab_registry<> reg;
  reg.data<t_stats>("stats", component_flag::required);
  reg.list<t_flags, std::string>("flags");
  reg.callback<t_on_pickup>("on_pickup");
  reg.on_construct([](aesthetics::entityid_t id, aesthetics::world& w, const no_spawn_args&) {
    w.create<t_spawned>(id, t_spawned{true});
  });

  prefab_load_context lc{&fns};

  REQUIRE(reg.add_prefab("goblin",
                         "stats = { hp = 10, atk = 3 }\n"
                         "flags = [ small, green ]\n"
                         "on_pickup = grab\n",
                         lc));
  REQUIRE(reg.add_prefab("ogre",
                         "base = goblin\n"
                         "stats = { hp = 40 }\n",
                         lc)); // переопределяет ТОЛЬКО hp; atk наследуется от goblin (field-level)

  aesthetics::world w;
  const auto id = reg.spawn("ogre", w);

  const auto* st = w.get<t_stats>(id);
  REQUIRE(st != nullptr);
  CHECK(st->hp == 40); // из ogre (переопределение поля)
  CHECK(st->atk == 3); // FIELD-LEVEL: унаследовано от goblin (ogre его не задавал)

  const auto* fl = w.get<t_flags>(id);
  REQUIRE(fl != nullptr); // унаследован от goblin
  REQUIRE(fl->names.size() == 2);
  CHECK(fl->names[0] == "small");
  CHECK(fl->names[1] == "green");

  const auto* op = w.get<t_on_pickup>(id);
  REQUIRE(op != nullptr); // унаследован от goblin
  CHECK(op->fn == utils::string_hash("grab"));

  const auto* sp = w.get<t_spawned>(id);
  REQUIRE(sp != nullptr); // добавлен on_construct-хуком
  CHECK(sp->ok == true);
}

TEST_CASE("prefab resource exposes a generic logical name and raw TAVL text [prefab][config][demiurg]") {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "devils_engine_prefab_resource_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "prefab");
  {
    std::ofstream config(root / "core" / "prefab" / "food.tavl");
    config << "stats = { hp = 5 }\n";
  }

  demiurg::module_system modules(root.generic_string() + "/");
  modules.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});
  demiurg::resource_system resources;
  resources.register_type<prefab_resource>("prefab", "tavl");
  resources.parse_resources(&modules);

  auto* resource = resources.get<prefab_resource>("prefab/food");
  REQUIRE(resource != nullptr);
  resource->load(utils::safe_handle_t{});
  CHECK(resource->prefab_name() == "food");
  CHECK(resource->text().find("hp = 5") != std::string_view::npos);

  fs::remove_all(root);
}

TEST_CASE("prefab_registry: описание актора — reference(GOAP)/inline-ds + spawn_at [prefab]") {
  prefab_registry<spawn_at_args> reg;
  reg.data<t_stats>("stats");

  // reference: имя GOAP-конфига → id (в реальном проекте — handle из resource_system; здесь заглушка).
  reg.reference<t_goap_ref>("goap", [](std::string_view name, const prefab_load_context&) {
    return t_goap_ref{utils::string_hash(name)};
  });

  // custom (inline-ds): builder видит сырой текст. Блок `{ ... }` → это инлайн-скрипт (проект компилирует
  // через ds), иначе имя → callback. Наследник заменяет целиком (chain.back()).
  reg.custom("think", [](const std::vector<std::string_view>& chain, const prefab_load_context&) {
    std::string_view raw = chain.back();
    t_think t{};
    if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}') {
      t.is_inline = true;
      t.source = std::string(raw.substr(1, raw.size() - 2)); // тело скрипта (в проекте → ds::parse)
    } else {
      t.fn = utils::string_hash(raw);
    }
    return [t](aesthetics::entityid_t id, aesthetics::world& w) {
      w.create<t_think>(id, t);
    };
  });

  // spawn_at: on_construct кладёт позицию из SpawnArgs (в tile_frontier — actor_position).
  reg.on_construct([](aesthetics::entityid_t id, aesthetics::world& w, const spawn_at_args& a) {
    w.create<t_pos>(id, t_pos{a.x, a.y}); // derived-компонент: доказываем, что args доходят до хука
  });

  prefab_load_context lc{nullptr};
  REQUIRE(reg.add_prefab("predator",
                         "stats = { hp = 5, atk = 1 }\n"
                         "goap = hunt\n"                                  // reference по имени
                         "think = { if hungry then seek else wander }\n", // inline-ds (блок)
                         lc));
  REQUIRE(reg.add_prefab("grazer",
                         "base = predator\n"
                         "think = wander\n",
                         lc)); // whole-value override: имя вместо инлайна

  aesthetics::world w;

  const auto pred = reg.spawn("predator", w, spawn_at_args{7.0f, 9.0f});
  const auto* gr = w.get<t_goap_ref>(pred);
  REQUIRE(gr != nullptr);
  CHECK(gr->config == utils::string_hash("hunt")); // GOAP разрешён по имени

  const auto* th = w.get<t_think>(pred);
  REQUIRE(th != nullptr);
  CHECK(th->is_inline == true); // inline-ds распознан (сырой блок доехал до билдера)
  CHECK(th->source.find("hungry") != std::string::npos);

  // spawn_at: construct-хук получил точку спавна (derived-компонент t_pos из args).
  const auto* ps = w.get<t_pos>(pred);
  REQUIRE(ps != nullptr);
  CHECK(ps->x == 7.0f);
  CHECK(ps->y == 9.0f);
  const auto* st = w.get<t_stats>(pred);
  REQUIRE(st != nullptr);
  CHECK(st->hp == 5); // data-компонент из конфига (не тронут хуком)
  CHECK(st->atk == 1);

  const auto graz = reg.spawn("grazer", w, spawn_at_args{1.0f, 2.0f});
  const auto* th2 = w.get<t_think>(graz);
  REQUIRE(th2 != nullptr);
  CHECK(th2->is_inline == false); // наследник заменил инлайн именем
  CHECK(th2->fn == utils::string_hash("wander"));
  const auto* gr2 = w.get<t_goap_ref>(graz);
  REQUIRE(gr2 != nullptr); // reference унаследован от predator
  CHECK(gr2->config == utils::string_hash("hunt"));
}
