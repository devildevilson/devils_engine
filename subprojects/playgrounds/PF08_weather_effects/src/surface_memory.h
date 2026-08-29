#ifndef DEVILS_ENGINE_PF08_SURFACE_MEMORY_H
#define DEVILS_ENGINE_PF08_SURFACE_MEMORY_H

namespace devils_engine::pf08 {

// Host-настройки дешёвой GPU world-map. Самой истории на CPU нет: единственный источник истины —
// per-frame buffer surface_precipitation_memory и его предыдущая копия.
struct surface_memory_settings {
  // Осадки продолжаются при паузе небесной механики: pause останавливает календарь, не погоду.
  double world_seconds_per_real_second = 60.0;
  double snow_melt_mm_h = 0.8;
  double dry_half_life_hours = 0.35;
};

bool valid_surface_memory_settings(const surface_memory_settings& settings);

} // namespace devils_engine::pf08

#endif
