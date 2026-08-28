// Общая модель облачного слоя. Её читают volume-pass и поверхность: рисунок облака и его тень
// поэтому не могут разъехаться. Слой конечен по высоте, а не является «туманом с большим base».

#ifndef PF08_CLOUDS_GLSL
#define PF08_CLOUDS_GLSL

#include "pf08_records.glsl"

float pf08_cloud_hash(const vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float pf08_cloud_value_noise(const vec2 p) {
  const vec2 cell = floor(p);
  const vec2 f = fract(p);
  const vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(pf08_cloud_hash(cell), pf08_cloud_hash(cell + vec2(1.0, 0.0)), u.x),
             mix(pf08_cloud_hash(cell + vec2(0.0, 1.0)),
                 pf08_cloud_hash(cell + vec2(1.0, 1.0)), u.x), u.y);
}

float pf08_cloud_hash(const vec3 p) {
  return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

float pf08_cloud_value_noise(const vec3 p) {
  const vec3 cell = floor(p);
  const vec3 f = fract(p);
  const vec3 u = f * f * (3.0 - 2.0 * f);
  const float z0 = mix(
    mix(pf08_cloud_hash(cell), pf08_cloud_hash(cell + vec3(1.0, 0.0, 0.0)), u.x),
    mix(pf08_cloud_hash(cell + vec3(0.0, 1.0, 0.0)),
        pf08_cloud_hash(cell + vec3(1.0, 1.0, 0.0)), u.x), u.y);
  const float z1 = mix(
    mix(pf08_cloud_hash(cell + vec3(0.0, 0.0, 1.0)),
        pf08_cloud_hash(cell + vec3(1.0, 0.0, 1.0)), u.x),
    mix(pf08_cloud_hash(cell + vec3(0.0, 1.0, 1.0)),
        pf08_cloud_hash(cell + vec3(1.0, 1.0, 1.0)), u.x), u.y);
  return mix(z0, z1, u.z);
}

vec2 pf08_cloud_advected_xz(const pf08_sky_block sky, const vec2 world_xz) {
  return world_xz - sky.wind_params.xy * max(sky.cloud_motion.y, 0.0) * sky.wind_params.w;
}

float pf08_cloud_shape_density(const pf08_sky_block sky, const vec3 world_position) {
  const float coverage = clamp(sky.cloud_params.x, 0.0, 1.0);
  if (coverage <= 0.0) return 0.0;

  const float cell_size = max(sky.cloud_motion.x, 1e-3);
  const vec2 horizontal = pf08_cloud_advected_xz(sky, world_position.xz) / cell_size;
  // Вертикальная координата сжата: слой в полкилометра должен иметь внутреннюю форму, а не быть
  // одной и той же 2D-маской, вытянутой вверх столбом.
  const vec3 p = vec3(horizontal.x, world_position.y / (cell_size * 0.42), horizontal.y);
  const float broad = pf08_cloud_value_noise(p);
  const float medium = pf08_cloud_value_noise(p * 2.03 + vec3(13.7, 5.1, -8.3));
  const float detail = pf08_cloud_value_noise(p * 4.09 + vec3(-31.2, 11.6, 19.4));
  const float field = broad * 0.60 + medium * 0.29 + detail * 0.11;
  // Coverage задаёт положение порога. Ненулевая ширина края нужна не только картинке: без неё
  // тень облака превращается в движущийся бинарный cookie.
  const float threshold = 1.0 - coverage;
  return smoothstep(threshold - 0.10, threshold + 0.10, field);
}

float pf08_cloud_horizontal_density(const pf08_sky_block sky, const vec2 world_xz) {
  return pf08_cloud_shape_density(
    sky, vec3(world_xz.x, 0.5 * (sky.cloud_shape.x + sky.cloud_shape.y), world_xz.y));
}

float pf08_cloud_vertical_density(const pf08_sky_block sky, const float world_height_m) {
  const float base = sky.cloud_shape.x;
  const float top = sky.cloud_shape.y;
  if (world_height_m <= base || world_height_m >= top) return 0.0;
  const float u = (world_height_m - base) / max(top - base, 1e-4);
  const float wave = sin(3.14159265358979323846 * u);
  return wave * wave;
}

float pf08_cloud_density(const pf08_sky_block sky, const vec3 world_position) {
  return pf08_cloud_vertical_density(sky, world_position.y) *
         pf08_cloud_shape_density(sky, world_position);
}

// Интеграл sin²(pi*u) от высоты приёмника до вершины слоя. Полный вертикальный столб равен
// половине толщины слоя; эта CPU/GPU-проверяемая формула позволяет поверхности получить облачную
// тень без второго raymarch на каждый пиксель.
float pf08_cloud_vertical_column(const pf08_sky_block sky, const float receiver_height_m) {
  const float base = sky.cloud_shape.x;
  const float top = sky.cloud_shape.y;
  const float thickness = max(top - base, 1e-4);
  if (receiver_height_m >= top) return 0.0;
  if (receiver_height_m <= base) return thickness * 0.5;
  const float u = (receiver_height_m - base) / thickness;
  const float pi = 3.14159265358979323846;
  return thickness * ((1.0 - u) * 0.5 + sin(2.0 * pi * u) / (4.0 * pi));
}

float pf08_cloud_light_transmittance(const pf08_sky_block sky, const vec3 world_position,
                                     const vec3 light_direction) {
  const float extinction = max(sky.cloud_params.y, 0.0);
  if (sky.cloud_params.x <= 0.0 || extinction <= 0.0) return 1.0;
  if (light_direction.y <= 0.0) return 0.0;

  const float top = sky.cloud_shape.y;
  if (world_position.y >= top) return 1.0;
  const float first_height = max(world_position.y, sky.cloud_shape.x);
  const float midpoint_height = 0.5 * (first_height + top);
  const float distance_to_midpoint = (midpoint_height - world_position.y) / light_direction.y;
  const vec2 midpoint_xz = world_position.xz + light_direction.xz * distance_to_midpoint;
  const float horizontal = pf08_cloud_shape_density(
    sky, vec3(midpoint_xz.x, midpoint_height, midpoint_xz.y));
  const float column_m = pf08_cloud_vertical_column(sky, world_position.y) /
                         max(light_direction.y, 1e-4);
  return exp(-extinction * horizontal * column_m);
}

float pf08_weather_volume_range(const pf08_sky_block sky) {
  const bool fog_active = sky.fog_params.x > 0.0;
  const bool cloud_active = sky.cloud_params.x > 0.0 && sky.cloud_params.y > 0.0;
  const bool rain_active = sky.precipitation_params.x > 0.0 && sky.precipitation_shape.y > 0.0;
  return max(max(fog_active ? sky.fog_params.w : 0.0, cloud_active ? sky.cloud_shape.w : 0.0),
             rain_active ? sky.precipitation_shape.w : 0.0);
}

#endif
