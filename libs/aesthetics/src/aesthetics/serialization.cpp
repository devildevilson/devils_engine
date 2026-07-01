#include "devils_engine/aesthetics/serialization.h"

#include <algorithm>

namespace devils_engine {
namespace aesthetics {
namespace serial {

std::vector<component_registry::entry>& component_registry::table() noexcept {
  static std::vector<entry> t; // Мейерс: обходим static-init-order-fiasco
  return t;
}

uint32_t component_registry::fingerprint() noexcept {
  // свёртка по УЖЕ отсортированной таблице -> детерминирована; считается один раз.
  static const uint32_t fp = [] {
    uint32_t acc = detail::fnv_offset;
    for (const auto& e : table()) {
      acc = (acc ^ e.hash)   * detail::fnv_prime;
      acc = (acc ^ e.layout) * detail::fnv_prime;
    }
    return acc;
  }();
  return fp;
}

std::size_t estimate_size(const world* w) {
  std::size_t total = 64; // заголовок + gen_state
  for (const auto& e : component_registry::table()) total += 8 + e.est(w); // +8: hash+len блока
  return total;
}

std::vector<std::byte> dump_world(const world* w) {
  std::vector<std::byte> buf;
  buf.resize(estimate_size(w)); // ПРЕД-resize: запись = чистый memcpy, ensure() почти не срабатывает
  writer wr{ buf };
  dump_world(w, wr);
  buf.resize(wr.pos());         // усечь до фактически записанного
  return buf;
}

void dump_world(const world* w, writer& wr) {
  wr.u32(snapshot_magic);
  wr.u32(component_registry::fingerprint());

  const auto st = w->save_state();
  wr.u64(uint64_t(st.cur_index));
  wr.u64(uint64_t(st.removed_entities.size()));
  for (const auto id : st.removed_entities) wr.u32(id);

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

bool load_world(world* w, reader& r) {
  const uint32_t magic = r.u32();
  const uint32_t fp = r.u32();
  // несовпадение -> восстановимо (чужой/старый/битый сейв): warn + false, не исключение.
  if (!r.ok || magic != snapshot_magic) {
    utils::warn("bad snapshot magic: 0x{:08x} (expected 0x{:08x})", magic, snapshot_magic);
    return false;
  }
  if (fp != component_registry::fingerprint()) {
    utils::warn("snapshot schema mismatch: file 0x{:08x} vs build 0x{:08x}", fp, component_registry::fingerprint());
    return false;
  }

  world::snapshot_state st;
  st.cur_index = std::size_t(r.u64());
  const uint64_t removed = r.u64();
  st.removed_entities.reserve(removed < r.b.size() ? removed : 0); // защита от мусорного размера
  for (uint64_t i = 0; i < removed && r.ok; ++i) st.removed_entities.push_back(r.u32());
  if (!r.ok) { utils::warn("snapshot: truncated generator state"); return false; }
  w->load_state(st);

  const uint32_t block_count = r.u32();
  const auto& table = component_registry::table();
  for (uint32_t b = 0; b < block_count && r.ok; ++b) {
    const uint32_t hash = r.u32();
    const uint32_t len = r.u32();
    const std::size_t next = r.pos + len;

    const auto it = std::lower_bound(table.begin(), table.end(), hash,
      [](const component_registry::entry& e, const uint32_t v) { return e.hash < v; });
    if (it != table.end() && it->hash == hash) it->load(w, r);
    // страховка: даже при найденном типе встаём на границу блока (рассинхрон длины / unknown-тип).
    if (next <= r.b.size()) r.pos = next; else r.ok = false;
  }

  if (!r.ok) { utils::warn("snapshot: truncated / corrupt payload"); return false; }

  // мир целиком построен -> будим системы: пусть пересоберут query/кэши.
  w->emit(snapshot_loaded_event{});
  return true;
}

} // namespace serial
} // namespace aesthetics
} // namespace devils_engine
