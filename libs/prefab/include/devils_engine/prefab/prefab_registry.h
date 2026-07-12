#ifndef DEVILS_ENGINE_PREFAB_PREFAB_REGISTRY_H
#define DEVILS_ENGINE_PREFAB_PREFAB_REGISTRY_H

#include <algorithm>
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

// prefab_registry — движковый механизм «рецепта сборки энтити из компонентов» (шаг 6). САМ механизм в
// devils_engine; КОНКРЕТНЫЕ типы компонентов регистрирует внешний проект. Три формы задания в конфиге:
//   data<C>      — POD, tavl-deserialize по полям:      stats = { hunger = 0.2 }
//   list<C,Item> — контейнер элементов одного типа:      flags = [ hostile, grabbable ]
//   callback<C>  — имя act-функции (инлайн ds — позже):  on_pickup = pickup_gold
// + on_construct-хук достраивает DERIVED-компоненты (perception/transform — не с диска).
// Наследование (`base = ...`) FIELD-LEVEL для data (слоёный deserialize base→наследник: наследник
// перекрывает только заданные поля), whole-value для list/callback (наследник заменяет целиком).

namespace devils_engine {
namespace prefab {

namespace aes = aesthetics;

enum class component_flag : uint32_t { none = 0, required = 1 };
inline component_flag operator|(component_flag a, component_flag b) noexcept {
  return component_flag(uint32_t(a) | uint32_t(b));
}
inline bool has_flag(component_flag v, component_flag f) noexcept {
  return (uint32_t(v) & uint32_t(f)) != 0;
}

// Контекст загрузки: резолв имён (callback → act-функция). Ссылки на ресурсы — позже.
struct prefab_load_context {
  const act::registry* functions = nullptr;
};

namespace detail {
// Обёртка «одно поле value» — чтобы список парсился как нормальная строка `value = [...]` (tavl на
// верхнем уровне ждёт `поле = значение`, а не голый блок/массив).
template <typename T> struct value_field { T value; };
}

class prefab_registry {
public:
  using applier = std::function<void(aes::entityid_t, aes::world&)>;
  // build: собрать applier из ЦЕПОЧКИ сырых текстов значения компонента (база→наследник).
  using builder = std::function<applier(const std::vector<std::string_view>&, const prefab_load_context&)>;

  // data<C>: значение = tavl-блок. Наследование field-level: слоями deserialize в один C
  // (tavl заполняет только присутствующие поля ⇒ наследник перекрывает заданные, остальное от базы).
  template <typename C>
  void data(std::string name, const component_flag flags = component_flag::none) {
    add_spec(std::move(name), flags, [](const std::vector<std::string_view>& chain, const prefab_load_context&) -> applier {
      C c{};
      for (auto text : chain) {
        // снимаем внешние { } → верхнеуровневые поля `hp = 10, atk = 3` (паттерн deserialize_all).
        if (text.size() >= 2 && text.front() == '{' && text.back() == '}') text = text.substr(1, text.size() - 2);
        tavl::parser sp; sp.add_default_operator(); sp.flush(text); sp.finish();
        tavl::ct_context ctx; tavl::deserialize(sp, ctx, c); // слой поверх — перекрывает заданные поля
      }
      return [c](const aes::entityid_t id, aes::world& w) { w.create<C>(id, c); };
    });
  }

  // list<C,Item>: значение = tavl-массив; наследник заменяет список целиком (берём последний в цепочке).
  template <typename C, typename Item>
  void list(std::string name, const component_flag flags = component_flag::none) {
    add_spec(std::move(name), flags, [](const std::vector<std::string_view>& chain, const prefab_load_context&) -> applier {
      detail::value_field<std::vector<Item>> wrap{};
      const std::string doc = "value = " + std::string(chain.back());
      tavl::parser sp; sp.add_default_operator(); sp.flush(doc); sp.finish();
      tavl::ct_context ctx; tavl::deserialize(sp, ctx, wrap);
      auto items = std::move(wrap.value);
      return [items](const aes::entityid_t id, aes::world& w) { w.create<C>(id, C{items}); };
    });
  }

  // callback<C>: значение = имя act-функции; C держит хеш имени (C{ utils::id }). Наследник — целиком.
  template <typename C>
  void callback(std::string name, const component_flag flags = component_flag::none) {
    add_spec(std::move(name), flags, [](const std::vector<std::string_view>& chain, const prefab_load_context& lc) -> applier {
      const std::string_view fn_name = chain.back();
      const auto fid = utils::string_hash(fn_name);
      if (lc.functions != nullptr && !lc.functions->has(fid)) {
        utils::warn("prefab callback '{}': функция не найдена в реестре", fn_name);
      }
      return [fid](const aes::entityid_t id, aes::world& w) { w.create<C>(id, C{ fid }); };
    });
  }

  void on_construct(std::function<void(aes::entityid_t, aes::world&)> fn) { construct_ = std::move(fn); }
  void on_destruct(std::function<void(aes::entityid_t, aes::world&)> fn) { destruct_ = std::move(fn); }

  // Разобрать текст префаба (один документ). Драйвит tavl-парсер построчно: `base` или компонент;
  // сырое ЗНАЧЕНИЕ компонента копируется (для field-level слоёв на finalize). Само построение —
  // ленивое на первом spawn (база может грузиться позже).
  bool add_prefab(std::string name, std::string_view text, const prefab_load_context& lc) {
    using ev_t = tavl::event_type;
    tavl::parser p;
    p.add_default_operator();
    p.flush(text);
    p.finish();

    loaded_prefab lp;
    lp.ctx = lc;
    for (;;) {
      const auto e = p.peek();
      if (e.type == ev_t::eof || e.type == ev_t::not_enought_data) { p.poll_event(); break; }
      if (e.type != ev_t::got_token && e.type != ev_t::got_row_identifier) { p.poll_event(); continue; }

      const auto [kev, kerr] = p.poll_event();
      const std::string key = p.to_string(kev.token);
      p.poll_event(); // '=' (op)
      const std::string_view raw = capture_value(p, text);
      if (key == "base") lp.base = std::string(raw);
      else if (specs_.contains(key)) lp.raw.emplace(key, std::string(raw));
      else utils::warn("prefab '{}': неизвестный компонент '{}', пропущен", name, key);
    }
    prefabs_.insert_or_assign(std::move(name), std::move(lp));
    return true;
  }

  aes::entityid_t spawn(const std::string& name, aes::world& w) {
    const auto id = w.gen_entityid();
    if (auto* lp = build(name)) {
      for (const auto& ap : lp->appliers) ap(id, w);
    }
    if (construct_) construct_(id, w);
    return id;
  }

  void despawn(const aes::entityid_t id, aes::world& w) const {
    if (destruct_) destruct_(id, w);
  }

  bool has_prefab(const std::string& name) const { return prefabs_.contains(name); }

private:
  struct component_spec { component_flag flags; builder build_fn; };
  struct loaded_prefab {
    std::string base;
    std::unordered_map<std::string, std::string> raw; // component -> сырой текст значения
    prefab_load_context ctx{};
    std::vector<applier> appliers;
    bool built = false;
  };

  void add_spec(std::string name, const component_flag flags, builder b) {
    specs_.insert_or_assign(std::move(name), component_spec{ flags, std::move(b) });
  }

  // Сырое значение (блок/массив/токен) с текущей позиции: возвращаем срез исходника.
  std::string_view capture_value(tavl::parser& p, std::string_view text) {
    using ev_t = tavl::event_type;
    const auto is_open  = [](ev_t t) { return t == ev_t::object_begin || t == ev_t::array_begin || t == ev_t::tuple_begin; };
    const auto is_close = [](ev_t t) { return t == ev_t::object_end   || t == ev_t::array_end   || t == ev_t::tuple_end; };

    const auto e = p.peek();
    if (is_open(e.type)) {
      const auto [oev, oerr] = p.poll_event();
      const std::size_t start = oev.token.span.offset;
      int nest = 1;
      tavl::token last = oev.token;
      while (nest > 0) {
        const auto [ce, cerr] = p.poll_event();
        if (ce.type == ev_t::eof || ce.type == ev_t::not_enought_data) break;
        if (is_open(ce.type)) ++nest;
        else if (is_close(ce.type)) { --nest; last = ce.token; }
      }
      const std::size_t stop = last.span.offset + last.span.size;
      return text.substr(start, stop - start);
    }
    const auto [tev, terr] = p.poll_event();
    return text.substr(tev.token.span.offset, tev.token.span.size);
  }

  // Лениво построить appliers префаба: собрать цепочку base→self, по каждому компоненту дать builder'у
  // цепочку сырых текстов (база→наследник) в порядке появления.
  loaded_prefab* build(const std::string& name) {
    const auto it = prefabs_.find(name);
    if (it == prefabs_.end()) { utils::warn("prefab '{}' не найден", name); return nullptr; }
    loaded_prefab& lp = it->second;
    if (lp.built) return &lp;

    std::vector<const loaded_prefab*> chain; // от корня-базы к наследнику
    collect_chain(name, chain, 0);

    std::vector<std::string> order;                                    // компоненты в порядке появления
    for (const auto* pf : chain)
      for (const auto& [comp, _] : pf->raw)
        if (std::find(order.begin(), order.end(), comp) == order.end()) order.push_back(comp);

    for (const auto& comp : order) {
      std::vector<std::string_view> texts; // база→наследник, только где задан
      for (const auto* pf : chain)
        if (const auto ri = pf->raw.find(comp); ri != pf->raw.end()) texts.push_back(ri->second);
      const auto si = specs_.find(comp);
      if (si == specs_.end() || texts.empty()) continue;
      lp.appliers.push_back(si->second.build_fn(texts, lp.ctx));
    }
    lp.built = true;
    return &lp;
  }

  void collect_chain(const std::string& name, std::vector<const loaded_prefab*>& out, int depth) {
    if (depth > 32) { utils::warn("prefab '{}': слишком глубокая цепочка base (цикл?)", name); return; }
    const auto it = prefabs_.find(name);
    if (it == prefabs_.end()) { utils::warn("prefab base '{}' не найден", name); return; }
    if (!it->second.base.empty()) collect_chain(it->second.base, out, depth + 1);
    out.push_back(&it->second);
  }

  std::unordered_map<std::string, component_spec> specs_;
  std::unordered_map<std::string, loaded_prefab> prefabs_;
  std::function<void(aes::entityid_t, aes::world&)> construct_;
  std::function<void(aes::entityid_t, aes::world&)> destruct_;
};

}
}

#endif
