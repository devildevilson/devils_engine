// Общие rain/snow LOD и shelter-величины для particle, froxel и surface consumers.

#ifndef PF08_PRECIPITATION_GLSL
#define PF08_PRECIPITATION_GLSL

#include "pf08_records.glsl"

bool pf08_rain_active(const pf08_sky_block sky) {
  return sky.precipitation_params.x > 0.0;
}

bool pf08_snow_active(const pf08_sky_block sky) {
  return sky.snow_params.x > 0.0;
}

float pf08_precipitation_far_weight(const float distance_m, const float start, const float radius) {
  return smoothstep(max(start, 0.0), max(start, 0.0) + max(radius * 0.45, 1.0), distance_m);
}

float pf08_rain_far_weight(const pf08_sky_block sky, const float view_distance_m) {
  return pf08_precipitation_far_weight(view_distance_m, sky.precipitation_shape.z,
                                       sky.precipitation_params.w);
}

float pf08_snow_far_weight(const pf08_sky_block sky, const float view_distance_m) {
  return pf08_precipitation_far_weight(view_distance_m, sky.snow_shape.z, sky.snow_params.w);
}

float pf08_precipitation_vertical_density(const pf08_sky_block sky, const float world_height_m) {
  // Нижнюю границу даёт scene depth, а не y=0: долина уходит ниже касательной плоскости, и step(0,y)
  // оставил бы над ней сухую невидимую полость. Сверху дождь гаснет у основания облака.
  const float cloud_base = max(sky.cloud_shape.x, 1.0);
  return 1.0 - smoothstep(cloud_base * 0.82, cloud_base, world_height_m);
}

vec3 pf08_precipitation_velocity(const pf08_sky_block sky, const bool snow) {
  const float fall = snow ? sky.snow_params.y : sky.precipitation_params.y;
  const float wind = snow ? sky.snow_params.z : sky.precipitation_params.z;
  return vec3(sky.wind_params.x * wind, -max(fall, 0.1), sky.wind_params.y * wind);
}

bool pf08_shelter_enabled(const pf08_sky_block sky) {
  return sky.shelter_minimum.w > 0.5 && sky.shelter_maximum.w > 0.5;
}

bool pf08_ray_box(const vec3 origin, const vec3 direction, const vec3 minimum,
                  const vec3 maximum, out float hit_t) {
  const vec3 safe_direction = vec3(
    abs(direction.x) < 1e-6 ? (direction.x < 0.0 ? -1e-6 : 1e-6) : direction.x,
    abs(direction.y) < 1e-6 ? (direction.y < 0.0 ? -1e-6 : 1e-6) : direction.y,
    abs(direction.z) < 1e-6 ? (direction.z < 0.0 ? -1e-6 : 1e-6) : direction.z);
  const vec3 a = (minimum - origin) / safe_direction;
  const vec3 b = (maximum - origin) / safe_direction;
  const vec3 near_axis = min(a, b);
  const vec3 far_axis = max(a, b);
  const float near_t = max(max(near_axis.x, near_axis.y), near_axis.z);
  const float far_t = min(min(far_axis.x, far_axis.y), far_axis.z);
  hit_t = max(near_t, 0.0);
  return far_t >= hit_t;
}

bool pf08_shelter_blocks_precipitation(const pf08_sky_block sky, const vec3 position,
                                       const vec3 falling_velocity) {
  if (!pf08_shelter_enabled(sky) || falling_velocity.y >= -1e-5) return false;
  // Крыша — горизонтальная коробка. Для миллионов froxel samples достаточно пересечь её нижнюю
  // плоскость: луч идёт ВВЕРХ против падения, поэтому после неё он либо вошёл в крышу, либо уже
  // никогда не войдёт. Это тот же AABB-ответ без шести делений на каждый sample.
  // Точка на верхней стороне крыши открыта осадкам; epsilon не даёт поверхности заслонить саму себя.
  if (position.y >= sky.shelter_maximum.y - 1e-3) return false;
  const vec3 upward = -falling_velocity;
  const float hit_t = max((sky.shelter_minimum.y - position.y) / max(upward.y, 1e-5), 0.0);
  const vec2 hit_xz = position.xz + upward.xz * hit_t;
  return all(greaterThanEqual(hit_xz, sky.shelter_minimum.xz)) &&
         all(lessThanEqual(hit_xz, sky.shelter_maximum.xz));
}

bool pf08_shelter_segment_contact(const pf08_sky_block sky, const vec3 previous_position,
                                  const vec3 candidate_position, out vec3 hit_position) {
  if (!pf08_shelter_enabled(sky)) return false;
  const vec3 segment = candidate_position - previous_position;
  const float length_m = length(segment);
  if (length_m <= 1e-6) return false;
  float hit_t;
  if (!pf08_ray_box(previous_position, segment / length_m, sky.shelter_minimum.xyz,
                    sky.shelter_maximum.xyz, hit_t) || hit_t > length_m) return false;
  hit_position = previous_position + segment / length_m * hit_t;
  return true;
}

float pf08_precipitation_light_transmittance(const pf08_sky_block sky, const vec3 world_position,
                                             const vec3 light_direction) {
  const float extinction = (pf08_rain_active(sky) ? max(sky.precipitation_shape.y, 0.0) : 0.0) +
                           (pf08_snow_active(sky) ? max(sky.snow_shape.y, 0.0) : 0.0);
  if (extinction <= 0.0) return 1.0;
  if (light_direction.y <= 0.0) return 0.0;
  const float cloud_base = max(sky.cloud_shape.x, 1.0);
  const float vertical_column = max(cloud_base - world_position.y, 0.0);
  return exp(-extinction * vertical_column / max(light_direction.y, 1e-4));
}

#endif
