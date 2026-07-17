#ifndef TILE_FRONTIER_CORE_BRAIN_CONFIG_LOADER_H
#define TILE_FRONTIER_CORE_BRAIN_CONFIG_LOADER_H

#include <string_view>

#include "actor_simulation.h"

namespace devils_engine {
namespace demiurg {
class resource_system;
}
}

namespace tile_frontier {
namespace core {

brain_config load_required_brain_config(
  devils_engine::demiurg::resource_system& resources,
  std::string_view script_id,
  std::string_view fsm_id,
  std::string_view goap_id,
  std::string_view prefab_prefix);

} // namespace core
} // namespace tile_frontier

#endif
