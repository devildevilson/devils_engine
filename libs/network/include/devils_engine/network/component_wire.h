#ifndef DEVILS_ENGINE_NETWORK_COMPONENT_WIRE_H
#define DEVILS_ENGINE_NETWORK_COMPONENT_WIRE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/utils/serialization.h>
#include <devils_engine/utils/type_traits.h>

// Declaring a component's wire representation is independent of registering it
// in an ECS checkpoint or choosing a delivery stream. The default complete-value
// codec is utils::serial, which owns canonical byte order and aggregate traversal.
namespace devils_engine::network {

inline constexpr uint32_t component_value_default_max_bytes = 1024;
inline constexpr uint32_t component_value_absolute_max_bytes = 64 * 1024;

template <class T>
[[nodiscard]] consteval uint32_t component_default_max_bytes() noexcept {
  static_assert(sizeof(T) <= UINT32_MAX, "component exceeds the wire size field");
  // A cheap default for plain fixed values. Custom adapters that emit more
  // than sizeof(T), and variable-sized components, declare a larger budget.
  if constexpr (std::is_trivially_copyable_v<T>) return uint32_t(sizeof(T));
  else return component_value_default_max_bytes;
}

struct component_wire_options {
  uint32_t max_value_bytes = 0; // derive the ordinary bound from T
};

// The usual identity is the fully qualified C++ type name. A project may
// specialize this trait for a long-lived external name, or bump revision if a
// custom serial adapter changes bytes without changing its declared layout.
template <class T> struct component_wire_identity {
  static constexpr std::string_view name = utils::type_name<T>();
  static constexpr uint32_t revision = 0;
};

// Most types need no validator. Semantic checks such as finite coordinates
// belong to the project and run before encoding and after decoding.
template <class T> struct component_wire_validation {
  [[nodiscard]] static bool valid(const T&) noexcept { return true; }
};

struct component_wire_descriptor {
  uint64_t id = 0;
  uint32_t layout_hash = 0;
  uint32_t revision = 0;
  uint32_t max_value_bytes = 0;

  bool operator==(const component_wire_descriptor&) const = default;
};

namespace detail {

[[nodiscard]] constexpr uint64_t component_name_hash(std::string_view name) noexcept {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (const char ch : name) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

} // namespace detail

template <class T>
[[nodiscard]] constexpr component_wire_descriptor component_descriptor(
  component_wire_options options = {}) noexcept {
  return {detail::component_name_hash(component_wire_identity<T>::name),
          utils::serial::detail::layout_hash<T>(), component_wire_identity<T>::revision,
          options.max_value_bytes == 0 ? component_default_max_bytes<T>() : options.max_value_bytes};
}

// Reuse the caller's vector capacity across values. Invalid project values
// leave it untouched; a size failure clears it and refuses the value.
template <class T>
[[nodiscard]] bool encode_component_value(const T& value, std::vector<std::byte>& output,
                                          uint32_t max_bytes = component_default_max_bytes<T>()) {
  if (max_bytes == 0 || max_bytes > component_value_absolute_max_bytes ||
      !component_wire_validation<T>::valid(value)) return false;
  output.clear();
  output.reserve(max_bytes);
  utils::serial::writer writer(output, false);
  utils::serial::serialize(writer, value);
  if (!writer.good() || output.size() > max_bytes) {
    output.clear();
    return false;
  }
  return true;
}

template <class T>
[[nodiscard]] bool decode_component_value(std::span<const std::byte> bytes, T& output,
                                          uint32_t max_bytes = component_default_max_bytes<T>()) {
  if (max_bytes == 0 || max_bytes > component_value_absolute_max_bytes ||
      bytes.size() > max_bytes) return false;
  T staged{};
  // Even a zero-byte element consumes the declared value budget. A network
  // value must not inherit the checkpoint reader's much larger default.
  utils::serial::reader reader(bytes, {max_bytes, 64});
  utils::serial::deserialize(reader, staged);
  if (!reader.good() || reader.position() != reader.size() ||
      !component_wire_validation<T>::valid(staged)) return false;
  output = std::move(staged);
  return true;
}

enum class component_wire_status : uint8_t { ok, invalid_descriptor, duplicate_id, frozen };

class component_wire_table {
public:
  template <class T>
  [[nodiscard]] component_wire_status add(component_wire_options options = {}) {
    if (component_wire_identity<T>::name.empty()) return component_wire_status::invalid_descriptor;
    const auto descriptor = component_descriptor<T>(options);
    std::vector<component_wire_descriptor> next = entries_;
    next.push_back(descriptor);
    return assign(next);
  }

  // Failed assignment leaves the old table intact. Sorting removes source
  // order from the compatibility fingerprint and makes lookup deterministic.
  [[nodiscard]] component_wire_status assign(std::span<const component_wire_descriptor> input) {
    if (frozen_) return component_wire_status::frozen;
    std::vector<component_wire_descriptor> sorted(input.begin(), input.end());
    for (const auto& entry : sorted) {
      if (entry.id == 0 || entry.max_value_bytes == 0 ||
          entry.max_value_bytes > component_value_absolute_max_bytes)
        return component_wire_status::invalid_descriptor;
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
      return a.id < b.id;
    });
    for (size_t i = 1; i < sorted.size(); ++i)
      if (sorted[i - 1].id == sorted[i].id) return component_wire_status::duplicate_id;

    std::vector<std::byte> canonical;
    canonical.reserve(sorted.size() * 20);
    utils::serial::writer writer(canonical, true);
    for (const auto& entry : sorted) {
      writer.u64(entry.id);
      writer.u32(entry.layout_hash);
      writer.u32(entry.revision);
      writer.u32(entry.max_value_bytes);
    }
    entries_ = std::move(sorted);
    fingerprint_ = utils::murmur_hash3_32(std::span<const std::byte>(canonical));
    return component_wire_status::ok;
  }

  [[nodiscard]] const component_wire_descriptor* find(uint64_t id) const noexcept {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), id,
                                     [](const auto& entry, uint64_t value) {
                                       return entry.id < value;
                                     });
    return it != entries_.end() && it->id == id ? &*it : nullptr;
  }

  [[nodiscard]] std::span<const component_wire_descriptor> entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] uint32_t fingerprint() const noexcept { return fingerprint_; }
  void freeze() noexcept { frozen_ = true; }
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }

private:
  std::vector<component_wire_descriptor> entries_;
  uint32_t fingerprint_ = 0;
  bool frozen_ = false;
};

template <class T>
[[nodiscard]] bool encode_component_value(const component_wire_table& table, const T& value,
                                          std::vector<std::byte>& output) {
  const auto expected = component_descriptor<T>();
  const auto* entry = table.find(expected.id);
  return entry != nullptr && entry->layout_hash == expected.layout_hash &&
         entry->revision == expected.revision &&
         encode_component_value(value, output, entry->max_value_bytes);
}

template <class T>
[[nodiscard]] bool decode_component_value(const component_wire_table& table,
                                          std::span<const std::byte> bytes, T& output) {
  const auto expected = component_descriptor<T>();
  const auto* entry = table.find(expected.id);
  return entry != nullptr && entry->layout_hash == expected.layout_hash &&
         entry->revision == expected.revision &&
         decode_component_value(bytes, output, entry->max_value_bytes);
}

} // namespace devils_engine::network

#endif
