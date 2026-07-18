#ifndef DEVILS_ENGINE_ACT_BUILDING_BLOCKS_H
#define DEVILS_ENGINE_ACT_BUILDING_BLOCKS_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <devils_script/system.h>

#include "devils_engine/utils/string_id.h" // utils::string_hash
#include "function.h"
#include "interaction.h"
#include "packer.h"
#include "registry.h"

// building_blocks — единая декларативная точка регистрации нативных gameplay building blocks
// (ROADMAP п.14). Главный путь: проект пишет эффект обычной C++-функцией, оборачивает её в
// catalogue MT-паттерн (fn_deferred_ptr) и заносит сюда effect<Traits>() — набор регистрирует
// блок в ds::system, и конфиг-скрипты композируют из него семантические действия. Семантическое
// имя действия попадает в act::registry из КОНФИГА (script_function), а не второй ручной таблицей.
//
// Прямая регистрация в act в обход ds — ИСКЛЮЧИТЕЛЬНЫЙ случай (native/effect_native): например,
// предикат для FSM-гварда или эффект, чей ds-контракт ещё не определён. Такие записи проигрываются
// в act::registry через install() при каждом его пересоздании (init/load), тогда как ds::system
// живёт в assets и регистрируется один раз через register_ds(). Коллизия имени native-записи с
// config-скриптом падает громко в registry::reg — фолбэков нет, конфликт решается удалением
// native-записи (модель «production fallback удалён»).
//
// interaction-дескриптор — свойство СЕМАНТИЧЕСКОГО имени, не бэкенда: регистрируется всегда,
// независимо от того, native или script стоит за именем.

namespace devils_engine {
namespace act {

class building_blocks {
public:
  // effect building block для конфиг-скриптов: Traits = catalogue::mt::domain<...>::fn_traits<&fn, "name", ...>.
  // В ds уходит fn_deferred_ptr — script pass только записывает typed call, commit владеет pipeline.
  template <typename Traits>
  building_blocks& effect() {
    entry e;
    e.name = std::string(Traits::name);
    e.ds_reg = +[](devils_script::system& sys, const std::string& name) {
      sys.register_function<Traits::fn_deferred_ptr>(name);
    };
    entries_.push_back(std::move(e));
    return *this;
  }

  // чистый building block (предикат/число/scope-функция) — только ds; имя даёт вызывающий.
  template <auto Fn>
  building_blocks& pure(std::string name) {
    entry e;
    e.name = std::move(name);
    e.ds_reg = +[](devils_script::system& sys, const std::string& n) {
      sys.register_function<Fn>(n);
    };
    entries_.push_back(std::move(e));
    return *this;
  }

  // ИСКЛЮЧЕНИЕ: act-only эффект в обход ds (deferred-обёртка с не-ds типами аргументов).
  // Уходит из списка, как только блок получает ds-контракт (target scope/RNG).
  template <typename Traits>
  building_blocks& effect_native() {
    entry e;
    e.name = std::string(Traits::name);
    e.act_factory = +[]() -> std::unique_ptr<function_base> {
      return pack<Traits::fn_deferred_ptr>();
    };
    entries_.push_back(std::move(e));
    return *this;
  }

  // ИСКЛЮЧЕНИЕ: act-only функция (напр. предикат FSM-гварда, скриптам не нужен).
  template <auto Fn>
  building_blocks& native(std::string name) {
    entry e;
    e.name = std::move(name);
    e.act_factory = +[]() -> std::unique_ptr<function_base> {
      return pack<Fn>();
    };
    entries_.push_back(std::move(e));
    return *this;
  }

  // арбитраж семантического имени (eat → elect по target scope); бэкенд имени роли не играет.
  building_blocks& reg_interaction(std::string name, const interaction desc) {
    entry e;
    e.name = std::move(name);
    e.desc = desc;
    e.has_interaction = true;
    entries_.push_back(std::move(e));
    return *this;
  }

  // Однократная регистрация ds-части (владелец ds::system зовёт из своего конструктора).
  void register_ds(devils_script::system& sys) const {
    for (const auto& e : entries_) {
      if (e.ds_reg != nullptr) {
        e.ds_reg(sys, e.name);
      }
    }
  }

  // Повторяемая act-часть: native-записи + interaction-дескрипторы на свежем registry.
  // Дубликат имени с уже зарегистрированной функцией (config-скриптом) падает внутри reg().
  void install(registry& reg) const {
    for (const auto& e : entries_) {
      if (e.act_factory != nullptr) {
        reg.reg(e.name, e.act_factory());
      }
      if (e.has_interaction) {
        reg.reg_interaction(utils::string_hash(e.name), e.desc);
      }
    }
  }

private:
  struct entry {
    std::string name;
    void (*ds_reg)(devils_script::system&, const std::string&) = nullptr;
    std::unique_ptr<function_base> (*act_factory)() = nullptr;
    interaction desc{};
    bool has_interaction = false;
  };
  std::vector<entry> entries_;
};

} // namespace act
} // namespace devils_engine

#endif
