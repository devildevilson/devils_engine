#include "surface_memory.h"

#include <cmath>

namespace devils_engine::pf08 {

bool valid_surface_memory_settings(const surface_memory_settings& s) {
  return std::isfinite(s.world_seconds_per_real_second) && s.world_seconds_per_real_second >= 0.0 &&
         std::isfinite(s.snow_melt_mm_h) && s.snow_melt_mm_h >= 0.0 &&
         std::isfinite(s.dry_half_life_hours) && s.dry_half_life_hours > 0.0;
}

} // namespace devils_engine::pf08
