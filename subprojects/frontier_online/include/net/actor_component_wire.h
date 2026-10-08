#ifndef FRONTIER_ONLINE_NET_ACTOR_COMPONENT_WIRE_H
#define FRONTIER_ONLINE_NET_ACTOR_COMPONENT_WIRE_H

#include <cmath>

#include <devils_engine/network/component_wire.h>

#include "core/actor_simulation.h"
#include "core/glm_serial.h"

namespace devils_engine::network {

// The generic codec knows the bytes of glm::vec2 through glm_serial.h;
// only the project's semantic constraint needs a specialization.
template <> struct component_wire_validation<frontier_online::core::actor_position> {
  [[nodiscard]] static bool valid(const frontier_online::core::actor_position& value) noexcept {
    return std::isfinite(value.value.x) && std::isfinite(value.value.y);
  }
};

} // namespace devils_engine::network

namespace frontier_online::net {

[[nodiscard]] inline devils_engine::network::component_wire_status declare_actor_components(
  devils_engine::network::component_wire_table& table) {
  namespace network = devils_engine::network;
  auto status = table.add<core::actor_position>();
  if (status != network::component_wire_status::ok) return status;
  return table.add<core::actor_state>();
}

} // namespace frontier_online::net

#endif
