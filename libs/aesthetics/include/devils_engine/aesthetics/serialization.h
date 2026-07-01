#ifndef DEVILS_ENGINE_AESTHETICS_SERIALIZATION_H
#define DEVILS_ENGINE_AESTHETICS_SERIALIZATION_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bit>
#include <limits>
#include <vector>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <tuple>
#include <variant>
#include <type_traits>
#include <utility>
#include <algorithm>

#include <reflect>

#include "devils_engine/utils/hash.h"
#include "devils_engine/utils/type_traits.h"
#include "devils_engine/utils/core.h"
#include "world.h"

// Собственный бинарный сериализатор поверх qlibs/reflect (zpp_bits убран — мы и так делали
// 80% его работы в serialize_det). Байты пишутся в КАНОНИЧНОМ little-endian через сдвиги/маски,
// поэтому формат host-НЕЗАВИСИМ (одинаков на x86 и на big-endian консолях) без byteswap.
//
// Формат снапшота:
//   [magic:u32][fingerprint:u32]
//   [cur_index:u64][removed_count:u64][ removed_entities: entityid_t* ]  -- world::snapshot_state
//   [block_count:u32]
//   block* : [hash:u32][byte_len:u32][ payload = count:u32 + (entityid_t, T)*count ]
//
// hash        = murmur32(type_name<T>) -- стабильный ключ типа (не runtime-индекс).
// byte_len    = размер payload; неизвестный тип пропускается через reader.skip(len).
// fingerprint = отпечаток схемы (см. canon): свёртка (hash ^ layout_hash<T>) по типам.

namespace devils_engine {
namespace aesthetics {
namespace serial {

static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
              "serializer assumes IEEE-754 float/double for bit_cast");

constexpr uint32_t snapshot_magic = UINT32_C(0xDE5A0001); // 'DE' snapshot v01

// эмитится в КОНЦЕ load_world (мир уже целый) -> системы/акторы пересобирают свои
// query/lazy_query и прочие кэши: загрузка пишет мимо world::create, create-события НЕ летят,
// поэтому кэши query сами не обновятся. view/lazy_view пересборки не требуют (ленивые).
struct snapshot_loaded_event {};

// --- буфер: LE writer/reader (host-независимо через сдвиги/маски) -------------

// Позиционный writer: пишет memcpy/индексом в буфер, проверка ёмкости РАЗ на значение (не на байт).
// Рост контролируем сами (удвоение) -> без загадочного O(n^2) zpp_bits. В горячем пути буфер
// предразмечен по estimate_size, поэтому ensure() обычно не срабатывает и запись = чистый memcpy.
// LE через сдвиги/маски -> host-НЕЗАВИСИМО (big-endian консоли тоже). Позиция = p (не b.size()).
struct writer {
  std::vector<std::byte>& b;
  std::size_t p = 0;

  std::size_t pos() const noexcept { return p; }
  void ensure(const std::size_t n) { if (p + n > b.size()) b.resize(p + n > b.size() * 2 ? p + n : b.size() * 2 + 64); }
  void raw(const void* src, const std::size_t n) { ensure(n); std::memcpy(b.data() + p, src, n); p += n; }
  void u8(const uint8_t v) { ensure(1); b[p++] = std::byte(v); }
  void u16(const uint16_t v) { ensure(2); for (int i = 0; i < 2; ++i) b[p + i] = std::byte(uint8_t(v >> (8 * i))); p += 2; }
  void u32(const uint32_t v) { ensure(4); for (int i = 0; i < 4; ++i) b[p + i] = std::byte(uint8_t(v >> (8 * i))); p += 4; }
  void u64(const uint64_t v) { ensure(8); for (int i = 0; i < 8; ++i) b[p + i] = std::byte(uint8_t(v >> (8 * i))); p += 8; }
  void f32(const float v) { u32(std::bit_cast<uint32_t>(v)); }
  void f64(const double v) { u64(std::bit_cast<uint64_t>(v)); }
  void patch_u32(const std::size_t at, const uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = std::byte(uint8_t(v >> (8 * i))); }
};

// reader из span; выход за границы -> ok=false (guard, без исключений). Проверка ёмкости раз на значение.
struct reader {
  std::span<const std::byte> b;
  std::size_t pos = 0;
  bool ok = true;

  bool need(const std::size_t n) noexcept { if (pos + n > b.size()) { ok = false; return false; } return true; }
  uint8_t u8() { if (!need(1)) return 0; return uint8_t(b[pos++]); }
  uint16_t u16() { if (!need(2)) return 0; uint16_t v = 0; for (int i = 0; i < 2; ++i) v |= uint16_t(uint8_t(b[pos + i])) << (8 * i); pos += 2; return v; }
  uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(b[pos + i])) << (8 * i); pos += 4; return v; }
  uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(b[pos + i])) << (8 * i); pos += 8; return v; }
  float f32() { return std::bit_cast<float>(u32()); }
  double f64() { return std::bit_cast<double>(u64()); }
  std::span<const std::byte> take(const std::size_t n) { if (!need(n)) return {}; auto s = b.subspan(pos, n); pos += n; return s; }
  void skip(const std::size_t n) { if (need(n)) pos += n; }
};

using out_t = writer; // совместимость имён в сигнатурах
using in_t  = reader;

// Точка расширения для ВНЕШНИХ не-агрегатных типов (glm::vec и т.п.) — чтобы не тащить их
// зависимости в aesthetics. Специализируй serial::adapter<T> в СВОЁМ модуле:
//   template <> struct adapter<glm::vec3> {
//     static constexpr std::string_view name = "glm.vec3f"; // КРОСС-КОМПИЛЯТОРНЫЙ тег для fingerprint
//     static void write(writer& w, const glm::vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
//     static void read (reader& r,       glm::vec3& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); }
//   };
// Тип с адаптером — ЛИСТ: canon = name, сериализация = write/read; reflect его не трогает.
template <typename T> struct adapter;

// --- compile-time отпечаток layout ------------------------------------------

namespace detail {
constexpr uint32_t fnv_offset = UINT32_C(2166136261);
constexpr uint32_t fnv_prime  = UINT32_C(16777619);
constexpr std::size_t max_recursion = 16; // страховка от рекурсивных структур (unique_ptr<Self>, vector<Self>)

consteval uint32_t hash_str(uint32_t h, const std::string_view s) noexcept {
  for (const char c : s) h = (h ^ uint32_t(uint8_t(c))) * fnv_prime;
  return h;
}
consteval uint32_t mix32(const uint32_t h, const uint32_t v) noexcept { return (h ^ v) * fnv_prime; }

// --- распознавание типов -----------------------------------------------------
template <typename> struct is_std_array : std::false_type {};
template <typename T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template <typename> struct is_unique_ptr : std::false_type {};
template <typename T, typename D> struct is_unique_ptr<std::unique_ptr<T, D>> : std::true_type {};
template <typename> struct is_shared_ptr : std::false_type {};
template <typename T> struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};
template <typename> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};
template <typename> struct is_string_view : std::false_type {};
template <typename C, typename Tr> struct is_string_view<std::basic_string_view<C, Tr>> : std::true_type {};
template <typename> struct is_std_string : std::false_type {};
template <typename C, typename Tr, typename A> struct is_std_string<std::basic_string<C, Tr, A>> : std::true_type {};
template <typename> struct is_span : std::false_type {};
template <typename T, std::size_t E> struct is_span<std::span<T, E>> : std::true_type {};
template <typename> struct is_pair : std::false_type {};
template <typename A, typename B> struct is_pair<std::pair<A, B>> : std::true_type {};
template <typename> struct is_tuple : std::false_type {};
template <typename... Ts> struct is_tuple<std::tuple<Ts...>> : std::true_type {};
template <typename> struct is_variant : std::false_type {};
template <typename... Ts> struct is_variant<std::variant<Ts...>> : std::true_type {};

template <typename T> concept map_like = requires { typename T::key_type; typename T::mapped_type; typename T::value_type; };
template <typename T> concept seq_like = requires(T t) { typename T::value_type; t.begin(); t.end(); } && !map_like<T>;
// пользователь специализировал serial::adapter<T> (наличия name достаточно для canon).
template <typename T> concept adapted = requires { { adapter<std::remove_cvref_t<T>>::name } -> std::convertible_to<std::string_view>; };

// каноничное СЕМАНТИЧЕСКОЕ имя скаляра — не зависит от компилятора
// (uint64_t: gcc "unsigned long" vs msvc "unsigned __int64" -> оба дают "u64";
//  ловит фундаментальные типы и ВНУТРИ контейнеров, где строковая правка type_name бессильна).
template <typename U>
consteval std::string_view scalar_tag() noexcept {
  if constexpr (std::is_same_v<U, bool>) return "b";
  else if constexpr (std::is_same_v<U, char>) return "c"; // char отделён от i8/u8: 1 байт вне зависимости от знаковости
  else if constexpr (std::is_same_v<U, char8_t> || std::is_same_v<U, char16_t> || std::is_same_v<U, char32_t> || std::is_same_v<U, wchar_t>) {
    if constexpr (sizeof(U) == 1) return "w8"; else if constexpr (sizeof(U) == 2) return "w16"; else return "w32";
  } else if constexpr (std::is_floating_point_v<U>) {
    if constexpr (sizeof(U) == 4) return "f32"; else if constexpr (sizeof(U) == 8) return "f64"; else return "fX";
  } else if constexpr (std::is_signed_v<U>) {
    if constexpr (sizeof(U) == 1) return "i8"; else if constexpr (sizeof(U) == 2) return "i16";
    else if constexpr (sizeof(U) == 4) return "i32"; else if constexpr (sizeof(U) == 8) return "i64"; else return "iX";
  } else {
    if constexpr (sizeof(U) == 1) return "u8"; else if constexpr (sizeof(U) == 2) return "u16";
    else if constexpr (sizeof(U) == 4) return "u32"; else if constexpr (sizeof(U) == 8) return "u64"; else return "uX";
  }
}

// canon: ОДИН проход = валидация (static_assert на запрещённом) + кросс-компиляторный отпечаток.
template <typename T, std::size_t Depth = 0>
consteval uint32_t canon(uint32_t h) {
  using U = std::remove_cvref_t<T>;

  static_assert(!std::is_pointer_v<U>,     "serializable: сырой указатель не сериализуется — используй std::unique_ptr<T>");
  static_assert(!std::is_array_v<U>,       "serializable: C-массив не сериализуется — используй std::array<T, N>");
  static_assert(!is_string_view<U>::value, "serializable: std::string_view невладеющий — храни std::string");
  static_assert(!is_span<U>::value,        "serializable: std::span невладеющий — храни владеющий контейнер");

  if constexpr (adapted<U>) {
    return hash_str(h, adapter<U>::name);      // внешний тип (glm и т.п.) -> кросс-компиляторный тег
  } else if constexpr (Depth > max_recursion) {
    return hash_str(h, utils::type_name<U>()); // страховка от бесконечной рекурсии
  } else if constexpr (std::is_enum_v<U>) {
    return canon<std::underlying_type_t<U>, Depth + 1>(hash_str(h, "e:"));
  } else if constexpr (std::is_arithmetic_v<U>) {
    return hash_str(h, scalar_tag<U>());
  } else if constexpr (is_std_string<U>::value) {
    return hash_str(h, "str");
  } else if constexpr (is_std_array<U>::value) {
    return canon<typename U::value_type, Depth + 1>(mix32(hash_str(h, "arr"), uint32_t(std::tuple_size_v<U>)));
  } else if constexpr (is_unique_ptr<U>::value || is_shared_ptr<U>::value) {
    return canon<typename U::element_type, Depth + 1>(hash_str(h, "ptr"));
  } else if constexpr (is_optional<U>::value) {
    return canon<typename U::value_type, Depth + 1>(hash_str(h, "opt"));
  } else if constexpr (is_pair<U>::value) {
    return canon<typename U::second_type, Depth + 1>(canon<typename U::first_type, Depth + 1>(hash_str(h, "pair")));
  } else if constexpr (is_tuple<U>::value) {
    h = hash_str(h, "tup");
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((h = canon<std::tuple_element_t<I, U>, Depth + 1>(h)), ...);
    }(std::make_index_sequence<std::tuple_size_v<U>>());
    return h;
  } else if constexpr (is_variant<U>::value) {
    h = hash_str(h, "var");
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((h = canon<std::variant_alternative_t<I, U>, Depth + 1>(h)), ...);
    }(std::make_index_sequence<std::variant_size_v<U>>());
    return h;
  } else if constexpr (map_like<U>) {
    return canon<typename U::mapped_type, Depth + 1>(canon<typename U::key_type, Depth + 1>(hash_str(h, "map")));
  } else if constexpr (seq_like<U>) {
    return canon<typename U::value_type, Depth + 1>(hash_str(h, "seq"));
  } else if constexpr (std::is_aggregate_v<U>) {
    h = hash_str(h, "{");
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((h = canon<std::remove_cvref_t<decltype(reflect::get<I>(std::declval<U&>()))>, Depth + 1>(h)), ...);
    }(std::make_index_sequence<reflect::size<U>()>());
    return hash_str(h, "}");
  } else {
    // не-агрегатный пользовательский тип без адаптера -> сериализатор его не поддержит (см. serialize).
    return hash_str(h, utils::type_name<U>());
  }
}

template <typename T>
consteval uint32_t layout_hash() noexcept { return canon<T, 0>(fnv_offset); }
} // namespace detail

// --- рекурсивная бинарная сериализация ---------------------------------------
// Хеш-мапа пишется СОРТИРОВАННОЙ по ключу -> детерминизм байтов для netcode/checksum.
// Владеющие указатели/optional -> [has:u8]+value. Всё через reflect, поля позиционно.

template <typename C, typename E>
void det_insert(C& c, E&& e) {
  if constexpr (requires { c.push_back(std::forward<E>(e)); }) c.push_back(std::forward<E>(e));
  else c.insert(std::forward<E>(e));
}

template <typename T> void serialize(writer& w, const T& v);
template <typename T> void deserialize(reader& r, T& v);

namespace detail {
template <typename V, std::size_t I = 0>
void variant_set(reader& r, V& v, const std::size_t idx) {
  if constexpr (I < std::variant_size_v<V>) {
    if (idx == I) { std::variant_alternative_t<I, V> alt{}; deserialize(r, alt); v = std::move(alt); }
    else variant_set<V, I + 1>(r, v, idx);
  }
}
} // namespace detail

template <typename T>
void serialize(writer& w, const T& v) {
  using U = std::remove_cvref_t<T>;
  if constexpr (detail::adapted<U>) {
    adapter<U>::write(w, v);
  } else if constexpr (std::is_same_v<U, bool>) {
    w.u8(v ? 1 : 0);
  } else if constexpr (std::is_enum_v<U>) {
    serialize(w, static_cast<std::underlying_type_t<U>>(v));
  } else if constexpr (std::is_integral_v<U>) {
    if constexpr (sizeof(U) == 1) w.u8(uint8_t(v));
    else if constexpr (sizeof(U) == 2) w.u16(uint16_t(v));
    else if constexpr (sizeof(U) == 4) w.u32(uint32_t(v));
    else w.u64(uint64_t(v));
  } else if constexpr (std::is_floating_point_v<U>) {
    if constexpr (sizeof(U) == 4) w.f32(v); else w.f64(v);
  } else if constexpr (detail::is_std_string<U>::value) {
    w.u64(v.size());
    w.raw(v.data(), v.size());
  } else if constexpr (detail::is_std_array<U>::value) {
    for (const auto& e : v) serialize(w, e); // размер фиксирован типом
  } else if constexpr (detail::is_unique_ptr<U>::value || detail::is_shared_ptr<U>::value) {
    w.u8(v ? 1 : 0);
    if (v) serialize(w, *v);
  } else if constexpr (detail::is_optional<U>::value) {
    w.u8(v.has_value() ? 1 : 0);
    if (v.has_value()) serialize(w, *v);
  } else if constexpr (detail::is_pair<U>::value) {
    serialize(w, v.first); serialize(w, v.second);
  } else if constexpr (detail::is_tuple<U>::value) {
    std::apply([&](const auto&... es) { (serialize(w, es), ...); }, v);
  } else if constexpr (detail::is_variant<U>::value) {
    w.u32(uint32_t(v.index()));
    std::visit([&](const auto& x) { serialize(w, x); }, v);
  } else if constexpr (detail::map_like<U>) {
    std::vector<const typename U::value_type*> ordered; // сортировка по ключу -> детерминизм
    ordered.reserve(v.size());
    for (const auto& kv : v) ordered.push_back(std::addressof(kv));
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
    w.u64(ordered.size());
    for (const auto* kv : ordered) { serialize(w, kv->first); serialize(w, kv->second); }
  } else if constexpr (detail::seq_like<U>) {
    w.u64(v.size());
    for (const auto& e : v) serialize(w, e);
  } else if constexpr (std::is_aggregate_v<U>) {
    [&]<std::size_t... I>(std::index_sequence<I...>) { (serialize(w, reflect::get<I>(v)), ...); }(std::make_index_sequence<reflect::size<U>()>());
  } else {
    static_assert(sizeof(U) == 0, "serializable: неподдерживаемый тип (не-агрегат без adapter<T>)");
  }
}

template <typename T>
void deserialize(reader& r, T& v) {
  using U = std::remove_cvref_t<T>;
  if constexpr (detail::adapted<U>) {
    adapter<U>::read(r, v);
  } else if constexpr (std::is_same_v<U, bool>) {
    v = r.u8() != 0;
  } else if constexpr (std::is_enum_v<U>) {
    std::underlying_type_t<U> u{}; deserialize(r, u); v = U(u);
  } else if constexpr (std::is_integral_v<U>) {
    if constexpr (sizeof(U) == 1) v = U(r.u8());
    else if constexpr (sizeof(U) == 2) v = U(r.u16());
    else if constexpr (sizeof(U) == 4) v = U(r.u32());
    else v = U(r.u64());
  } else if constexpr (std::is_floating_point_v<U>) {
    v = (sizeof(U) == 4) ? U(r.f32()) : U(r.f64());
  } else if constexpr (detail::is_std_string<U>::value) {
    const uint64_t n = r.u64();
    const auto s = r.take(n);
    v.assign(reinterpret_cast<const char*>(s.data()), s.size());
  } else if constexpr (detail::is_std_array<U>::value) {
    for (auto& e : v) deserialize(r, e);
  } else if constexpr (detail::is_unique_ptr<U>::value) {
    if (r.u8()) { v = std::make_unique<typename U::element_type>(); deserialize(r, *v); } else v.reset();
  } else if constexpr (detail::is_shared_ptr<U>::value) {
    if (r.u8()) { v = std::make_shared<typename U::element_type>(); deserialize(r, *v); } else v.reset();
  } else if constexpr (detail::is_optional<U>::value) {
    if (r.u8()) { v.emplace(); deserialize(r, *v); } else v.reset();
  } else if constexpr (detail::is_pair<U>::value) {
    deserialize(r, v.first); deserialize(r, v.second);
  } else if constexpr (detail::is_tuple<U>::value) {
    std::apply([&](auto&... es) { (deserialize(r, es), ...); }, v);
  } else if constexpr (detail::is_variant<U>::value) {
    const uint32_t idx = r.u32();
    if (idx < std::variant_size_v<U>) detail::variant_set<U>(r, v, idx);
    else r.ok = false;
  } else if constexpr (detail::map_like<U>) {
    const uint64_t n = r.u64();
    v.clear();
    for (uint64_t i = 0; i < n && r.ok; ++i) {
      typename U::key_type k{};
      typename U::mapped_type val{};
      deserialize(r, k); deserialize(r, val);
      v.emplace(std::move(k), std::move(val));
    }
  } else if constexpr (detail::seq_like<U>) {
    const uint64_t n = r.u64();
    v.clear();
    for (uint64_t i = 0; i < n && r.ok; ++i) { typename U::value_type e{}; deserialize(r, e); det_insert(v, std::move(e)); }
  } else if constexpr (std::is_aggregate_v<U>) {
    [&]<std::size_t... I>(std::index_sequence<I...>) { (deserialize(r, reflect::get<I>(v)), ...); }(std::make_index_sequence<reflect::size<U>()>());
  } else {
    static_assert(sizeof(U) == 0, "serializable: неподдерживаемый тип (не-агрегат без adapter<T>)");
  }
}

// --- блочные примитивы -------------------------------------------------------

// оценка размера блока для reserve буфера (см. estimate_size). Точна для фикс-типов,
// занижена для динамики (vector/string/map) — недостача дорастётся нативным push_back, дёшево.
template <typename T>
std::size_t estimate_one(const world* w) {
  const auto* stor = w->get_allocator<T>();
  return stor != nullptr ? stor->components.size() * (sizeof(T) + sizeof(entityid_t) + 8) : 0;
}

// пишет payload: [count][ (id, component)*count ].
template <typename T>
void dump_one(const world* w, writer& wr) {
  const auto* stor = w->get_allocator<T>();
  const uint32_t count = stor != nullptr ? uint32_t(stor->components.size()) : 0;
  wr.u32(count);
  if (stor == nullptr) return;

  // позиционный проход по sparce_set (O(n)); entity_at_dense_index O(n) -> не годится.
  const auto& ss = stor->sparce_set;
  for (std::size_t i = 0; i < ss.size(); ++i) {
    if (is_invalid_entityid(ss[i])) continue;
    const entityid_t id = make_entityid(i, get_entityid_version(ss[i]));
    const std::size_t dense = get_entityid_index(ss[i]);
    serialize(wr, id);
    serialize(wr, stor->components[dense]);
  }
}

// читает payload и пишет прямо в аллокатор, МИМО world::create -> без emit событий
// (иначе разбудим системы/queries на полупостроенном мире).
template <typename T>
void load_one(world* w, reader& r) {
  const uint32_t count = r.u32();
  auto* stor = w->get_or_create_allocator<T>(sizeof(T) * 250);
  for (uint32_t i = 0; i < count && r.ok; ++i) {
    entityid_t id = invalid_entityid;
    deserialize(r, id);
    T tmp{};
    deserialize(r, tmp); // всегда вычитываем -> выравнивание не рушится на дубле
    if (auto* slot = stor->create_comp(id)) *slot = std::move(tmp);
  }
}

// --- реестр ------------------------------------------------------------------

class component_registry {
public:
  using dump_fn = void (*)(const world*, writer&);
  using load_fn = void (*)(world*, reader&);
  using size_fn = std::size_t (*)(const world*);

  struct entry {
    uint32_t hash;
    uint32_t layout;
    std::string_view name;
    dump_fn dump;
    load_fn load;
    size_fn est;
  };

  static std::vector<entry>& table() noexcept; // Мейерс-синглтон, определён в serialization.cpp

  template <typename T>
  static bool add() {
    // компонент обязан быть агрегатом (нужно и reflect'у). ВНИМАНИЕ: C-массивы в полях (char[N])
    // reflect не поддержит и упадёт СВОИМ сообщением про structured binding — используй std::array<T,N>.
    static_assert(std::is_aggregate_v<T>, "serializable component must be an aggregate (no user-declared ctors)");
    const std::string_view name = utils::type_name<T>();
    const uint32_t h = utils::murmur_hash3_32(name);

    auto& t = table();
    const auto it = std::lower_bound(t.begin(), t.end(), h,
      [](const entry& e, const uint32_t v) { return e.hash < v; });

    if (it != t.end() && it->hash == h) // тихая коллизия -> громкая ошибка
      utils::error{}("component hash collision (murmur32=0x{:08x}): '{}' vs '{}'", h, it->name, name);

    t.insert(it, entry{ h, detail::layout_hash<T>(), name, &dump_one<T>, &load_one<T>, &estimate_one<T> });
    aesthetics::component_type_id<T>(); // прибиваем монотонный runtime-индекс на старте
    return true;
  }

  // отпечаток схемы: свёртка по УЖЕ отсортированной таблице -> детерминирован. В serialization.cpp.
  static uint32_t fingerprint() noexcept;
};

#define SERIALIZABLE_COMPONENT(T) \
  inline const bool _serreg_##T = ::devils_engine::aesthetics::serial::component_registry::add<T>();

// --- диспетчер (реализация в serialization.cpp) ------------------------------

// оценка размера снапшота для reserve буфера.
std::size_t estimate_size(const world* w);

// снапшот мира -> writer. Порядок блоков = по hash (детерминирован).
void dump_world(const world* w, writer& wr);

// удобная обёртка: reserve по estimate_size, дампит, возвращает готовый буфер.
std::vector<std::byte> dump_world(const world* w);

// грузить в ЧИСТЫЙ world. В КОНЦЕ эмитит snapshot_loaded_event -> системы пересобирают query.
// false при несовпадении magic/схемы или обрыве данных (guard, не migration).
bool load_world(world* w, reader& r);

} // namespace serial
} // namespace aesthetics
} // namespace devils_engine

#endif
