#ifndef FRONTIER_ONLINE_NET_CANONICAL_ACTION_H
#define FRONTIER_ONLINE_NET_CANONICAL_ACTION_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <devils_engine/utils/serialization.h>
#include <devils_engine/utils/string_id.h>

// Payload канонической записи — то, что авторитет РЕШИЛ, в том виде, в каком его исполняют все.
//
// Едет внутри `canonical_tick` (библиотека владеет конвертом тика, проект — этими байтами).
// Координаты здесь уже деквантованы авторитетом: все ведомые исполняют ОДНО прочтение
// предложения, и спорить о нём им не о чем. Ревизия входит в отпечаток проектной схемы
// (`player_intent_schema_fingerprint`), поэтому изменение этих байтов без её роста — дефект.
//
// Header-only и без транспорта: тот же кодек исполняют авторитет, ведомые и проверки
// причинного шага, которые с сетью не линкуются.

namespace frontier_online::net {

inline constexpr uint8_t canonical_action_revision = 2;
inline constexpr uint8_t canonical_action_spawn_food = 1;
inline constexpr uint8_t canonical_action_peer_joined = 2;
inline constexpr size_t canonical_food_action_bytes = 1 + 8 + 8 + 8;
inline constexpr size_t canonical_join_action_bytes = 1;

// Присоединение — решение авторитета, а не предложение участника, но едет тем же конвертом:
// origin_peer = присоединившийся, input_sequence = этот зарезервированный номер. Участник до него
// не доберётся (полный номер расширяется из 16 бит от единицы), поэтому (peer, sequence) остаётся
// уникальной парой, и canonical_tick_wire не нужно знать о записях авторитета. В тике присоединения
// других записей этого peer нет: авторитет принимает его предложения только на тики после него.
inline constexpr uint64_t canonical_join_sequence = UINT64_MAX;

enum class canonical_action_kind : uint8_t { invalid, spawn_food, peer_joined };

// Род записи по первому байту и (для присоединения) по зарезервированному номеру: присоединение с
// обычным номером или пища с номером присоединения — invalid.
[[nodiscard]] inline canonical_action_kind classify(const std::span<const std::byte> bytes,
                                                    const uint64_t input_sequence) noexcept {
  if (bytes.empty()) return canonical_action_kind::invalid;
  const uint8_t kind = uint8_t(bytes.front());
  if (kind == canonical_action_peer_joined && bytes.size() == canonical_join_action_bytes &&
      input_sequence == canonical_join_sequence)
    return canonical_action_kind::peer_joined;
  if (kind == canonical_action_spawn_food && input_sequence != canonical_join_sequence)
    return canonical_action_kind::spawn_food;
  return canonical_action_kind::invalid;
}

[[nodiscard]] inline std::vector<std::byte> encode_join() {
  return {std::byte{canonical_action_peer_joined}};
}

struct canonical_food_action {
  double x = 0;
  double y = 0;
};

// Отказ на нефинитной координате: такое решение авторитет принять не мог.
[[nodiscard]] inline bool encode(const canonical_food_action& value, std::vector<std::byte>& out) {
  out.clear();
  if (!std::isfinite(value.x) || !std::isfinite(value.y)) return false;
  out.reserve(canonical_food_action_bytes);
  devils_engine::utils::serial::writer w(out, false);
  w.u8(canonical_action_spawn_food);
  w.u64(devils_engine::utils::string_hash("food"));
  w.f64(value.x);
  w.f64(value.y);
  return w.good() && out.size() == canonical_food_action_bytes;
}

[[nodiscard]] inline bool decode(const std::span<const std::byte> bytes, canonical_food_action& out) {
  devils_engine::utils::serial::reader r(bytes);
  const uint8_t kind = r.u8();
  const uint64_t prefab = r.u64();
  canonical_food_action value;
  value.x = r.f64();
  value.y = r.f64();
  if (!r.good() || r.position() != r.size() || kind != canonical_action_spawn_food ||
      prefab != devils_engine::utils::string_hash("food") || !std::isfinite(value.x) ||
      !std::isfinite(value.y))
    return false;
  out = value;
  return true;
}

} // namespace frontier_online::net

#endif
