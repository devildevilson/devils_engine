#ifndef DEVILS_ENGINE_UTILS_SERIALIZATION_H
#define DEVILS_ENGINE_UTILS_SERIALIZATION_H

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <reflect>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/type_traits.h"

// Project-neutral canonical binary codec. It owns byte order, primitive/container grammar,
// bounded writes and type-layout fingerprints; it knows nothing about ECS worlds, checkpoints,
// files or transports. Domain libraries provide adapter<T> specializations and compose records.

namespace devils_engine::utils::serial {

static_assert(std::numeric_limits<float>::is_iec559 &&
                std::numeric_limits<double>::is_iec559,
              "serializer assumes IEEE-754 float/double for bit_cast");

class writer {
public:
  explicit writer(std::vector<std::byte>& bytes, const bool allow_growth = true) noexcept
    : b(bytes), p(bytes.size()), allow_growth_(allow_growth) {}

  [[nodiscard]] std::size_t pos() const noexcept {
    return p;
  }
  [[nodiscard]] std::size_t position() const noexcept {
    return p;
  }
  [[nodiscard]] bool good() const noexcept {
    return ok;
  }
  void fail() noexcept {
    ok = false;
  }

  void raw(const void* source, const std::size_t count) {
    if (!need(count)) return;
    if (count != 0) std::memcpy(b.data() + p, source, count);
    p += count;
  }

  void bytes(const std::span<const std::byte> values) {
    raw(values.data(), values.size());
  }

  void u8(const std::uint8_t value) {
    if (!need(1)) return;
    b[p++] = std::byte(value);
  }

  void u16(const std::uint16_t value) {
    if (!need(2)) return;
    for (unsigned i = 0; i < 2; ++i)
      b[p + i] = std::byte(std::uint8_t(value >> (8 * i)));
    p += 2;
  }

  void u32(const std::uint32_t value) {
    if (!need(4)) return;
    for (unsigned i = 0; i < 4; ++i)
      b[p + i] = std::byte(std::uint8_t(value >> (8 * i)));
    p += 4;
  }

  void u64(const std::uint64_t value) {
    if (!need(8)) return;
    for (unsigned i = 0; i < 8; ++i)
      b[p + i] = std::byte(std::uint8_t(value >> (8 * i)));
    p += 8;
  }

  void f32(const float value) {
    u32(std::bit_cast<std::uint32_t>(value));
  }
  void f64(const double value) {
    u64(std::bit_cast<std::uint64_t>(value));
  }

  void patch_u32(const std::size_t at, const std::uint32_t value) noexcept {
    if (!ok) return;
    if (at > p || p - at < 4) {
      ok = false;
      return;
    }
    for (unsigned i = 0; i < 4; ++i)
      b[at + i] = std::byte(std::uint8_t(value >> (8 * i)));
  }

  std::vector<std::byte>& b;
  std::size_t p = 0;
  bool ok = true;

private:
  bool need(const std::size_t count) {
    if (!ok) return false;
    if (count > std::numeric_limits<std::size_t>::max() - p) {
      ok = false;
      return false;
    }
    const std::size_t required = p + count;
    if (!allow_growth_ && required > b.capacity()) {
      ok = false;
      return false;
    }
    if (required > b.size()) b.resize(required);
    return true;
  }

  bool allow_growth_ = true;
};

struct decode_limits {
  std::size_t values = 8 * 1024 * 1024;
  std::size_t depth = 128;
};

class reader {
public:
  explicit reader(const std::span<const std::byte> bytes, const decode_limits limits = {}) noexcept
    : values_left(limits.values), depth_left(limits.depth), b(bytes) {}

  bool presence() noexcept {
    const auto value = u8();
    if (value > 1) ok = false;
    return ok && value == 1;
  }

  // A value budget also bounds zero-byte elements; byte bounds alone cannot do that.
  bool enter() noexcept {
    if (!ok || values_left == 0 || depth_left == 0) {
      ok = false;
      return false;
    }
    --values_left;
    --depth_left;
    return true;
  }
  void leave() noexcept {
    ++depth_left;
  }
  bool count(const std::uint64_t value) noexcept {
    if (!ok || value > values_left) {
      ok = false;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool good() const noexcept {
    return ok;
  }
  [[nodiscard]] std::size_t position() const noexcept {
    return pos;
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return b.size();
  }

  bool need(const std::size_t count) noexcept {
    if (!ok || pos > b.size() || count > b.size() - pos) {
      ok = false;
      return false;
    }
    return true;
  }

  std::uint8_t u8() noexcept {
    if (!need(1)) return 0;
    return std::to_integer<std::uint8_t>(b[pos++]);
  }

  std::uint16_t u16() noexcept {
    if (!need(2)) return 0;
    std::uint16_t value = 0;
    for (unsigned i = 0; i < 2; ++i)
      value |= std::uint16_t(std::to_integer<std::uint8_t>(b[pos + i])) << (8 * i);
    pos += 2;
    return value;
  }

  std::uint32_t u32() noexcept {
    if (!need(4)) return 0;
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= std::uint32_t(std::to_integer<std::uint8_t>(b[pos + i])) << (8 * i);
    pos += 4;
    return value;
  }

  std::uint64_t u64() noexcept {
    if (!need(8)) return 0;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
      value |= std::uint64_t(std::to_integer<std::uint8_t>(b[pos + i])) << (8 * i);
    pos += 8;
    return value;
  }

  float f32() noexcept {
    return std::bit_cast<float>(u32());
  }
  double f64() noexcept {
    return std::bit_cast<double>(u64());
  }

  std::span<const std::byte> take(const std::size_t count) noexcept {
    if (!need(count)) return {};
    const auto result = b.subspan(pos, count);
    pos += count;
    return result;
  }

  void skip(const std::size_t count) noexcept {
    if (need(count)) pos += count;
  }

  std::size_t values_left;
  std::size_t depth_left;

  std::span<const std::byte> b;
  std::size_t pos = 0;
  bool ok = true;
};

using out_t = writer;
using in_t = reader;

template <typename T>
struct adapter;

namespace detail {
inline constexpr std::uint32_t fnv_offset = UINT32_C(2166136261);
inline constexpr std::uint32_t fnv_prime = UINT32_C(16777619);
inline constexpr std::size_t max_recursion = 16;

consteval std::uint32_t hash_str(std::uint32_t hash, const std::string_view value) noexcept {
  for (const char c : value)
    hash = (hash ^ std::uint32_t(std::uint8_t(c))) * fnv_prime;
  return hash;
}

consteval std::uint32_t mix32(const std::uint32_t hash, const std::uint32_t value) noexcept {
  return (hash ^ value) * fnv_prime;
}

template <typename>
struct is_std_array : std::false_type {};
template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};
template <typename>
struct is_unique_ptr : std::false_type {};
template <typename T, typename D>
struct is_unique_ptr<std::unique_ptr<T, D>> : std::true_type {};
template <typename>
struct is_shared_ptr : std::false_type {};
template <typename T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};
template <typename>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};
template <typename>
struct is_string_view : std::false_type {};
template <typename C, typename Tr>
struct is_string_view<std::basic_string_view<C, Tr>> : std::true_type {};
template <typename>
struct is_std_string : std::false_type {};
template <typename C, typename Tr, typename A>
struct is_std_string<std::basic_string<C, Tr, A>> : std::true_type {};
template <typename>
struct is_span : std::false_type {};
template <typename T, std::size_t E>
struct is_span<std::span<T, E>> : std::true_type {};
template <typename>
struct is_pair : std::false_type {};
template <typename A, typename B>
struct is_pair<std::pair<A, B>> : std::true_type {};
template <typename>
struct is_tuple : std::false_type {};
template <typename... T>
struct is_tuple<std::tuple<T...>> : std::true_type {};
template <typename>
struct is_variant : std::false_type {};
template <typename... T>
struct is_variant<std::variant<T...>> : std::true_type {};

template <typename T>
concept map_like = requires { typename T::key_type; typename T::mapped_type; typename T::value_type; };
template <typename T>
concept seq_like = requires(T value) { typename T::value_type; value.begin(); value.end(); } && !map_like<T>;
template <typename T>
concept adapted = requires {
  { adapter<std::remove_cvref_t<T>>::name } -> std::convertible_to<std::string_view>;
};

template <typename U>
consteval std::string_view scalar_tag() noexcept {
  if constexpr (std::is_same_v<U, bool>)
    return "b";
  else if constexpr (std::is_same_v<U, char>)
    return "c";
  else if constexpr (std::is_same_v<U, char8_t> || std::is_same_v<U, char16_t> ||
                     std::is_same_v<U, char32_t> || std::is_same_v<U, wchar_t>) {
    if constexpr (sizeof(U) == 1)
      return "w8";
    else if constexpr (sizeof(U) == 2)
      return "w16";
    else
      return "w32";
  } else if constexpr (std::is_floating_point_v<U>) {
    if constexpr (sizeof(U) == 4)
      return "f32";
    else if constexpr (sizeof(U) == 8)
      return "f64";
    else
      return "fX";
  } else if constexpr (std::is_signed_v<U>) {
    if constexpr (sizeof(U) == 1)
      return "i8";
    else if constexpr (sizeof(U) == 2)
      return "i16";
    else if constexpr (sizeof(U) == 4)
      return "i32";
    else if constexpr (sizeof(U) == 8)
      return "i64";
    else
      return "iX";
  } else {
    if constexpr (sizeof(U) == 1)
      return "u8";
    else if constexpr (sizeof(U) == 2)
      return "u16";
    else if constexpr (sizeof(U) == 4)
      return "u32";
    else if constexpr (sizeof(U) == 8)
      return "u64";
    else
      return "uX";
  }
}

template <typename T, std::size_t Depth = 0>
consteval std::uint32_t canon(std::uint32_t hash) {
  using U = std::remove_cvref_t<T>;
  static_assert(!std::is_pointer_v<U>, "serializable: raw pointers are not supported");
  static_assert(!std::is_array_v<U>, "serializable: use std::array instead of a C array");
  static_assert(!is_string_view<U>::value, "serializable: string_view is non-owning");
  static_assert(!is_span<U>::value, "serializable: span is non-owning");

  if constexpr (adapted<U>)
    return hash_str(hash, adapter<U>::name);
  else if constexpr (Depth > max_recursion)
    return hash_str(hash, utils::type_name<U>());
  else if constexpr (std::is_enum_v<U>)
    return canon<std::underlying_type_t<U>, Depth + 1>(hash_str(hash, "e:"));
  else if constexpr (std::is_arithmetic_v<U>)
    return hash_str(hash, scalar_tag<U>());
  else if constexpr (is_std_string<U>::value)
    return hash_str(hash, "str");
  else if constexpr (is_std_array<U>::value)
    return canon<typename U::value_type, Depth + 1>(
      mix32(hash_str(hash, "arr"), std::uint32_t(std::tuple_size_v<U>)));
  else if constexpr (is_unique_ptr<U>::value || is_shared_ptr<U>::value)
    return canon<typename U::element_type, Depth + 1>(hash_str(hash, "ptr"));
  else if constexpr (is_optional<U>::value)
    return canon<typename U::value_type, Depth + 1>(hash_str(hash, "opt"));
  else if constexpr (is_pair<U>::value)
    return canon<typename U::second_type, Depth + 1>(
      canon<typename U::first_type, Depth + 1>(hash_str(hash, "pair")));
  else if constexpr (is_tuple<U>::value) {
    hash = hash_str(hash, "tup");
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((hash = canon<std::tuple_element_t<I, U>, Depth + 1>(hash)), ...);
    }(std::make_index_sequence<std::tuple_size_v<U>>());
    return hash;
  } else if constexpr (is_variant<U>::value) {
    hash = hash_str(hash, "var");
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((hash = canon<std::variant_alternative_t<I, U>, Depth + 1>(hash)), ...);
    }(std::make_index_sequence<std::variant_size_v<U>>());
    return hash;
  } else if constexpr (map_like<U>)
    return canon<typename U::mapped_type, Depth + 1>(
      canon<typename U::key_type, Depth + 1>(hash_str(hash, "map")));
  else if constexpr (seq_like<U>)
    return canon<typename U::value_type, Depth + 1>(hash_str(hash, "seq"));
  else if constexpr (std::is_aggregate_v<U>) {
    hash = hash_str(hash, "{");
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((hash = canon<std::remove_cvref_t<decltype(reflect::get<I>(std::declval<U&>()))>,
                     Depth + 1>(hash)),
       ...);
    }(std::make_index_sequence<reflect::size<U>()>());
    return hash_str(hash, "}");
  } else
    return hash_str(hash, utils::type_name<U>());
}

template <typename T>
consteval std::uint32_t layout_hash() noexcept {
  return canon<T, 0>(fnv_offset);
}
} // namespace detail

template <typename C, typename E>
void deterministic_insert(C& container, E&& element) {
  if constexpr (requires { container.push_back(std::forward<E>(element)); })
    container.push_back(std::forward<E>(element));
  else
    container.insert(std::forward<E>(element));
}

template <typename T>
void serialize(writer& output, const T& value);
template <typename T>
void deserialize(reader& input, T& value);

namespace detail {
template <typename V, std::size_t I = 0>
void variant_set(reader& input, V& value, const std::size_t index) {
  if constexpr (I < std::variant_size_v<V>) {
    if (index == I) {
      std::variant_alternative_t<I, V> alternative{};
      deserialize(input, alternative);
      value = std::move(alternative);
    } else
      variant_set<V, I + 1>(input, value, index);
  }
}
} // namespace detail

template <typename T>
void serialize(writer& output, const T& value) {
  if (!output.good()) return;
  using U = std::remove_cvref_t<T>;
  if constexpr (detail::adapted<U>)
    adapter<U>::write(output, value);
  else if constexpr (std::is_same_v<U, bool>)
    output.u8(value ? 1 : 0);
  else if constexpr (std::is_enum_v<U>)
    serialize(output, static_cast<std::underlying_type_t<U>>(value));
  else if constexpr (std::is_integral_v<U>) {
    if constexpr (sizeof(U) == 1)
      output.u8(std::uint8_t(value));
    else if constexpr (sizeof(U) == 2)
      output.u16(std::uint16_t(value));
    else if constexpr (sizeof(U) == 4)
      output.u32(std::uint32_t(value));
    else
      output.u64(std::uint64_t(value));
  } else if constexpr (std::is_floating_point_v<U>) {
    if constexpr (sizeof(U) == 4)
      output.f32(value);
    else
      output.f64(value);
  } else if constexpr (detail::is_std_string<U>::value) {
    output.u64(value.size());
    output.raw(value.data(), value.size());
  } else if constexpr (detail::is_std_array<U>::value) {
    for (const auto& element : value)
      serialize(output, element);
  } else if constexpr (detail::is_unique_ptr<U>::value || detail::is_shared_ptr<U>::value ||
                       detail::is_optional<U>::value) {
    output.u8(value ? 1 : 0);
    if (value) serialize(output, *value);
  } else if constexpr (detail::is_pair<U>::value) {
    serialize(output, value.first);
    serialize(output, value.second);
  } else if constexpr (detail::is_tuple<U>::value) {
    std::apply([&](const auto&... elements) {
      (serialize(output, elements), ...);
    },
               value);
  } else if constexpr (detail::is_variant<U>::value) {
    output.u32(std::uint32_t(value.index()));
    std::visit([&](const auto& element) {
      serialize(output, element);
    },
               value);
  } else if constexpr (detail::map_like<U>) {
    std::vector<const typename U::value_type*> ordered;
    ordered.reserve(value.size());
    for (const auto& pair : value)
      ordered.push_back(std::addressof(pair));
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
      return left->first < right->first;
    });
    output.u64(ordered.size());
    for (const auto* pair : ordered) {
      serialize(output, pair->first);
      serialize(output, pair->second);
    }
  } else if constexpr (detail::seq_like<U>) {
    output.u64(value.size());
    for (const auto& element : value)
      serialize(output, element);
  } else if constexpr (std::is_aggregate_v<U>) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (serialize(output, reflect::get<I>(value)), ...);
    }(std::make_index_sequence<reflect::size<U>()>());
  } else
    static_assert(sizeof(U) == 0, "serializable: unsupported non-aggregate type");
}

template <typename T>
void deserialize(reader& input, T& value) {
  using U = std::remove_cvref_t<T>;
  if (!input.enter()) return;
  struct depth_guard {
    reader& input;
    ~depth_guard() {
      input.leave();
    }
  } guard{input};
  if constexpr (detail::adapted<U>)
    adapter<U>::read(input, value);
  else if constexpr (std::is_same_v<U, bool>)
    value = input.presence();
  else if constexpr (std::is_enum_v<U>) {
    std::underlying_type_t<U> raw{};
    deserialize(input, raw);
    value = U(raw);
  } else if constexpr (std::is_integral_v<U>) {
    if constexpr (sizeof(U) == 1)
      value = U(input.u8());
    else if constexpr (sizeof(U) == 2)
      value = U(input.u16());
    else if constexpr (sizeof(U) == 4)
      value = U(input.u32());
    else
      value = U(input.u64());
  } else if constexpr (std::is_floating_point_v<U>) {
    if constexpr (sizeof(U) == 4)
      value = U(input.f32());
    else
      value = U(input.f64());
  } else if constexpr (detail::is_std_string<U>::value) {
    const std::uint64_t count = input.u64();
    if (count > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
      input.ok = false;
      return;
    }
    const auto bytes = input.take(std::size_t(count));
    if (input.ok) value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  } else if constexpr (detail::is_std_array<U>::value) {
    for (auto& element : value)
      deserialize(input, element);
  } else if constexpr (detail::is_unique_ptr<U>::value) {
    if (input.presence()) {
      value = std::make_unique<typename U::element_type>();
      deserialize(input, *value);
    } else
      value.reset();
  } else if constexpr (detail::is_shared_ptr<U>::value) {
    if (input.presence()) {
      value = std::make_shared<typename U::element_type>();
      deserialize(input, *value);
    } else
      value.reset();
  } else if constexpr (detail::is_optional<U>::value) {
    if (input.presence()) {
      value.emplace();
      deserialize(input, *value);
    } else
      value.reset();
  } else if constexpr (detail::is_pair<U>::value) {
    deserialize(input, value.first);
    deserialize(input, value.second);
  } else if constexpr (detail::is_tuple<U>::value) {
    std::apply([&](auto&... elements) {
      (deserialize(input, elements), ...);
    },
               value);
  } else if constexpr (detail::is_variant<U>::value) {
    const std::uint32_t index = input.u32();
    if (index < std::variant_size_v<U>)
      detail::variant_set<U>(input, value, index);
    else
      input.ok = false;
  } else if constexpr (detail::map_like<U>) {
    const std::uint64_t count = input.u64();
    if (!input.count(count)) return;
    value.clear();
    for (std::uint64_t i = 0; i < count && input.ok; ++i) {
      typename U::key_type key{};
      typename U::mapped_type element{};
      deserialize(input, key);
      deserialize(input, element);
      if (input.ok && !value.emplace(std::move(key), std::move(element)).second) input.ok = false;
    }
  } else if constexpr (detail::seq_like<U>) {
    const std::uint64_t count = input.u64();
    if (!input.count(count)) return;
    value.clear();
    for (std::uint64_t i = 0; i < count && input.ok; ++i) {
      typename U::value_type element{};
      deserialize(input, element);
      if (input.ok) deterministic_insert(value, std::move(element));
    }
  } else if constexpr (std::is_aggregate_v<U>) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (deserialize(input, reflect::get<I>(value)), ...);
    }(std::make_index_sequence<reflect::size<U>()>());
  } else
    static_assert(sizeof(U) == 0, "serializable: unsupported non-aggregate type");
}

} // namespace devils_engine::utils::serial

#endif
