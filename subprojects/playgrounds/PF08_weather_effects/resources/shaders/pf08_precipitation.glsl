// Общие rain-LOD величины для froxel volume и particle passes.

#ifndef PF08_PRECIPITATION_GLSL
#define PF08_PRECIPITATION_GLSL

#include "pf08_records.glsl"

bool pf08_rain_active(const pf08_sky_block sky) {
  return sky.precipitation_params.x > 0.0;
}

float pf08_rain_far_weight(const pf08_sky_block sky, const float view_distance_m) {
  const float start = max(sky.precipitation_shape.z, 0.0);
  const float width = max(sky.precipitation_params.w * 0.45, 1.0);
  return smoothstep(start, start + width, view_distance_m);
}

float pf08_rain_vertical_density(const pf08_sky_block sky, const float world_height_m) {
  // Нижнюю границу даёт scene depth, а не y=0: долина уходит ниже касательной плоскости, и step(0,y)
  // оставил бы над ней сухую невидимую полость. Сверху дождь гаснет у основания облака.
  const float cloud_base = max(sky.cloud_shape.x, 1.0);
  return 1.0 - smoothstep(cloud_base * 0.82, cloud_base, world_height_m);
}

float pf08_rain_light_transmittance(const pf08_sky_block sky, const vec3 world_position,
                                    const vec3 light_direction) {
  if (!pf08_rain_active(sky) || sky.precipitation_shape.y <= 0.0) return 1.0;
  if (light_direction.y <= 0.0) return 0.0;
  const float cloud_base = max(sky.cloud_shape.x, 1.0);
  const float vertical_column = max(cloud_base - world_position.y, 0.0);
  return exp(-sky.precipitation_shape.y * vertical_column / max(light_direction.y, 1e-4));
}

#endif
