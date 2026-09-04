#include <algorithm>

#include "devils_engine/aesthetics/serialization.h"

namespace devils_engine {
namespace aesthetics {
namespace serial {

namespace {
struct component_registry_storage {
  std::vector<component_registry::entry> table;
  bool frozen = false;
};

component_registry_storage& registry_storage() noexcept {
  static component_registry_storage value;
  return value;
}
} // namespace

const std::vector<component_registry::entry>& component_registry::table() noexcept {
  return registry_storage().table;
}

std::vector<component_registry::entry>& component_registry::mutable_table() noexcept {
  return registry_storage().table;
}

bool component_registry::frozen() noexcept {
  return registry_storage().frozen;
}

void component_registry::freeze() noexcept {
  registry_storage().frozen = true;
}

uint32_t component_registry::fingerprint() noexcept {
  // свёртка по УЖЕ отсортированной таблице -> детерминирована; считается один раз.
  freeze();
  static const uint32_t fp = [] {
    uint32_t acc = detail::fnv_offset;
    for (const auto& e : table()) {
      acc = (acc ^ e.hash) * detail::fnv_prime;
      acc = (acc ^ e.layout) * detail::fnv_prime;
    }
    return acc;
  }();
  return fp;
}

std::size_t estimate_size(const world* w) {
  component_registry::freeze();
  std::size_t total = 64; // заголовок + gen_state
  for (const auto& e : component_registry::table()) {
    total += 8 + e.est(w); // +8: hash+len блока
  }
  return total;
}

std::vector<std::byte> dump_world(const world* w) {
  std::vector<std::byte> buf;
  buf.resize(estimate_size(w)); // ПРЕД-resize: запись = чистый memcpy, ensure() почти не срабатывает
  writer wr{buf};
  dump_world(w, wr);
  buf.resize(wr.pos()); // усечь до фактически записанного
  return buf;
}

void dump_world(const world* w, writer& wr) {
  wr.u32(snapshot_magic);
  wr.u32(component_registry::fingerprint());

  const auto st = w->save_state();
  wr.u64(uint64_t(st.cur_index));
  wr.u64(uint64_t(st.removed_entities.size()));
  for (const auto id : st.removed_entities) {
    wr.u32(id);
  }

  const auto& table = component_registry::table();
  wr.u32(uint32_t(table.size()));

  for (const auto& e : table) {
    wr.u32(e.hash);
    const std::size_t len_slot = wr.pos(); // резерв под byte_len
    wr.u32(0);
    const std::size_t body_start = wr.pos();

    e.dump(w, wr); // payload

    wr.patch_u32(len_slot, uint32_t(wr.pos() - body_start)); // бэкпатч длины
  }
}

std::optional<world> stage_world(reader& r) {
  world staged;
  const uint32_t magic = r.u32();
  const uint32_t fp = r.u32();
  // несовпадение -> восстановимо (чужой/старый/битый сейв): warn + false, не исключение.
  if (!r.ok || magic != snapshot_magic) {
    utils::warn("bad snapshot magic: 0x{:08x} (expected 0x{:08x})", magic, snapshot_magic);
    return std::nullopt;
  }
  if (fp != component_registry::fingerprint()) {
    utils::warn("snapshot schema mismatch: file 0x{:08x} vs build 0x{:08x}", fp, component_registry::fingerprint());
    return std::nullopt;
  }

  world::snapshot_state st;
  st.cur_index = std::size_t(r.u64());
  const uint64_t removed = r.u64();
  st.removed_entities.reserve(removed < r.b.size() ? removed : 0); // защита от мусорного размера
  for (uint64_t i = 0; i < removed && r.ok; ++i) {
    st.removed_entities.push_back(r.u32());
  }
  if (!r.ok) {
    utils::warn("snapshot: truncated generator state");
    return std::nullopt;
  }
  staged.load_state(st);

  const uint32_t block_count = r.u32();
  const auto& table = component_registry::table();
  if (!r.ok || block_count != table.size()) {
    utils::warn("snapshot: component block count {} does not match schema {}", block_count, table.size());
    return std::nullopt;
  }
  for (uint32_t b = 0; b < block_count && r.ok; ++b) {
    const uint32_t hash = r.u32();
    const uint32_t len = r.u32();
    if (!r.ok || hash != table[b].hash) {
      utils::warn("snapshot: component block {} has non-canonical or unexpected id 0x{:08x}", b, hash);
      return std::nullopt;
    }
    if (len > r.b.size() - r.pos) {
      utils::warn("snapshot: truncated component block 0x{:08x}", hash);
      return std::nullopt;
    }
    const std::size_t next = r.pos + len;
    table[b].load(&staged, r);
    if (!r.ok || r.pos != next) {
      utils::warn("snapshot: component block 0x{:08x} did not consume its declared payload", hash);
      return std::nullopt;
    }
  }

  return staged;
}

bool load_world(world* w, reader& r) {
  auto staged = stage_world(r);
  if (!staged.has_value()) {
    return false;
  }

  w->replace_state(std::move(*staged));
  return true;
}

} // namespace serial
} // namespace aesthetics
} // namespace devils_engine
