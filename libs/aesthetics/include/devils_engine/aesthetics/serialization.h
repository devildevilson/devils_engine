#ifndef DEVILS_ENGINE_AESTHETICS_SERIALIZATION_H
#define DEVILS_ENGINE_AESTHETICS_SERIALIZATION_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "devils_engine/utils/hash.h"
#include "devils_engine/utils/serialization.h"
#include "world.h"

// ECS checkpoint projection. The canonical byte codec lives in utils::serial; this layer owns only
// world allocator state, component registration and the component-block schema. A world payload can
// therefore be embedded as one section of a larger project checkpoint without acquiring ownership
// of its buffer, compression or publication policy.

namespace devils_engine::aesthetics::serial {

using utils::serial::deserialize;
using utils::serial::in_t;
using utils::serial::out_t;
using utils::serial::reader;
using utils::serial::serialize;
using utils::serial::writer;

constexpr std::uint32_t snapshot_magic = UINT32_C(0xDE5A0001);
using snapshot_loaded_event = aesthetics::snapshot_loaded_event;

template <typename T>
std::size_t estimate_one(const world* value) {
  const auto* storage = value->get_allocator<T>();
  return storage != nullptr
           ? storage->components.size() * (sizeof(T) + sizeof(entityid_t) + 8)
           : 0;
}

template <typename T>
void dump_one(const world* value, writer& output) {
  const auto* storage = value->get_allocator<T>();
  const std::uint32_t count =
    storage != nullptr ? std::uint32_t(storage->components.size()) : 0;
  output.u32(count);
  if (storage == nullptr) return;

  const auto& sparse = storage->sparce_set;
  for (std::size_t i = 0; i < sparse.size(); ++i) {
    if (is_invalid_entityid(sparse[i])) continue;
    const entityid_t id = make_entityid(i, get_entityid_version(sparse[i]));
    const std::size_t dense = get_entityid_index(sparse[i]);
    serialize(output, id);
    serialize(output, storage->components[dense]);
  }
}

template <typename T>
void load_one(world* value, reader& input) {
  const std::uint32_t count = input.u32();
  auto* storage = value->get_or_create_allocator<T>(sizeof(T) * 250);
  std::size_t previous_index = 0;
  for (std::uint32_t i = 0; i < count && input.ok; ++i) {
    entityid_t id = invalid_entityid;
    T component{};
    deserialize(input, id);
    deserialize(input, component);
    const auto index = get_entityid_index(id);
    if (!input.good() || is_invalid_entityid(id) || index >= value->index_capacity() ||
        (i != 0 && index <= previous_index)) {
      input.ok = false;
      return;
    }
    previous_index = index;
    if (auto* slot = storage->create_comp(id)) *slot = std::move(component);
    else input.ok = false;
  }
}

class component_registry {
public:
  using dump_fn = void (*)(const world*, writer&);
  using load_fn = void (*)(world*, reader&);
  using size_fn = std::size_t (*)(const world*);

  struct entry {
    std::uint32_t hash;
    std::uint32_t layout;
    std::string_view name;
    dump_fn dump;
    load_fn load;
    size_fn est;
  };

  static const std::vector<entry>& table() noexcept;
  static bool frozen() noexcept;
  static void freeze() noexcept;

  template <typename T>
  static bool add() {
    static_assert(std::is_aggregate_v<T>,
                  "serializable component must be an aggregate (no user-declared constructors)");
    const std::string_view name = utils::type_name<T>();
    const std::uint32_t hash = utils::murmur_hash3_32(name);
    if (frozen()) utils::error{}("serializable component '{}' registered after component schema freeze", name);

    auto& entries = mutable_table();
    const auto position = std::lower_bound(
      entries.begin(), entries.end(), hash,
      [](const entry& current, const std::uint32_t key) { return current.hash < key; });
    if (position != entries.end() && position->hash == hash) {
      utils::error{}("component hash collision (murmur32=0x{:08x}): '{}' vs '{}'",
                     hash, position->name, name);
    }

    entries.insert(position,
                   entry{hash, utils::serial::detail::layout_hash<T>(), name,
                         &dump_one<T>, &load_one<T>, &estimate_one<T>});
    aesthetics::component_type_id<T>();
    return true;
  }

  static std::uint32_t fingerprint() noexcept;

private:
  static std::vector<entry>& mutable_table() noexcept;
};

#define SERIALIZABLE_COMPONENT(T) \
  inline const bool _serreg_##T = \
    ::devils_engine::aesthetics::serial::component_registry::add<T>();

std::size_t estimate_size(const world* value);
void dump_world(const world* value, writer& output);
std::vector<std::byte> dump_world(const world* value);
std::optional<world> stage_world(reader& input);
bool load_world(world* value, reader& input);

} // namespace devils_engine::aesthetics::serial

#endif
