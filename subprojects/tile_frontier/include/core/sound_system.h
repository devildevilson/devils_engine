#ifndef TILE_FRONTIER_CORE_SOUND_SYSTEM_H
#define TILE_FRONTIER_CORE_SOUND_SYSTEM_H

#include <cstddef>

#include <devils_engine/simul/standard_sound_system.h>

namespace tile_frontier {
namespace core {

struct broker;

class sound_simulation : public devils_engine::simul::standard_sound_system<broker> {
public:
  using devils_engine::simul::standard_sound_system<::tile_frontier::core::broker>::standard_sound_system;
};

}
}

#endif
