#ifndef DEVILS_ENGINE_ACT_STAT_ACCESSORS_H
#define DEVILS_ENGINE_ACT_STAT_ACCESSORS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <reflect>
#include <devils_script/system.h>

#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/utils/type_traits.h>

namespace devils_engine {
namespace act {

namespace stats_detail {

template <typename T>
consteval bool valid_aggregate() {
  if constexpr (!std::is_aggregate_v<T>) return false;
  else {
    bool valid = true;
    reflect::for_each<T>([&](auto Idx) {
      using field_t = std::remove_cvref_t<decltype(reflect::get<decltype(Idx)::value>(std::declval<T&>()))>;
      valid = valid && ((std::is_integral_v<field_t> && !std::is_same_v<field_t, bool>) ||
                        std::is_floating_point_v<field_t>);
    });
    return valid;
  }
}

template <typename StatsT, std::size_t I>
using field_t = std::remove_cvref_t<decltype(reflect::get<I>(std::declval<StatsT&>()))>;

template <typename StatsT, typename Scope, auto Getter, std::size_t I>
field_t<StatsT, I> read(Scope s) {
  StatsT* p = Getter(s);
  return p != nullptr ? reflect::get<I>(*p) : field_t<StatsT, I>{};
}

template <typename StatsT, typename Scope, auto Getter, std::size_t I>
void add(Scope s, const field_t<StatsT, I> v) {
  if (StatsT* p = Getter(s)) reflect::get<I>(*p) += v;
}

template <typename StatsT, std::size_t I>
consteval auto add_name() {
  constexpr std::string_view f = reflect::member_name<I, StatsT>();
  utils::template_string_t<f.size() + 1> field{};
  for (std::size_t i = 0; i < f.size(); ++i) field.value[i] = f[i];
  return utils::template_string_concat(utils::template_string_t("add_"), field);
}

}

template <typename T>
concept numeric_stats_aggregate = stats_detail::valid_aggregate<T>();

// Удобный direct scope; реальные проекты обычно передают entity_scope и component getter.
template <numeric_stats_aggregate StatsT>
struct stat_scope {
  uint32_t id = UINT32_MAX;
  StatsT* ptr = nullptr;
  bool valid() const noexcept { return ptr != nullptr; }
};

template <numeric_stats_aggregate StatsT>
StatsT* stat_scope_getter(const stat_scope<StatsT> s) noexcept { return s.ptr; }

namespace stats_detail {
template <numeric_stats_aggregate StatsT, typename ParentScope, auto Getter>
stat_scope<StatsT> enter(ParentScope s) noexcept {
  uint32_t id = UINT32_MAX;
  if constexpr (requires { s.id; }) id = static_cast<uint32_t>(s.id);
  return stat_scope<StatsT>{id, Getter(s)};
}
}

template <numeric_stats_aggregate StatsT, typename... Args>
constexpr StatsT make_stats(Args&&... args) noexcept(noexcept(StatsT{std::forward<Args>(args)...})) {
  return StatsT{std::forward<Args>(args)...};
}

// Выбирает C++ default member initializers агрегата (`int32_t health = 100;`). Это всё ещё
// aggregate initialization, пользовательский конструктор не нужен.
template <numeric_stats_aggregate StatsT>
constexpr StatsT initialize_stats() noexcept(noexcept(StatsT{})) {
  return StatsT{};
}

// Инициализатор вызывается как init(index_constant, field_name) и возвращает число,
// приводимое к конкретному типу поля. Этот overload намеренно задаёт КАЖДОЕ поле заново;
// no-arg overload выше выбирает C++ defaults.
template <numeric_stats_aggregate StatsT, typename Initializer>
constexpr StatsT initialize_stats(Initializer&& init) {
  StatsT out{};
  reflect::for_each<StatsT>([&](auto Idx) {
    constexpr size_t I = decltype(Idx)::value;
    using field_t = stats_detail::field_t<StatsT, I>;
    reflect::get<I>(out) = static_cast<field_t>(
      std::invoke(init, Idx, reflect::member_name<I, StatsT>()));
  });
  return out;
}

// Регистрирует локальные функции типа stats-scope. Одинаковые имена полей разных агрегатов
// корректно перегружаются devils_script по типу текущего scope.
template <numeric_stats_aggregate StatsT, auto Domain>
void register_stat_accessors(devils_script::system& sys) {
  reflect::for_each<StatsT>([&](auto Idx) {
    constexpr std::size_t I = decltype(Idx)::value;
    const std::string field(reflect::member_name<I, StatsT>());
    sys.register_function<&stats_detail::read<StatsT, stat_scope<StatsT>, &stat_scope_getter<StatsT>, I>>(field);

    using traits = typename catalogue::domain<Domain>::template fn_traits<
      &stats_detail::add<StatsT, stat_scope<StatsT>, &stat_scope_getter<StatsT>, I>, stats_detail::add_name<StatsT, I>(),
      utils::template_string_t("scope"), utils::template_string_t("value")>;
    sys.register_function<traits::fn_ptr>("add_" + field);
  });
}

// Полная регистрация одного проектного stats-компонента: ParentScope --scope_name--> stat_scope<T>,
// затем field/add_field уже живут внутри возвращённого типизированного scope.
template <numeric_stats_aggregate StatsT, typename ParentScope, auto Getter, auto Domain>
void register_stats(devils_script::system& sys, const std::string_view scope_name) {
  sys.register_function<&stats_detail::enter<StatsT, ParentScope, Getter>>(std::string(scope_name));
  register_stat_accessors<StatsT, Domain>(sys);
}

}
}

#endif
