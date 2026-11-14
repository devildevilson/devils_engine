#ifndef FRONTIER_ONLINE_NET_PLAYER_INTENT_WIRE_H
#define FRONTIER_ONLINE_NET_PLAYER_INTENT_WIRE_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include <devils_engine/act/intent.h>
#include <devils_engine/network/fixed_point.h>
#include <devils_engine/network/intent_wire.h>
#include <devils_engine/utils/hash.h>
#include <devils_engine/utils/string_id.h>

#include "net/canonical_action.h"

// One project seam for the first external action. The wire proposal contains
// neither actor ID nor provenance; the authority derives both after checking
// the authenticated logical peer.
//
// The cell of the target point travels as an ABSOLUTE int32 key (author decision
// 2026-11-14). A delta from a shared base cell is cheaper, but until a player
// controller per participant exists there is no base both sides agree on, and a
// mismatched base silently moves the action by whole cells.
namespace frontier_online::net {

namespace network = devils_engine::network;

inline constexpr uint8_t spawn_food_kind = 1;
inline constexpr network::fixed_axis<uint16_t> player_intent_axis{6, 10};
static_assert(player_intent_axis.valid());
inline constexpr network::intent_field_layout spawn_food_layout{2, false, false, false, true};
// Revision of the spawn_food verb and its upstream semantics. 2: absolute cells.
inline constexpr uint8_t player_intent_revision = 2;

// The handshake advertises the project meaning of BOTH directions of the hot
// path: the compact upstream proposal (layout, lattice) and the canonical
// record the authority writes into each tick. A change to either without a
// revision bump would let two builds agree in the handshake and then read one
// bundle in two ways.
inline constexpr uint32_t player_intent_schema_fingerprint = [] {
  const std::array<std::byte, 9> bytes{
    std::byte{player_intent_revision},
    std::byte{spawn_food_kind},
    std::byte{spawn_food_layout.axes},
    std::byte{uint8_t(spawn_food_layout.absolute_cells)},
    std::byte{player_intent_axis.cell_shift},
    std::byte{player_intent_axis.fraction_bits},
    std::byte{canonical_action_revision},
    std::byte{canonical_action_spawn_food},
    std::byte{uint8_t(canonical_food_action_bytes)}};
  return devils_engine::utils::murmur_hash3_32(std::span<const std::byte>(bytes));
}();

[[nodiscard]] inline network::intent_layout_table player_intent_layouts() {
  network::intent_layout_table table;
  const std::array declaration{network::intent_kind_layout{spawn_food_kind, spawn_food_layout}};
  (void)table.assign(declaration);
  return table;
}

[[nodiscard]] inline bool encode_player_intent(
  const devils_engine::act::intent& action, const uint16_t sequence_low,
  const uint8_t tick_delta, network::intent& output) noexcept {
  if (action.kind != devils_engine::act::intent_kind::spawn_prefab ||
      action.payload.spawn.prefab != devils_engine::utils::string_hash("food") ||
      action.payload.spawn.target.z != 0.0 ||
      tick_delta > network::intent_tick_delta_limit) return false;
  network::intent staged;
  staged.kind = spawn_food_kind;
  staged.sequence_low = sequence_low;
  staged.tick_delta = tick_delta;
  for (size_t axis = 0; axis < 2; ++axis) {
    const double coordinate = axis == 0 ? action.payload.spawn.target.x
                                        : action.payload.spawn.target.y;
    const auto split = network::split_axis(player_intent_axis, coordinate);
    if (split.clamped) return false;
    staged.cell_key[axis] = split.key;
    staged.code[axis] = uint16_t(split.code);
  }
  output = staged;
  return true;
}

[[nodiscard]] inline bool decode_player_intent(const network::intent& proposal,
                                               devils_engine::act::intent& output) noexcept {
  if (proposal.kind != spawn_food_kind || proposal.target != 0 || proposal.reference != 0 ||
      proposal.turn != 0 || proposal.code[2] != 0 || proposal.cell_key[2] != 0 ||
      proposal.cell_delta != std::array<int8_t, network::intent_max_axes>{}) return false;
  const double x = network::join_axis(player_intent_axis, proposal.cell_key[0], proposal.code[0]);
  const double y = network::join_axis(player_intent_axis, proposal.cell_key[1], proposal.code[1]);
  if (!std::isfinite(x) || !std::isfinite(y)) return false;
  devils_engine::act::intent staged;
  staged.kind = devils_engine::act::intent_kind::spawn_prefab;
  staged.payload.spawn.prefab = devils_engine::utils::string_hash("food");
  staged.payload.spawn.target = devils_engine::act::vec3{x, y, 0.0};
  staged.source_action = devils_engine::utils::string_hash("network_spawn_food");
  output = staged;
  return true;
}

} // namespace frontier_online::net

#endif
