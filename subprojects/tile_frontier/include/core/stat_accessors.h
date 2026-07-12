#ifndef TILE_FRONTIER_CORE_STAT_ACCESSORS_H
#define TILE_FRONTIER_CORE_STAT_ACCESSORS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include <reflect>
#include <devils_script/system.h>
#include <devils_engine/utils/type_traits.h>          // template_string_t / template_string_concat
#include <devils_engine/catalogue/introspection.h>    // catalogue::domain / fn_traits

// Generic-компонент ХАРАКТЕРИСТИК (пункт (д) engine-usage-model): плоский POD чисел (int/float), из
// которого статической рефлексией автогенерируются ds-аксессоры — по паре на поле: чтение (имя поля)
// и прибавление ("add_"+имя, эффект, логируется в catalogue). Скрипты тогда пишут `hunger > 0.5`,
// `add_strength(5)` без ручной регистрации. Тот же плоский POD идеально ложится на сериализацию.
//
// Аксессоры регистрируются НАД ЗАДАННЫМ scope-типом (Scope) через getter Scope -> StatsT* — так они
// работают ПРЯМО на скоупе проекта (напр. entity_scope), без промежуточной ds-навигации: getter
// достаёт компонент из мира. null-безопасно (нет компонента ⇒ дефолт при чтении, no-op при add).
// Механизм generic (reflect + ds), кандидат на вынос в движок.

namespace tile_frontier {
namespace core {

// Простой scope над указателем на компонент — удобный Scope по умолчанию (тесты/прямой доступ).
template <typename StatsT>
struct stat_scope {
  uint32_t id = UINT32_MAX;
  StatsT* ptr = nullptr;
  bool valid() const noexcept { return ptr != nullptr; }
};

template <typename StatsT>
StatsT* stat_scope_getter(stat_scope<StatsT> s) noexcept { return s.ptr; }

namespace detail {

template <typename StatsT, std::size_t I>
using stat_field_t = std::remove_cvref_t<decltype(reflect::get<I>(std::declval<StatsT&>()))>;

// чтение поля I (число) — value-функция ds; getter достаёт StatsT из скоупа (null ⇒ дефолт).
template <typename StatsT, typename Scope, auto Getter, std::size_t I>
stat_field_t<StatsT, I> read_stat(Scope s) {
  StatsT* p = Getter(s);
  return p != nullptr ? reflect::get<I>(*p) : stat_field_t<StatsT, I>{};
}

// прибавление к полю I — эффект ds (мутирует компонент; null ⇒ no-op).
template <typename StatsT, typename Scope, auto Getter, std::size_t I>
void add_stat(Scope s, const stat_field_t<StatsT, I> v) {
  if (StatsT* p = Getter(s)) reflect::get<I>(*p) += v;
}

// compile-time имя эффекта "add_<field>" как template-строка (для NTTP catalogue fn_traits).
template <typename StatsT, std::size_t I>
consteval auto add_stat_name() {
  constexpr std::string_view f = reflect::member_name<I, StatsT>();
  devils_engine::utils::template_string_t<f.size() + 1> field{};
  for (std::size_t i = 0; i < f.size(); ++i) field.value[i] = f[i];
  return devils_engine::utils::template_string_concat(devils_engine::utils::template_string_t("add_"), field);
}

} // namespace detail

// Зарегистрировать в ds::system аксессоры полей StatsT над скоупом Scope (getter: Scope -> StatsT*):
// `<field>` (чтение, чистая) и `add_<field>` (прибавление, ЭФФЕКТ — оборачивается через
// catalogue::domain<Domain> для лога реплея). Имена полей — статической рефлексией.
template <typename StatsT, typename Scope, auto Getter, auto Domain>
void register_stat_accessors(devils_script::system& sys) {
  reflect::for_each<StatsT>([&](auto Idx) {
    constexpr std::size_t I = decltype(Idx)::value;
    const std::string name(reflect::member_name<I, StatsT>());

    sys.register_function<&detail::read_stat<StatsT, Scope, Getter, I>>(name);

    // add_<field> через catalogue: fn_ptr — by-value обёртка с точной сигнатурой оригинала (maker::call).
    using traits = typename devils_engine::catalogue::domain<Domain>::template fn_traits<
      &detail::add_stat<StatsT, Scope, Getter, I>, detail::add_stat_name<StatsT, I>(),
      devils_engine::utils::template_string_t("scope"), devils_engine::utils::template_string_t("value")>;
    sys.register_function<traits::fn_ptr>("add_" + name);
  });
}

}
}

#endif
