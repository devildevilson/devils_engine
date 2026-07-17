#ifndef DEVILS_ENGINE_ACT_PACKER_H
#define DEVILS_ENGINE_ACT_PACKER_H

#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "call_context.h"
#include "common.h"
#include "exec_context.h"
#include "function.h"
#include "value.h"

#include "devils_engine/utils/prng.h" // utils::mix

// packer — АДАПТЕР обычной C++-функции в act::function<Ret>. Цель: писать геймплейные функции БЕЗ
// exec_context — от «обычных аргументов, которые примерно и ожидаешь», а упаковщик сам достаёт их из
// контекста и подставляет (ROADMAP п.16, память entity-interaction-model). Биндинг ПО ТИПУ параметра:
//   - act::entity_handle  → scope[k] (+ world из ctx): k-й по счёту entity_handle аргумент ← k-й scope;
//   - act::rng_source     → детерминированные входы (seed/entity/tick) из ctx (для RNG/тика; least-priv);
//   - const exec_context& → сам ctx (СПЕЦ-СЛУЧАЙ: особому эффекту нужен полный контекст);
//   - прочее (plain)      → call_context.arguments() позиционно (k-й plain аргумент ← k-й named value).
// Индексы источников вычисляются в COMPILE-TIME из позиции параметра, поэтому порядок вычисления
// аргументов вызова роли не играет (никаких running-счётчиков). least-privilege: функция объявляет в
// сигнатуре только то, что ей нужно; остального (rng/sink/scratch/полный scope) она не видит.
//
// АРБИТРАЖ (elect/collect) НЕ здесь: `pack` лишь собирает C++-аргументы из act-контекста.
// Если Fn = catalogue `fn_deferred_ptr`, сама generated-обёртка запишет typed arguments в strategy executor;
// `seal/commit` выполнит вызвавший pipeline. act остаётся ECS- и MT-агностичным.

namespace devils_engine {
namespace act {

// entity_handle — invoke-time склейка мира и сущности. НЕ хранится/не сериализуется (см. common.h).
// world — act::world (opaque); проект реинтерпретирует в конкретный ECS своим аксессором.
struct entity_handle {
  world* w = nullptr;
  entity_id id{};
};

// rng_source — детерминированные входы вызова (как в exec_context::random). Инжектится упаковщиком.
struct rng_source {
  uint64_t seed = 0, entity = 0, tick = 0;
  uint64_t random(const uint64_t purpose) const noexcept {
    return utils::mix(seed, entity, tick, purpose);
  }
};

namespace detail {
template <class F>
struct sig;
template <class R, class... A>
struct sig<R (*)(A...)> {
  using ret = R;
  using args = std::tuple<A...>;
};
template <class R, class... A>
struct sig<R (*)(A...) noexcept> {
  using ret = R;
  using args = std::tuple<A...>;
};

template <class T>
inline constexpr bool is_handle_v = std::is_same_v<std::remove_cvref_t<T>, entity_handle>;
template <class T>
inline constexpr bool is_rng_v = std::is_same_v<std::remove_cvref_t<T>, rng_source>;
template <class T>
inline constexpr bool is_ctx_v = std::is_same_v<std::remove_cvref_t<T>, exec_context>;
template <class T>
inline constexpr bool is_plain_v = !is_handle_v<T> && !is_rng_v<T> && !is_ctx_v<T>;

// число параметров-предикатов Pred среди первых I типов кортежа (compile-time source index).
template <template <class> class Pred, class Tuple, size_t I>
constexpr size_t count_before() {
  size_t n = 0;
  [&]<size_t... K>(std::index_sequence<K...>) {
    ((n += Pred<std::tuple_element_t<K, Tuple>>::value ? 1 : 0), ...);
  }(std::make_index_sequence<I>{});
  return n;
}
template <class T>
struct handle_pred {
  static constexpr bool value = is_handle_v<T>;
};
template <class T>
struct plain_pred {
  static constexpr bool value = is_plain_v<T>;
};

template <class T>
T from_value(const value& v) {
  using P = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<P, bool>) {
    return v.bln;
  } else if constexpr (std::is_same_v<P, entity_id>) {
    return entity_id{static_cast<uint32_t>(v.hnd)};
  } else if constexpr (std::is_enum_v<P>) {
    return static_cast<P>(v.inum);
  } else if constexpr (std::is_integral_v<P>) {
    return static_cast<P>(v.inum);
  } else if constexpr (std::is_floating_point_v<P>) {
    return static_cast<P>(v.num);
  } else {
    static_assert(sizeof(P) == 0, "act::packer: unsupported plain argument type");
  }
}
} // namespace detail

// packed_function — function<Ret> над обычной Fn (см. описание файла). Состояния не держит (Fn — NTTP).
template <auto Fn>
struct packed_function final : function<typename detail::sig<decltype(Fn)>::ret> {
  using ret_t = typename detail::sig<decltype(Fn)>::ret;
  using args_t = typename detail::sig<decltype(Fn)>::args;
  static constexpr size_t arity = std::tuple_size_v<args_t>;

  packed_function() noexcept : function<ret_t>(detail::category_of<ret_t>()) {}

  using function<ret_t>::invoke;
  ret_t invoke(const exec_context& ctx, call_context& call) const override {
    return call_fn(ctx, call, std::make_index_sequence<arity>{});
  }
  void describe(const exec_context&, const describe_callback&) const override {}

private:
  template <size_t I>
  decltype(auto) bind(const exec_context& ctx, call_context& call) const {
    using P = std::tuple_element_t<I, args_t>;
    if constexpr (detail::is_handle_v<P>) {
      constexpr size_t k = detail::count_before<detail::handle_pred, args_t, I>();
      return entity_handle{const_cast<world*>(ctx.w), ctx.scope[k]};
    } else if constexpr (detail::is_rng_v<P>) {
      return rng_source{ctx.rng_seed, ctx.rng_entity, ctx.rng_tick};
    } else if constexpr (detail::is_ctx_v<P>) {
      return (ctx); // спец-случай: полный контекст (ссылка, не копия)
    } else {
      constexpr size_t k = detail::count_before<detail::plain_pred, args_t, I>();
      const auto args = call.arguments();
      return detail::from_value<P>(k < args.size() ? args[k].data : value{});
    }
  }

  template <size_t... I>
  ret_t call_fn(const exec_context& ctx, call_context& call, std::index_sequence<I...>) const {
    if constexpr (std::is_void_v<ret_t>) {
      Fn(bind<I>(ctx, call)...);
      call.result = value{};
    } else {
      ret_t r = Fn(bind<I>(ctx, call)...);
      call.result = detail::to_value(r);
      return r;
    }
  }
};

// pack<&fn>() — фабрика для registry::reg(name, act::pack<&fn>()). Категория выводится из Ret.
template <auto Fn>
std::unique_ptr<function<typename detail::sig<decltype(Fn)>::ret>> pack() {
  return std::make_unique<packed_function<Fn>>();
}

} // namespace act
} // namespace devils_engine

#endif
