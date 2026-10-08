#ifndef FRONTIER_ONLINE_NET_PLAYER_INTENT_WIRE_H
#define FRONTIER_ONLINE_NET_PLAYER_INTENT_WIRE_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <devils_engine/act/intent.h>
#include <devils_engine/network/fixed_point.h>
#include <devils_engine/network/intent_wire.h>
#include <devils_engine/utils/string_id.h>

// One project seam for the first external action. The wire proposal contains
// neither actor ID nor provenance; the authority derives both after checking
// the authenticated logical peer. Cell bases are supplied by that session,
// never accepted from a client packet.
namespace frontier_online::net {

namespace network = devils_engine::network;

inline constexpr uint8_t spawn_food_kind = 1;
inline constexpr network::fixed_axis<uint16_t> player_intent_axis{6, 10};
static_assert(player_intent_axis.valid());
inline constexpr uint8_t player_intent_revision = 1;

// The handshake advertises the project meaning of the compact proposal. The
// revision covers the spawn_food verb and its canonical payload semantics;
// the remaining fields come from the actual lattice and layout declaration.
inline constexpr uint32_t player_intent_schema_fingerprint = [] {
  uint32_t hash = 2166136261u;
  for (const uint8_t field : std::array<uint8_t, 5>{
         player_intent_revision, spawn_food_kind, 2,
         player_intent_axis.cell_shift, player_intent_axis.fraction_bits}) {
    hash = (hash ^ field) * 16777619u;
  }
  return hash;
}();

[[nodiscard]] inline network::intent_layout_table player_intent_layouts() {
  network::intent_layout_table table;
  const std::array declaration{network::intent_kind_layout{spawn_food_kind, {2, false, false, false}}};
  (void)table.assign(declaration);
  return table;
}

[[nodiscard]] inline bool encode_player_intent(
  const devils_engine::act::intent& action, const uint16_t sequence_low,
  const uint8_t tick_delta, const std::array<int32_t, 2> base_cells,
  network::intent& output) noexcept {
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
    const auto delta = network::relative_cell(base_cells[axis], split.key);
    if (delta.out_of_range) return false;
    staged.cell_delta[axis] = delta.value;
    staged.code[axis] = uint16_t(split.code);
  }
  output = staged;
  return true;
}

[[nodiscard]] inline bool decode_player_intent(
  const network::intent& proposal, const std::array<int32_t, 2> base_cells,
  devils_engine::act::intent& output) noexcept {
  if (proposal.kind != spawn_food_kind || proposal.target != 0 || proposal.reference != 0 ||
      proposal.turn != 0 || proposal.code[2] != 0 || proposal.cell_delta[2] != 0) return false;
  const int32_t key_x = network::absolute_cell(base_cells[0], proposal.cell_delta[0]);
  const int32_t key_y = network::absolute_cell(base_cells[1], proposal.cell_delta[1]);
  if (network::relative_cell(base_cells[0], key_x).value != proposal.cell_delta[0] ||
      network::relative_cell(base_cells[1], key_y).value != proposal.cell_delta[1]) return false;
  const double x = network::join_axis(player_intent_axis, key_x, proposal.code[0]);
  const double y = network::join_axis(player_intent_axis, key_y, proposal.code[1]);
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
