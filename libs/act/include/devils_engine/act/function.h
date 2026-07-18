#ifndef DEVILS_ENGINE_ACT_FUNCTION_H
#define DEVILS_ENGINE_ACT_FUNCTION_H

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include <devils_script/container.h> // ds::container (process/describe/max_lists) — invoke зовёт методы
#include <devils_script/context.h>   // ds::context (clear/set_arg/get_return/create_lists/userptr)

#include "call_context.h"
#include "common.h"
#include "devils_engine/utils/core.h"      // utils::error
#include "devils_engine/utils/string_id.h" // utils::id
#include "exec_context.h"                  // + алиас namespace ds = ::devils_script
#include "execution_scratch.h"

struct lua_State;

namespace devils_engine {
namespace act {
namespace ds = ::devils_script;
// Геймплейные функции РАЗДЕЛЕНЫ ПО ТИПУ ВОЗВРАТА (как user_function_type в
// devils_script) — это явный контракт, и он же кодирует чистоту (effect=mutating,
// остальные=pure), поэтому отдельных метаданных purity/signature НЕ нужно.
//
// Чтобы не плодить category×backend классов: КАТЕГОРИЯ = шаблон function<RetT>,
// БЭКЕНД = шаблон-реализация (native/script/lua), инстанцируется на каждый RetT.
//
// Эти функции зовутся НЕ только в детерминированном симе, но и из Lua-UI (напр.
// строковая функция → ключ локализации по условию) — поэтому реестр и контекст общие.

enum class category : uint8_t { effect,
                                predicate,
                                number,
                                string,
                                object };

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
  virtual RetT invoke(const exec_context& ctx, call_context& call) const = 0;

  // Совместимый короткий путь для существующих predicate/effect consumers без аргументов.
  RetT invoke(const exec_context& ctx) const {
    if (ctx.scratch == nullptr) {
      utils::error{}("act::function::invoke: exec_context.scratch is not set (per-worker execution_scratch is required)");
    }
    auto& call = ctx.scratch->call;
    call.clear_schema(); // short path has no caller-owned args/out schema; do not leak it across functions
    if constexpr (std::is_void_v<RetT>) {
      invoke(ctx, call);
    } else {
      return invoke(ctx, call);
    }
  }
};

using effect_function = function<void>;
using predicate_function = function<bool>;
using number_function = function<real_t>;
using string_function = function<utils::id>;
using object_function = function<entity_id>;
// ОТКРЫТО: number_function можно доп. типизировать по СМЫСЛУ возврата (деньги/угол/%/
// дистанция) опц. unit-тегом, чтобы UI форматировал и складывал совместимое.

namespace detail {
template <typename RetT>
constexpr category category_of();
template <>
constexpr category category_of<void>() {
  return category::effect;
}
template <>
constexpr category category_of<bool>() {
  return category::predicate;
}
template <>
constexpr category category_of<real_t>() {
  return category::number;
}
template <>
constexpr category category_of<utils::id>() {
  return category::string;
}
template <>
constexpr category category_of<entity_id>() {
  return category::object;
}

value to_value(bool v) noexcept;
value to_value(real_t v) noexcept;
value to_value(utils::id v) noexcept;
value to_value(entity_id v) noexcept;

bool ds_type(std::string_view type, std::string_view expected) noexcept;

template <typename Set>
bool value_to_ds(const value& v, const std::string_view expected, Set&& set) {
  switch (v.kind) {
    case value_kind::boolean:
      if (ds::type_is_bool(expected)) {
        set(v.bln);
        return true;
      }
      break;
    case value_kind::integer:
      if (expected == utils::type_name<int8_t>()) {
        set(static_cast<int8_t>(v.inum));
        return true;
      }
      if (expected == utils::type_name<int16_t>()) {
        set(static_cast<int16_t>(v.inum));
        return true;
      }
      if (expected == utils::type_name<int32_t>()) {
        set(static_cast<int32_t>(v.inum));
        return true;
      }
      if (expected == utils::type_name<int64_t>()) {
        set(v.inum);
        return true;
      }
      if (expected == utils::type_name<uint8_t>()) {
        set(static_cast<uint8_t>(v.inum));
        return true;
      }
      if (expected == utils::type_name<uint16_t>()) {
        set(static_cast<uint16_t>(v.inum));
        return true;
      }
      if (expected == utils::type_name<uint32_t>()) {
        set(static_cast<uint32_t>(v.inum));
        return true;
      }
      if (expected == utils::type_name<uint64_t>()) {
        set(static_cast<uint64_t>(v.inum));
        return true;
      }
      if (ds::type_is_floating_point(expected)) {
        set(static_cast<double>(v.inum));
        return true;
      }
      break;
    case value_kind::number:
      if (expected == utils::type_name<float>()) {
        set(static_cast<float>(v.num));
        return true;
      }
      if (expected == utils::type_name<double>()) {
        set(static_cast<double>(v.num));
        return true;
      }
      break;
    case value_kind::handle:
      if (expected == utils::type_name<entity_id>()) {
        set(entity_id{static_cast<uint32_t>(v.hnd)});
        return true;
      }
      if (expected == utils::type_name<uint64_t>()) {
        set(v.hnd);
        return true;
      }
      break;
    case value_kind::string:
      // act string — стабильный loc/id hash, не заимствованный string_view.
      if (expected == utils::type_name<utils::id>()) {
        set(v.str);
        return true;
      }
      break;
    // act::vec3 = 24 bytes (double x3), а ds stack slot сейчас 16 bytes: bridge намеренно
    // не делает lossy-конверсию. Для vector нужен отдельный compact/fixed тип при fixed-point pass.
    case value_kind::vector: break;
    case value_kind::none: break;
  }
  return false;
}

bool value_from_ds(std::string_view type, const ds::stack_element& el, value& out);
void bind_call(const ds::script_container& program, const call_context& call, ds::context* vm);
void collect_call(const ds::script_container& program, call_context& call, const ds::context* vm);
} // namespace detail

// ── нативная C++-функция: сырой указатель, без std::function (горячий путь A*) ──
template <typename RetT>
struct native_function final : public function<RetT> {
  using legacy_fn_t = RetT (*)(const exec_context&);
  using fn_t = RetT (*)(const exec_context&, call_context&);
  legacy_fn_t legacy_fn = nullptr;
  fn_t fn = nullptr;
  std::string desc; // опциональное человекочитаемое описание для describe

  explicit native_function(const legacy_fn_t f, std::string d = {}) noexcept
    : function<RetT>(detail::category_of<RetT>()), legacy_fn(f), desc(std::move(d)) {}
  explicit native_function(const fn_t f, std::string d = {}) noexcept
    : function<RetT>(detail::category_of<RetT>()), fn(f), desc(std::move(d)) {}

  using function<RetT>::invoke;
  RetT invoke(const exec_context& ctx, call_context& call) const override {
    if constexpr (std::is_void_v<RetT>) {
      if (fn != nullptr) {
        fn(ctx, call);
      } else {
        legacy_fn(ctx);
      }
      call.result = value{};
    } else {
      const RetT ret = fn != nullptr ? fn(ctx, call) : legacy_fn(ctx);
      call.result = detail::to_value(ret);
      return ret;
    }
  }
  void describe(const exec_context&, const describe_callback& out) const override {
    if (!desc.empty() && out) {
      out(desc);
    }
  }
};

// ── devils_script: скомпилированный container исполняется на ctx.scratch->vm ──
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

  using function<RetT>::invoke;
  RetT invoke(const exec_context& ctx, call_context& call) const override {
    ds::context* vm = ctx.scratch != nullptr ? &ctx.scratch->vm : nullptr;
    if (vm == nullptr) {
      utils::error{}("act::script_function::invoke: exec_context.scratch is not set");
    }
    vm->clear();
    vm->userptr = const_cast<exec_context*>(&ctx); // задел под effect_sink: эффекты читают ctx через userptr
    // Детерминированный RNG скрипта — собственный prng_state ds, ПОСЕЯННЫЙ из immutable act-входов.
    // random/chance-блоки конфиг-скриптов получают детерминизм по (seed, entity, tick) без
    // act::rng_source в сигнатурах; per-callsite state и container::prng_state ds домешивает сам.
    vm->prng_state = utils::mix(ctx.rng_seed, ctx.rng_entity, ctx.rng_tick);
    if (program->max_lists != 0) {
      vm->create_lists(program);
    }
    if (seed != nullptr) {
      seed(ctx, vm); // set_arg(0, root_scope) + опц. именованные аргументы
    }
    detail::bind_call(*program, call, vm);
    program->process(vm);
    detail::collect_call(*program, call, vm);
    if constexpr (std::is_void_v<RetT>) {
      call.result = value{};
      return; // effect: process() уже применил эффект
    } else if constexpr (std::is_same_v<RetT, bool>) {
      const bool ret = vm->is_return<bool>() ? vm->get_return<bool>() : false;
      call.result = value::of(ret);
      return ret;
    } else if constexpr (std::is_same_v<RetT, real_t>) {
      const real_t ret = static_cast<real_t>(vm->get_return<double>());
      call.result = value::of(ret);
      return ret;
    } else {
      // string (utils::id) и object (entity_id): маршалинг возврата ещё не определён — см. план (Risk 1).
      utils::error{}("act::script_function::invoke: marshalling for this return category is not implemented yet");
      return RetT{};
    }
  }

  void describe(const exec_context& ctx, const describe_callback& out) const override {
    ds::context* vm = ctx.scratch != nullptr ? &ctx.scratch->vm : nullptr;
    if (vm == nullptr) {
      return;
    }
    vm->clear();
    vm->prng_state = utils::mix(ctx.rng_seed, ctx.rng_entity, ctx.rng_tick); // как в invoke: describe видит те же random-ветки
    if (seed != nullptr) {
      seed(ctx, vm);
    }
    program->describe(vm, [&out](const ds::container::description_entry& e) {
      if (out) {
        out(e.name); // минимум: стримим имена узлов; payload уточним при созревании describe
      }
    });
  }
};

// ── lua: ГОСТЬ — глобальное/UI, НЕ эффекты сима ── ЗАГЛУШКА ──
template <typename RetT>
struct lua_function final : public function<RetT> {
  lua_State* L = nullptr;
  int ref = 0;
  lua_function(lua_State* l, const int r) noexcept
    : function<RetT>(detail::category_of<RetT>()), L(l), ref(r) {}
  using function<RetT>::invoke;
  RetT invoke(const exec_context&, call_context&) const override {
    utils::error{}("act::lua_function::invoke is not implemented (the Lua backend is not connected yet)");
    if constexpr (!std::is_void_v<RetT>) {
      return RetT{};
    }
  }
  void describe(const exec_context&, const describe_callback&) const override {
    utils::error{}("act::lua_function::describe is not implemented (the Lua backend is not connected yet)");
  }
};

} // namespace act
} // namespace devils_engine

#endif
