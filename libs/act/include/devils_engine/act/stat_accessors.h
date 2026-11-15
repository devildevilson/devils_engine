#ifndef DEVILS_ENGINE_ACT_STAT_ACCESSORS_H
#define DEVILS_ENGINE_ACT_STAT_ACCESSORS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <reflect>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <devils_engine/catalogue/introspection.h>
#include <devils_engine/utils/type_traits.h>
#include <devils_script/system.h>

namespace devils_engine {
namespace act {

// Поля перебираются через std::index_sequence и шаблоны функций, а не reflect::for_each с generic
// lambda: MSVC (VS 2026) падал с internal compiler error C1001 в каждом TU с этим заголовком, а индекс
// из лямбды (`constexpr I = decltype(Idx)::value`) уходил здесь в аргументы шаблонов. Порядок
// регистрации тот же: comma-fold идёт по индексам слева направо, как for_each.
namespace stats_detail {

template <typename StatsT, std::size_t I>
using field_t = std::remove_cvref_t<decltype(reflect::get<I>(std::declval<StatsT&>()))>;

template <typename T>
inline constexpr bool numeric_field_v =
  (std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_floating_point_v<T>;

template <typename T, std::size_t... I>
consteval bool all_fields_numeric(std::index_sequence<I...>) {
  return (numeric_field_v<field_t<T, I>> && ...);
}

template <typename T>
consteval bool valid_aggregate() {
  if constexpr (!std::is_aggregate_v<T>) {
    return false;
  } else {
    return all_fields_numeric<T>(std::make_index_sequence<reflect::size<T>()>{});
  }
}

template <typename T>
using field_indices = std::make_index_sequence<reflect::size<T>()>;

template <typename StatsT, typename Scope, auto Getter, std::size_t I>
field_t<StatsT, I> read(Scope s) {
  StatsT* p = Getter(s);
  return p != nullptr ? reflect::get<I>(*p) : field_t<StatsT, I>{};
}

template <typename StatsT, typename Scope, auto Getter, std::size_t I>
void add(Scope s, const field_t<StatsT, I> v) {
  if (StatsT* p = Getter(s)) {
    reflect::get<I>(*p) += v;
  }
}

template <typename StatsT, std::size_t I>
consteval auto add_name() {
  constexpr std::string_view f = reflect::member_name<I, StatsT>();
  utils::template_string_t<f.size() + 1> field{};
  for (std::size_t i = 0; i < f.size(); ++i) {
    field.value[i] = f[i];
  }
  return utils::template_string_concat(utils::template_string_t("add_"), field);
}

template <typename StatsT, std::size_t I>
inline constexpr auto add_name_v = add_name<StatsT, I>();

template <typename StatsT, typename Initializer, std::size_t... I>
constexpr void initialize_fields(StatsT& out, Initializer& init, std::index_sequence<I...>) {
  ((reflect::get<I>(out) = static_cast<field_t<StatsT, I>>(
      std::invoke(init, std::integral_constant<std::size_t, I>{}, reflect::member_name<I, StatsT>()))),
   ...);
}

} // namespace stats_detail

template <typename T>
concept numeric_stats_aggregate = stats_detail::valid_aggregate<T>();

// Удобный direct scope; реальные проекты обычно передают entity_scope и component getter.
template <numeric_stats_aggregate StatsT>
struct stat_scope {
  uint32_t id = UINT32_MAX;
  StatsT* ptr = nullptr;
  bool valid() const noexcept {
    return ptr != nullptr;
  }
};

template <numeric_stats_aggregate StatsT>
StatsT* stat_scope_getter(const stat_scope<StatsT> s) noexcept {
  return s.ptr;
}

namespace stats_detail {
template <numeric_stats_aggregate StatsT, typename ParentScope, auto Getter>
stat_scope<StatsT> enter(ParentScope s) noexcept {
  uint32_t id = UINT32_MAX;
  if constexpr (requires { s.id; }) {
    id = static_cast<uint32_t>(s.id);
  }
  return stat_scope<StatsT>{id, Getter(s)};
}

template <typename StatsT, std::size_t I>
void register_reader(devils_script::system& sys) {
  sys.register_function<&read<StatsT, stat_scope<StatsT>, &stat_scope_getter<StatsT>, I>>(
    std::string(reflect::member_name<I, StatsT>()));
}

template <typename StatsT, std::size_t... I>
void register_readers(devils_script::system& sys, std::index_sequence<I...>) {
  (register_reader<StatsT, I>(sys), ...);
}

template <typename StatsT, auto Domain, std::size_t I>
void register_writer(devils_script::system& sys) {
  using traits = typename catalogue::domain<Domain>::template fn_traits<
    &add<StatsT, stat_scope<StatsT>, &stat_scope_getter<StatsT>, I>, add_name_v<StatsT, I>,
    utils::template_string_t("scope"), utils::template_string_t("value")>;
  sys.register_function<traits::fn_ptr>("add_" + std::string(reflect::member_name<I, StatsT>()));
}

template <typename StatsT, auto Domain, std::size_t... I>
void register_writers(devils_script::system& sys, std::index_sequence<I...>) {
  (register_writer<StatsT, Domain, I>(sys), ...);
}
} // namespace stats_detail

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
  stats_detail::initialize_fields(out, init, stats_detail::field_indices<StatsT>{});
  return out;
}

// Регистрирует только локальные readers типа stats-scope. Это отдельная точка для pipeline-ов,
// где прямое изменение компонента обошло бы authoritative outcome/commit слой.
template <numeric_stats_aggregate StatsT>
void register_stat_readers(devils_script::system& sys) {
  stats_detail::register_readers<StatsT>(sys, stats_detail::field_indices<StatsT>{});
}

// Регистрирует только локальные add_<field> writers. Публичный register_stats ниже сохраняет
// прежнее поведение readers+writes; разделение нужно не для его замены, а для явного opt-out.
template <numeric_stats_aggregate StatsT, auto Domain>
void register_stat_writers(devils_script::system& sys) {
  stats_detail::register_writers<StatsT, Domain>(sys, stats_detail::field_indices<StatsT>{});
}

// Регистрирует локальные readers и writers типа stats-scope. Одинаковые имена полей разных
// агрегатов корректно перегружаются devils_script по типу текущего scope.
template <numeric_stats_aggregate StatsT, auto Domain>
void register_stat_accessors(devils_script::system& sys) {
  register_stat_readers<StatsT>(sys);
  register_stat_writers<StatsT, Domain>(sys);
}

// Полная read-only регистрация stats scope: scope getter + reflected field readers, без add_*.
// Соседний register_stats намеренно остаётся mutable и сохраняет прежний API/семантику.
template <numeric_stats_aggregate StatsT, typename ParentScope, auto Getter>
void register_stats_readonly(devils_script::system& sys,
                             const std::string_view scope_name) {
  sys.register_function<&stats_detail::enter<StatsT, ParentScope, Getter>>(
    std::string(scope_name));
  register_stat_readers<StatsT>(sys);
}

// Полная регистрация одного проектного stats-компонента: ParentScope --scope_name--> stat_scope<T>,
// затем field/add_field уже живут внутри возвращённого типизированного scope.
template <numeric_stats_aggregate StatsT, typename ParentScope, auto Getter, auto Domain>
void register_stats(devils_script::system& sys, const std::string_view scope_name) {
  sys.register_function<&stats_detail::enter<StatsT, ParentScope, Getter>>(std::string(scope_name));
  register_stat_accessors<StatsT, Domain>(sys);
}

} // namespace act
} // namespace devils_engine

#endif
