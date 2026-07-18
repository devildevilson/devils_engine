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

// Собирает multi-brain конфиг: script_id — предикат actor.is_hungry; fsm_prefix/goap_prefix —
// префиксы наборов ресурсов (обычно "fsm"/"goap"): КАЖДЫЙ ресурс набора = отдельный именованный
// мозг (имя = id без префикса), на который префабы ссылаются строками goap=/fsm= (per-entity refs).
brain_config load_required_brain_config(
  devils_engine::demiurg::resource_system& resources,
  std::string_view script_id,
  std::string_view fsm_prefix,
  std::string_view goap_prefix,
  std::string_view prefab_prefix);

} // namespace core
} // namespace tile_frontier

#endif
