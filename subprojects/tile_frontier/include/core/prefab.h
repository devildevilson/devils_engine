#ifndef TILE_FRONTIER_CORE_PREFAB_H
#define TILE_FRONTIER_CORE_PREFAB_H

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <tavl/parser.h>
#include <tavl/deserialize.h>

#include <devils_engine/aesthetics/world.h>
#include <devils_engine/aesthetics/common.h>
#include <devils_engine/act/registry.h>     // act::registry (резолв callback-имён)
#include <devils_engine/utils/core.h>       // utils::warn
#include <devils_engine/utils/string_id.h>  // utils::string_hash / utils::id

// prefab_registry — «рецепт сборки энтити из компонентов» (шаг 6). Проект регистрирует словарь
// компонентов по ТРЁМ формам задания в конфиге:
//   data<C>      — POD, tavl-deserialize по полям:  stats = { hunger = 0.2 }
//   list<C,Item> — контейнер элементов одного типа:  flags = [ hostile, grabbable ]
//   callback<C>  — имя act-функции (инлайн ds — позже):  on_pickup = pickup_gold
// + on_construct-хук достраивает DERIVED-компоненты (perception/cognition/transform — не с диска).
// Наследование (`base = ...`) — на уровне КОМПОНЕНТА: наследник целиком переопределяет одноимённый
// компонент базы, остальные наследует (field-level override — следующая итерация). Проектный holder.

namespace tile_frontier {
namespace core {

namespace aes = devils_engine::aesthetics;

enum class component_flag : uint32_t { none = 0, required = 1 };
inline component_flag operator|(component_flag a, component_flag b) noexcept {
  return component_flag(uint32_t(a) | uint32_t(b));
}
inline bool has_flag(component_flag v, component_flag f) noexcept {
  return (uint32_t(v) & uint32_t(f)) != 0;
}

// Контекст загрузки: резолв имён (callback → act-функция, ссылки → ресурсы). Пока только реестр функций.
struct prefab_load_context {
  const devils_engine::act::registry* functions = nullptr;
};

class prefab_registry {
public:
  using applier = std::function<void(aes::entityid_t, aes::world&)>;

  // data<C>: значение = tavl-блок, разбирается по полям в C; на спавне create<C>(id, C).
  template <typename C>
  void data(std::string name, const component_flag flags = component_flag::none) {
    specs_.emplace(std::move(name), component_spec{ flags,
      [](tavl::parser& p, tavl::ct_context& ctx, const prefab_load_context&) -> applier {
        C c{};
        tavl::deserialize(p, ctx, c);
        return [c](const aes::entityid_t id, aes::world& w) { w.create<C>(id, c); };
      }});
  }

  // list<C,Item>: значение = tavl-массив элементов Item; на спавне create<C>(id, C{items}).
  // (кастомный inserter для пул-хранилищ типа фракций — следующая итерация.)
  template <typename C, typename Item>
  void list(std::string name, const component_flag flags = component_flag::none) {
    specs_.emplace(std::move(name), component_spec{ flags,
      [](tavl::parser& p, tavl::ct_context& ctx, const prefab_load_context&) -> applier {
        std::vector<Item> items;
        tavl::deserialize(p, ctx, items);
        return [items](const aes::entityid_t id, aes::world& w) { w.create<C>(id, C{items}); };
      }});
  }

  // callback<C>: значение = имя act-функции; C держит хеш имени (C{ utils::id }). Резолв/валидация по
  // реестру — здесь по имени. Инлайн ds — следующая итерация.
  template <typename C>
  void callback(std::string name, const component_flag flags = component_flag::none) {
    specs_.emplace(std::move(name), component_spec{ flags,
      [](tavl::parser& p, tavl::ct_context&, const prefab_load_context& lc) -> applier {
        const auto [ev, err] = p.poll_event();
        const std::string fn_name = p.to_string(ev.token);
        const auto fid = devils_engine::utils::string_hash(fn_name);
        if (lc.functions != nullptr && !lc.functions->has(fid)) {
          devils_engine::utils::warn("prefab callback '{}': функция не найдена в реестре", fn_name);
        }
        return [fid](const aes::entityid_t id, aes::world& w) { w.create<C>(id, C{ fid }); };
      }});
  }

  void on_construct(std::function<void(aes::entityid_t, aes::world&)> fn) { construct_ = std::move(fn); }
  void on_destruct(std::function<void(aes::entityid_t, aes::world&)> fn) { destruct_ = std::move(fn); }

  // Разобрать текст префаба (один документ) и запомнить под именем `name`. Драйвит tavl-парсер:
  // на каждую строку `key = value` — либо base, либо диспетчер к зарегистрированному компоненту.
  bool add_prefab(std::string name, std::string_view text, const prefab_load_context& lc) {
    tavl::parser p;
    p.add_default_operator();
    p.flush(text);
    p.finish();

    loaded_prefab lp;
    tavl::ct_context ctx;
    using ev_t = tavl::event_type;
    for (;;) {
      const auto e = p.peek();
      if (e.type == ev_t::eof || e.type == ev_t::not_enought_data) { p.poll_event(); break; }
      if (e.type != ev_t::got_token && e.type != ev_t::got_row_identifier) { p.poll_event(); continue; }

      const auto [kev, kerr] = p.poll_event();
      const std::string key = p.to_string(kev.token);
      p.poll_event(); // '='

      if (key == "base") {
        const auto [bev, berr] = p.poll_event();
        lp.base = p.to_string(bev.token);
        continue;
      }
      const auto it = specs_.find(key);
      if (it == specs_.end()) {
        devils_engine::utils::warn("prefab '{}': неизвестный компонент '{}', пропущен", name, key);
        continue; // значение пропустится обычным ходом цикла
      }
      lp.appliers.emplace(key, it->second.parse(p, ctx, lc));
    }

    prefabs_.emplace(std::move(name), std::move(lp));
    return true;
  }

  // Заспавнить энтити по префабу: применить config-компоненты (с наследованием) + on_construct.
  aes::entityid_t spawn(const std::string& name, aes::world& w) const {
    const auto id = w.gen_entityid();

    // Слить цепочку наследования: базы применяются первыми, наследник переопределяет одноимённые
    // компоненты. Собираем итоговую карту component_name -> applier, идя от корня базы к наследнику.
    std::unordered_map<std::string, const applier*> merged;
    collect(name, merged, 0);
    for (const auto& [comp, ap] : merged) (*ap)(id, w);

    if (construct_) construct_(id, w);
    return id;
  }

  void despawn(const aes::entityid_t id, aes::world& w) const {
    if (destruct_) destruct_(id, w);
    // фактическое удаление сущности/компонентов — забота вызывающего/мира.
  }

  bool has_prefab(const std::string& name) const { return prefabs_.contains(name); }

private:
  struct component_spec {
    component_flag flags;
    std::function<applier(tavl::parser&, tavl::ct_context&, const prefab_load_context&)> parse;
  };
  struct loaded_prefab {
    std::string base;
    std::unordered_map<std::string, applier> appliers;
  };

  // Рекурсивно собрать appliers по цепочке base: сперва база (глубже), затем этот префаб (override).
  void collect(const std::string& name, std::unordered_map<std::string, const applier*>& out, int depth) const {
    if (depth > 32) { devils_engine::utils::warn("prefab '{}': слишком глубокая цепочка base (цикл?)", name); return; }
    const auto it = prefabs_.find(name);
    if (it == prefabs_.end()) { devils_engine::utils::warn("prefab '{}' не найден", name); return; }
    if (!it->second.base.empty()) collect(it->second.base, out, depth + 1);
    for (const auto& [comp, ap] : it->second.appliers) out[comp] = &ap; // наследник перекрывает базу
  }

  std::unordered_map<std::string, component_spec> specs_;
  std::unordered_map<std::string, loaded_prefab> prefabs_;
  std::function<void(aes::entityid_t, aes::world&)> construct_;
  std::function<void(aes::entityid_t, aes::world&)> destruct_;
};

}
}

#endif
