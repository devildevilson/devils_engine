#ifndef TILE_FRONTIER_CORE_INSTANCE_LAYOUT_H
#define TILE_FRONTIER_CORE_INSTANCE_LAYOUT_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <glm/glm.hpp>

#include <reflect>
#include <devils_engine/utils/core.h>
#include <devils_engine/painter/common.h>

// Сопоставление C++-агрегата с layout-строкой draw_group (грамматика painter::format,
// напр. "v4", "v3c4", "v4ui1"). Идея: layout инстанса data-driven (живёт в конфиге
// draw_group), поэтому конкретные структуры остаются только на стороне main, а матчер
// проверяет на этапе подготовки, что разложение T по полям байт-в-байт соответствует
// тому, что ждёт draw_group. Матчинг — ПО ТОКЕНАМ (= по границам вершинных атрибутов /
// location'ов), а не по плоским байтам: иначе "v4" и "v2v2" были бы неотличимы.
//
// Разрешённые поля агрегата: float, uint32_t, int32_t, glm::vec<L,U,Q>, std::array<U,N>
// (U из тех же скаляров) и вложенные агрегаты из этих же типов. Любой другой тип поля —
// ошибка компиляции в flatten<T>()/count_atoms<T>().

namespace tile_frontier {
namespace core {
namespace instance_layout {

using namespace devils_engine;

struct rgba8_color {
  uint32_t value = 0xffffffffu;
};

// один атом = один вершинный атрибут: тип элемента + число компонент + размер в байтах
struct atom {
  painter::format_element_type::values element_type = painter::format_element_type::INVALID;
  uint32_t el_count = 0;
  uint32_t size = 0;

  constexpr bool operator==(const atom& o) const noexcept {
    return element_type == o.element_type && el_count == o.el_count && size == o.size;
  }
};

namespace detail {

template <typename> inline constexpr bool always_false = false;

// скаляр C++ → тип элемента painter
template <typename U> consteval painter::format_element_type::values scalar_element_type() {
  if constexpr (std::is_same_v<U, float>) return painter::format_element_type::SFLOAT;
  else if constexpr (std::is_same_v<U, uint32_t>) return painter::format_element_type::UINT;
  else if constexpr (std::is_same_v<U, int32_t>) return painter::format_element_type::SINT;
  else static_assert(always_false<U>, "scalar element must be float / uint32_t / int32_t");
}

// тип поля агрегата T под индексом N (без default-конструирования T)
template <typename T, std::size_t N>
using field_type_t = std::remove_cvref_t<decltype(reflect::get<N>(std::declval<const T&>()))>;

} // namespace detail

// --- листовые типы (одно поле = один атрибут) ---

template <typename T> struct is_leaf : std::false_type {};
template <> struct is_leaf<float> : std::true_type {};
template <> struct is_leaf<uint32_t> : std::true_type {};
template <> struct is_leaf<int32_t> : std::true_type {};
template <> struct is_leaf<rgba8_color> : std::true_type {};
template <glm::length_t L, typename U, glm::qualifier Q> struct is_leaf<glm::vec<L, U, Q>> : std::true_type {};
template <typename U, std::size_t N> struct is_leaf<std::array<U, N>> : std::true_type {};

template <typename T> struct leaf_traits;
template <> struct leaf_traits<float> { static constexpr atom value{painter::format_element_type::SFLOAT, 1, 4}; };
template <> struct leaf_traits<uint32_t> { static constexpr atom value{painter::format_element_type::UINT, 1, 4}; };
template <> struct leaf_traits<int32_t> { static constexpr atom value{painter::format_element_type::SINT, 1, 4}; };
template <> struct leaf_traits<rgba8_color> { static constexpr atom value{painter::format_element_type::UNORM, 4, 4}; };
template <glm::length_t L, typename U, glm::qualifier Q> struct leaf_traits<glm::vec<L, U, Q>> {
  static constexpr atom value{detail::scalar_element_type<U>(), uint32_t(L), uint32_t(uint32_t(L) * sizeof(U))};
};
template <typename U, std::size_t N> struct leaf_traits<std::array<U, N>> {
  static constexpr atom value{detail::scalar_element_type<U>(), uint32_t(N), uint32_t(N * sizeof(U))};
};

// --- разложение агрегата в плоскую последовательность атомов ---
// std::array — агрегат, но мы трактуем его как лист, поэтому ветка is_leaf идёт ПЕРВОЙ.

template <typename T> consteval uint32_t count_atoms() {
  if constexpr (is_leaf<T>::value) {
    return 1;
  } else if constexpr (std::is_aggregate_v<T>) {
    uint32_t n = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((n += count_atoms<detail::field_type_t<T, I>>()), ...);
    }(std::make_index_sequence<reflect::size<T>()>{});
    return n;
  } else {
    static_assert(detail::always_false<T>,
      "instance field must be float/uint32_t/int32_t/glm::vec/std::array or an aggregate of these");
    return 0;
  }
}

template <typename T, std::size_t Cap>
consteval void fill_atoms(std::array<atom, Cap>& out, uint32_t& idx) {
  if constexpr (is_leaf<T>::value) {
    out[idx++] = leaf_traits<T>::value;
  } else if constexpr (std::is_aggregate_v<T>) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (fill_atoms<detail::field_type_t<T, I>>(out, idx), ...);
    }(std::make_index_sequence<reflect::size<T>()>{});
  } else {
    static_assert(detail::always_false<T>, "unreachable: validated by count_atoms<T>()");
  }
}

// последовательность атомов агрегата T (compile-time)
template <typename T> consteval auto flatten() {
  std::array<atom, count_atoms<T>()> out{};
  uint32_t idx = 0;
  fill_atoms<T>(out, idx);
  return out;
}

// размер одного инстанса T в байтах (= stride)
template <typename T> constexpr uint32_t stride_of() noexcept { return uint32_t(sizeof(T)); }

// --- layout-строка → атомы (тот же токенайзер, что painter::structures::parse_layout) ---
//
// БЕЗ АЛЛОКАЦИЙ: атомы кладём в переданный буфер `out`, парсим не больше out.size() штук за вызов.
// Возврат = { остаток строки, сколько атомов распознано в этот вызов }. Вызывать в цикле:
//
//   std::string_view cur = layout;
//   std::array<atom, 16> buf;
//   while (!cur.empty()) {
//     const auto [remain, count] = parse_layout_atoms(cur, std::span(buf));
//     if (count == 0) break;               // нераспознанный токен / нет прогресса
//     // ... обработать buf[0..count) ...
//     cur = remain;
//   }
//
// Остановки: буфер заполнен (remain — хвост), строка кончилась (remain пуст), либо встречен
// нераспознанный токен (remain начинается с этого токена, count = атомов до него).
inline std::tuple<std::string_view, std::size_t>
parse_layout_atoms(const std::string_view& str, const std::span<atom>& out) {
  if (str.empty() || std::isdigit(static_cast<unsigned char>(str[0]))) return {std::string_view{}, 0};

  std::size_t i = 0;
  std::size_t n = 0;
  while (i < str.size() && n < out.size()) {
    const std::size_t start = i;
    for (; i < str.size() && !std::isdigit(static_cast<unsigned char>(str[i])); ++i) {}
    for (; i < str.size() && std::isdigit(static_cast<unsigned char>(str[i])); ++i) {}

    const auto part = str.substr(start, i - start);
    const auto fmt = painter::format::from_string(part);
    if (fmt >= painter::format::count) return {str.substr(start), n}; // нераспознанный токен — стоп
    out[n++] = atom{painter::format::element_type(fmt), painter::format::el_count(fmt), painter::format::size(fmt)};
  }

  return {str.substr(i), n};
}

// --- результат сверки ---
// БЕЗ СТРОК: только код проблемы + числовой контекст. Текст пусть пишет внешний код.

namespace match_error {
enum values : uint32_t {
  ok,                        // совпало
  empty_layout,              // layout пуст / не распарсился ни в один атрибут
  parse_error,               // нераспознанный токен в layout-строке (where = индекс атома)
  attribute_count_mismatch,  // разное число атрибутов (expected = у структуры, actual = у layout)
  attribute_mismatch,        // атрибут #where отличается (тип/число компонент/размер)
  stride_mismatch,           // sizeof(T) != GPU-страйд (expected = sizeof(T), actual = страйд)
  count
};

inline std::string_view to_string(const values v) noexcept {
  switch (v) {
    case ok:                       return "ok";
    case empty_layout:             return "empty_layout";
    case parse_error:              return "parse_error";
    case attribute_count_mismatch: return "attribute_count_mismatch";
    case attribute_mismatch:       return "attribute_mismatch";
    case stride_mismatch:          return "stride_mismatch";
    default:                       return "unknown";
  }
}
} // namespace match_error

struct match_result {
  match_error::values error = match_error::ok;
  uint32_t where = 0;     // индекс атрибута (для *_mismatch / parse_error)
  uint32_t expected = 0;  // ожидалось (число атрибутов / страйд)
  uint32_t actual = 0;    // по факту (число атрибутов / страйд)

  bool ok() const noexcept { return error == match_error::ok; }
  explicit operator bool() const noexcept { return ok(); }
};

// размер атрибута на GPU = align_to(max(size, 4), 4) — РОВНО правило painter::compute_size
// (structures.cpp): каждый атрибут занимает минимум 4 байта, округлённо вверх до 4. Поэтому
// v3=12, v4=16, ui1/float=4, а суб-4-байтные c3/c1 раздуваются до 4 (отсюда "c3c1 = 8 байт").
// ВНИМАНИЕ: это НЕ std140/std430 — там vec3 выравнивается на 16; для вершинного/instance-буфера
// (а draw_intent целится именно туда) такого паддинга нет. std430-режим у painter — TODO.
constexpr uint32_t gpu_atom_size(const uint32_t tight_size) noexcept {
  return uint32_t(utils::align_to(std::max<std::size_t>(tight_size, sizeof(uint32_t)), sizeof(uint32_t)));
}

// сверка разложения T с layout-строкой draw_group ("v4", "v3c4", ...). Без аллокаций: парсим
// чанками в фиксированный буфер и сверяем инкрементально с flatten<T>(), попутно копя GPU-страйд.
template <typename T>
match_result check(const std::string_view& layout) {
  static constexpr auto fields = flatten<T>();
  if (layout.empty()) return {match_error::empty_layout};

  std::array<atom, 16> buf{};
  std::string_view cur = layout;
  std::size_t idx = 0;        // сколько атрибутов layout уже сверили
  uint32_t gpu_stride = 0;

  while (!cur.empty()) {
    const auto [remain, n] = parse_layout_atoms(cur, std::span<atom>(buf));
    if (n == 0) return {match_error::parse_error, uint32_t(idx)};  // нераспознанный токен / нет прогресса
    for (std::size_t i = 0; i < n; ++i, ++idx) {
      if (idx >= fields.size())                                    // в layout атрибутов больше, чем в T
        return {match_error::attribute_count_mismatch, uint32_t(idx), uint32_t(fields.size()), uint32_t(idx) + 1};
      if (!(fields[idx] == buf[i]))
        return {match_error::attribute_mismatch, uint32_t(idx)};
      gpu_stride += gpu_atom_size(buf[i].size);
    }
    cur = remain;
  }

  if (idx != fields.size())                                        // в layout атрибутов меньше, чем в T
    return {match_error::attribute_count_mismatch, uint32_t(idx), uint32_t(fields.size()), uint32_t(idx)};
  if (stride_of<T>() != gpu_stride)
    return {match_error::stride_mismatch, 0, stride_of<T>(), gpu_stride};

  return {};
}

// сверка с распарсенным draw_group: атомы из instance_layout + painter-страйд как источник правды.
// Без аллокаций — сравниваем на лету, painter::format::values → atom поэлементно.
template <typename T>
match_result check(const std::span<const painter::format::values>& layout, const uint32_t gpu_stride) {
  static constexpr auto fields = flatten<T>();
  if (layout.empty()) return {match_error::empty_layout};

  if (layout.size() != fields.size())
    return {match_error::attribute_count_mismatch, 0, uint32_t(fields.size()), uint32_t(layout.size())};

  for (std::size_t i = 0; i < layout.size(); ++i) {
    const auto fmt = layout[i];
    if (fmt >= painter::format::count) return {match_error::parse_error, uint32_t(i)};
    const atom a{painter::format::element_type(fmt), painter::format::el_count(fmt), painter::format::size(fmt)};
    if (!(fields[i] == a)) return {match_error::attribute_mismatch, uint32_t(i)};
  }

  if (stride_of<T>() != gpu_stride)
    return {match_error::stride_mismatch, 0, stride_of<T>(), gpu_stride};

  return {};
}

} // namespace instance_layout
} // namespace core
} // namespace tile_frontier

#endif
