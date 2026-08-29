#ifndef DEVILS_ENGINE_PF08_SURFACE_WEATHER_H
#define DEVILS_ENGINE_PF08_SURFACE_WEATHER_H

#include "weather.h"

namespace devils_engine::pf08 {

// Глобальная история осадков хранит только интегральные величины. Пространственный mask — наклон,
// world-space пятна и навес — вычисляет GPU из этой истории и геометрии, поэтому он не едет с камерой.
struct surface_weather_settings {
  // Мир PF08 обычно идёт на одну игровую минуту за реальную секунду. Осадки продолжаются и при
  // паузе небесной механики: pause останавливает календарь, не дождь вокруг наблюдателя.
  double world_seconds_per_real_second = 60.0;
  double snow_depth_per_water_depth = 10.0;
  double max_snow_depth_m = 0.12;
  double snow_cover_depth_m = 0.008;
  // Пока температура не authored, это явная скорость релаксации после снегопада, а не скрытая
  // «температура по имени пресета». Дождь ускоряет её пропорционально rate.
  double snow_melt_mm_h = 0.8;
  double rain_melt_rate_scale = 0.18;
  double wet_saturation_mm = 0.20;
  double dry_half_life_hours = 0.35;
};

struct surface_weather_state {
  double snow_water_mm = 0.0;
  double wetness = 0.0;
};

struct surface_weather_sample {
  double snow_depth_m = 0.0;
  double snow_coverage = 0.0;
  double wetness = 0.0;
};

bool valid_surface_weather_settings(const surface_weather_settings& settings);
void advance_surface_weather(surface_weather_state& state, const weather_state& weather,
                             const surface_weather_settings& settings, double real_seconds);
surface_weather_sample sample_surface_weather(const surface_weather_state& state,
                                               const surface_weather_settings& settings);

} // namespace devils_engine::pf08

#endif
