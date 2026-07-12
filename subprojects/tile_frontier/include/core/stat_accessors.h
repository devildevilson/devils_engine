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
// и прибавление ("add_"+имя). Скрипты тогда пишут `strength + luck`, `add_strength(5)` без ручной
// регистрации предикатов. Тот же плоский POD идеально ложится на сериализацию (структуру нужно лишь
// верифицировать — ничего лишнего). Метаинформация (что считать положительным, для UI) — отдельно.
//
// Механизм полностью generic (reflect + devils_script, без ECS): scope = {entity, StatsT*}; НАВИГАЦИЯ
// entity->StatsT (получение компонента из мира) — проектная и регистрируется отдельно. Кандидат на
// вынос в движок после стабилизации.

namespace tile_frontier {
namespace core {

// Скоуп характеристик для devils_script: сущность + указатель на её компонент статов. Value-скоуп
// (≤16 байт, trivially destructible, с valid()) — как handle<person> в примерах ds.
template <typename StatsT>
struct stat_scope {
  uint32_t id = UINT32_MAX;
  StatsT* ptr = nullptr;
  bool valid() const noexcept { return ptr != nullptr; }
};

namespace detail {

template <typename StatsT, std::size_t I>
using stat_field_t = std::remove_cvref_t<decltype(reflect::get<I>(std::declval<StatsT&>()))>;

// чтение поля I (число) — value-функция ds
template <typename StatsT, std::size_t I>
stat_field_t<StatsT, I> read_stat(stat_scope<StatsT> s) {
  return reflect::get<I>(*s.ptr);
}

// прибавление к полю I — эффект ds (мутирует компонент через scope.ptr)
template <typename StatsT, std::size_t I>
void add_stat(stat_scope<StatsT> s, const stat_field_t<StatsT, I> v) {
  reflect::get<I>(*s.ptr) += v;
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

// Зарегистрировать в ds::system по паре функций на каждое поле StatsT: `<field>` (чтение, чистая),
// `add_<field>` (прибавление, ЭФФЕКТ). Эффект оборачивается через catalogue::domain<Domain> — его
// вызовы попадают в лог интроспекции/реплея (когда introspection настроен; иначе просто инвок).
// Имена полей и имя эффекта выводятся статической рефлексией + compile-time конкатом.
template <typename StatsT, auto Domain>
void register_stat_accessors(devils_script::system& sys) {
  reflect::for_each<StatsT>([&](auto Idx) {
    constexpr std::size_t I = decltype(Idx)::value;
    const std::string name(reflect::member_name<I, StatsT>());

    sys.register_function<&detail::read_stat<StatsT, I>>(name);

    // add_<field>: fn_traits несёт домен + имя + имена аргументов; его `fn_ptr` = by-value обёртка с
    // ТОЧНОЙ сигнатурой оригинала (maker::call), роутящая вызов через catalogue (лог реплея, когда
    // introspection настроен; иначе просто инвок). ds видит чистую сигнатуру.
    using traits = typename devils_engine::catalogue::domain<Domain>::template fn_traits<
      &detail::add_stat<StatsT, I>, detail::add_stat_name<StatsT, I>(),
      devils_engine::utils::template_string_t("scope"), devils_engine::utils::template_string_t("value")>;
    sys.register_function<traits::fn_ptr>("add_" + name);
  });
}

}
}

#endif
