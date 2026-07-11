#ifndef DEVILS_ENGINE_ACT_FUNCTION_H
#define DEVILS_ENGINE_ACT_FUNCTION_H

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include "devils_engine/utils/core.h"      // utils::error
#include "devils_engine/utils/string_id.h" // utils::id
#include "common.h"
#include "exec_context.h"       // + алиас namespace ds = ::devils_script

#include <devils_script/container.h> // ds::container (process/describe/max_lists) — invoke зовёт методы
#include <devils_script/context.h>   // ds::context (clear/set_arg/get_return/create_lists/userptr)

struct lua_State;

namespace devils_engine {
namespace act {
// Геймплейные функции РАЗДЕЛЕНЫ ПО ТИПУ ВОЗВРАТА (как user_function_type в
// devils_script) — это явный контракт, и он же кодирует чистоту (effect=mutating,
// остальные=pure), поэтому отдельных метаданных purity/signature НЕ нужно.
//
// Чтобы не плодить category×backend классов: КАТЕГОРИЯ = шаблон function<RetT>,
// БЭКЕНД = шаблон-реализация (native/script/lua), инстанцируется на каждый RetT.
//
// Эти функции зовутся НЕ только в детерминированном симе, но и из Lua-UI (напр.
// строковая функция → ключ локализации по условию) — поэтому реестр и контекст общие.

enum class category : uint8_t { effect, predicate, number, string, object };

// describe — пробежать функцию без применения эффектов и ОТДАВАТЬ данные в коллбек (для
// UI: тултип "почему нельзя"/"+5 от X, −2 от Y", превью эффекта, разбор предиката/числа).
// devils_script умеет интроспекцию/описание скомпилированных контейнеров — он будет
// стримить узлы разбора в этот коллбек, по одному за вызов. Тип коллбека ВРЕМЕННЫЙ
// (любой), уточним когда подключим script-бэкенд и определимся с payload.
using describe_callback = std::function<void(std::string_view)>;

// общая база — для generic-хранения в реестре + describe. invoke типизирован в наследниках.
struct function_base {
  category cat;
  explicit function_base(const category c) noexcept : cat(c) {}
  virtual ~function_base() noexcept = default;
  virtual void describe(const exec_context& ctx, const describe_callback& out) const = 0;
};

// category ⇒ тип возврата: void=effect, bool=predicate, real_t=number,
// utils::id=string (ключ локализации хешем), entity_id=object.
template <typename RetT>
struct function : public function_base {
  using function_base::function_base;
  virtual RetT invoke(const exec_context& ctx) const = 0;
};

using effect_function    = function<void>;
using predicate_function = function<bool>;
using number_function    = function<real_t>;
using string_function    = function<utils::id>;
using object_function    = function<entity_id>;
// ОТКРЫТО: number_function можно доп. типизировать по СМЫСЛУ возврата (деньги/угол/%/
// дистанция) опц. unit-тегом, чтобы UI форматировал и складывал совместимое.

namespace detail {
template <typename RetT> constexpr category category_of();
template <> constexpr category category_of<void>()      { return category::effect; }
template <> constexpr category category_of<bool>()      { return category::predicate; }
template <> constexpr category category_of<real_t>()    { return category::number; }
template <> constexpr category category_of<utils::id>() { return category::string; }
template <> constexpr category category_of<entity_id>() { return category::object; }
}

// ── нативная C++-функция: сырой указатель, без std::function (горячий путь A*) ──
template <typename RetT>
struct native_function final : public function<RetT> {
  using fn_t = RetT (*)(const exec_context&);
  fn_t fn = nullptr;
  std::string desc; // опциональное человекочитаемое описание для describe

  explicit native_function(const fn_t f, std::string d = {}) noexcept
    : function<RetT>(detail::category_of<RetT>()), fn(f), desc(std::move(d)) {}

  RetT invoke(const exec_context& ctx) const override { return fn(ctx); }
  void describe(const exec_context&, const describe_callback& out) const override {
    if (!desc.empty() && out) out(desc);
  }
};

// ── devils_script: скомпилированный container исполняется на ctx.vm (пер-воркер скретчпад) ──
// seed — ПРОЕКТНЫЙ засев root-скоупа (и опц. именованных аргументов) из exec_context в ds::context.
// act остаётся ECS-agnostic: как из exec_context получить scope-значение, знает только проект
// (reinterpret-seam над ctx.w + primary()). nullptr ⇒ scope-less скрипт. Реестр однороден —
// function<RetT>*, категория бэкенда роли не играет для acumen/mood.
template <typename RetT>
struct script_function final : public function<RetT> {
  using seed_fn = void (*)(const exec_context&, ds::context*);
  const ds::container* program = nullptr;
  seed_fn seed = nullptr;

  explicit script_function(const ds::container* p, const seed_fn s = nullptr) noexcept
    : function<RetT>(detail::category_of<RetT>()), program(p), seed(s) {}

  RetT invoke(const exec_context& ctx) const override {
    ds::context* vm = ctx.vm;
    if (vm == nullptr) utils::error{}("act::script_function::invoke: exec_context.vm не задан (нужен пер-воркер ds::context)");
    vm->clear();
    vm->userptr = const_cast<exec_context*>(&ctx); // задел под effect_sink: эффекты читают ctx через userptr
    if (program->max_lists != 0) vm->create_lists(program);
    if (seed != nullptr) seed(ctx, vm);            // set_arg(0, root_scope) + опц. именованные аргументы
    program->process(vm);
    if constexpr (std::is_void_v<RetT>) {
      return; // effect: process() уже применил эффект
    } else if constexpr (std::is_same_v<RetT, bool>) {
      return vm->is_return<bool>() ? vm->get_return<bool>() : false;
    } else if constexpr (std::is_same_v<RetT, real_t>) {
      return static_cast<real_t>(vm->get_return<double>());
    } else {
      // string (utils::id) и object (entity_id): маршалинг возврата ещё не определён — см. план (Risk 1).
      utils::error{}("act::script_function::invoke: маршалинг этой категории возврата ещё не реализован");
      return RetT{};
    }
  }

  void describe(const exec_context& ctx, const describe_callback& out) const override {
    ds::context* vm = ctx.vm;
    if (vm == nullptr) return;
    vm->clear();
    if (seed != nullptr) seed(ctx, vm);
    program->describe(vm, [&out](const ds::container::description_entry& e) {
      if (out) out(e.name); // минимум: стримим имена узлов; payload уточним при созревании describe
    });
  }
};

// ── lua: ГОСТЬ — глобальное/UI, НЕ эффекты сима ── ЗАГЛУШКА ──
template <typename RetT>
struct lua_function final : public function<RetT> {
  lua_State* L = nullptr; int ref = 0;
  lua_function(lua_State* l, const int r) noexcept
    : function<RetT>(detail::category_of<RetT>()), L(l), ref(r) {}
  RetT invoke(const exec_context&) const override {
    utils::error{}("act::lua_function::invoke не реализован (бэкенд lua ещё не подключён)");
    if constexpr (!std::is_void_v<RetT>) return RetT{};
  }
  void describe(const exec_context&, const describe_callback&) const override {
    utils::error{}("act::lua_function::describe не реализован (бэкенд lua ещё не подключён)");
  }
};

}
}

#endif
