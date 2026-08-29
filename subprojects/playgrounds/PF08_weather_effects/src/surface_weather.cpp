#include "surface_weather.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devils_engine::pf08 {

bool valid_surface_weather_settings(const surface_weather_settings& s) {
  return std::isfinite(s.world_seconds_per_real_second) && s.world_seconds_per_real_second >= 0.0 &&
         std::isfinite(s.snow_depth_per_water_depth) && s.snow_depth_per_water_depth > 0.0 &&
         std::isfinite(s.max_snow_depth_m) && s.max_snow_depth_m >= 0.0 &&
         std::isfinite(s.snow_cover_depth_m) && s.snow_cover_depth_m > 0.0 &&
         std::isfinite(s.snow_melt_mm_h) && s.snow_melt_mm_h >= 0.0 &&
         std::isfinite(s.rain_melt_rate_scale) && s.rain_melt_rate_scale >= 0.0 &&
         std::isfinite(s.wet_saturation_mm) && s.wet_saturation_mm > 0.0 &&
         std::isfinite(s.dry_half_life_hours) && s.dry_half_life_hours > 0.0;
}

void advance_surface_weather(surface_weather_state& state, const weather_state& weather,
                             const surface_weather_settings& settings, const double real_seconds) {
  if (!valid_surface_weather_settings(settings) || !std::isfinite(real_seconds) || real_seconds <= 0.0) return;
  const double world_hours = real_seconds * settings.world_seconds_per_real_second / 3600.0;
  if (world_hours <= 0.0) return;

  const double maximum_water_mm = settings.max_snow_depth_m * 1000.0 /
                                  settings.snow_depth_per_water_depth;
  const double snowfall_mm = std::max(weather.snow_rate_mm_h, 0.0) * world_hours;
  state.snow_water_mm = std::clamp(state.snow_water_mm + snowfall_mm, 0.0, maximum_water_mm);

  double melt_mm = 0.0;
  if (weather.snow_rate_mm_h <= 1e-9 && state.snow_water_mm > 0.0) {
    const double melt_rate = settings.snow_melt_mm_h *
                             (1.0 + std::max(weather.rain_rate_mm_h, 0.0) *
                                      settings.rain_melt_rate_scale);
    melt_mm = std::min(state.snow_water_mm, melt_rate * world_hours);
    state.snow_water_mm -= melt_mm;
  }

  const double liquid_mm = std::max(weather.rain_rate_mm_h, 0.0) * world_hours + melt_mm;
  state.wetness = std::clamp(state.wetness, 0.0, 1.0);
  if (liquid_mm > 0.0) {
    // Последовательные порции воды композиционны: две по 0.1 мм дают тот же результат, что одна
    // 0.2 мм. Линейный clamp этим свойством не обладает и зависит от частоты кадров.
    state.wetness = 1.0 - (1.0 - state.wetness) *
                            std::exp(-liquid_mm / settings.wet_saturation_mm);
  } else {
    state.wetness *= std::exp(-std::numbers::ln2 * world_hours /
                              settings.dry_half_life_hours);
  }
}

surface_weather_sample sample_surface_weather(const surface_weather_state& state,
                                               const surface_weather_settings& settings) {
  if (!valid_surface_weather_settings(settings)) return {};
  const double depth = std::clamp(state.snow_water_mm, 0.0,
                                  settings.max_snow_depth_m * 1000.0 /
                                    settings.snow_depth_per_water_depth) *
                       settings.snow_depth_per_water_depth * 0.001;
  const double coverage = depth <= 0.0 ? 0.0 : 1.0 - std::exp(-depth / settings.snow_cover_depth_m);
  return {depth, std::clamp(coverage, 0.0, 1.0), std::clamp(state.wetness, 0.0, 1.0)};
}

} // namespace devils_engine::pf08
