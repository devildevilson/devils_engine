// Пространственный отклик поверхности на ИСТОРИЮ осадков. Host интегрирует water equivalent и
// wetness, а здесь одна world-space формула превращает их в пятна, толщину и материал.

#ifndef PF08_SURFACE_WEATHER_GLSL
#define PF08_SURFACE_WEATHER_GLSL

#include "pf08_precipitation.glsl"

float pf08_surface_hash(const vec2 cell) {
  return fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453123);
}

float pf08_surface_value_noise(const vec2 position) {
  const vec2 cell = floor(position);
  const vec2 local = fract(position);
  const vec2 blend = local * local * (3.0 - 2.0 * local);
  const float a = pf08_surface_hash(cell);
  const float b = pf08_surface_hash(cell + vec2(1.0, 0.0));
  const float c = pf08_surface_hash(cell + vec2(0.0, 1.0));
  const float d = pf08_surface_hash(cell + vec2(1.0, 1.0));
  return mix(mix(a, b, blend.x), mix(c, d, blend.x), blend.y);
}

float pf08_surface_snow_mask(const pf08_sky_block sky, const vec3 world_position,
                             const vec3 world_normal, const float foliage) {
  const float coverage = clamp(sky.surface_weather.y, 0.0, 1.0);
  if (sky.surface_weather.w < 0.5 || sky.surface_weather.x <= 0.0 || coverage <= 0.0 ||
      foliage > 0.5) return 0.0;

  const float slope = smoothstep(0.30, 0.78, normalize(world_normal).y);
  if (slope <= 0.0) return 0.0;
  const bool sheltered = pf08_shelter_blocks_precipitation(
    sky, world_position, pf08_precipitation_velocity(sky, true));
  if (sheltered) return 0.0;

  const float cell = max(sky.surface_weather_shape.w, 0.5);
  const float broad = pf08_surface_value_noise(world_position.xz / cell);
  const float detail = pf08_surface_value_noise(world_position.xz / (cell * 0.37) + vec2(19.7, 7.3));
  const float field = broad * 0.72 + detail * 0.28;
  const float threshold = 1.0 - coverage;
  float patch_weight = smoothstep(threshold - 0.16, threshold + 0.16, field);
  patch_weight *= smoothstep(0.0, 0.025, coverage);
  patch_weight = mix(patch_weight, 1.0, smoothstep(0.86, 1.0, coverage));
  return slope * patch_weight;
}

float pf08_surface_wet_mask(const pf08_sky_block sky, const vec3 world_position) {
  if (sky.surface_weather.w < 0.5 || sky.surface_weather.z <= 0.0) return 0.0;
  const bool sheltered = pf08_shelter_blocks_precipitation(
    sky, world_position, pf08_precipitation_velocity(sky, false));
  return sheltered ? 0.0 : clamp(sky.surface_weather.z, 0.0, 1.0);
}

vec3 pf08_apply_snow_displacement(const pf08_sky_block sky, const vec3 world_position,
                                  const vec3 world_normal, const float foliage) {
  if (sky.surface_weather_shape.x < 0.5) return world_position;
  const float mask = pf08_surface_snow_mask(sky, world_position, world_normal, foliage);
  vec3 displaced = world_position;
  // Снег оседает по гравитации, а не раздувает меш вдоль нормали. На склоне это даёт меньшую
  // вертикальную толщину через slope-mask и не раздвигает края коробок в стороны.
  displaced.y += min(sky.surface_weather.x, sky.surface_weather_shape.y) * mask;
  return displaced;
}

void pf08_surface_weather_material(const pf08_sky_block sky, const vec3 world_position,
                                   const vec3 world_normal, const float foliage,
                                   const vec3 base_albedo, out vec3 albedo,
                                   out float roughness, out vec3 f0,
                                   out float snow_mask, out float wet_mask) {
  if (sky.surface_weather.w < 0.5 ||
      (sky.surface_weather.x <= 0.0 && sky.surface_weather.z <= 0.0)) {
    albedo = base_albedo;
    roughness = 0.72;
    f0 = vec3(0.025);
    snow_mask = 0.0;
    wet_mask = 0.0;
    return;
  }
  snow_mask = pf08_surface_snow_mask(sky, world_position, world_normal, foliage);
  wet_mask = pf08_surface_wet_mask(sky, world_position) * (1.0 - snow_mask);

  const float fine = pf08_surface_value_noise(world_position.xz * 0.43 + vec2(3.1, 11.9));
  const vec3 snow_albedo = mix(vec3(0.72, 0.78, 0.82), vec3(0.90, 0.94, 0.97), fine);
  const vec3 wet_albedo = base_albedo * mix(1.0, 0.58, wet_mask);
  albedo = mix(wet_albedo, snow_albedo, snow_mask);
  // Это мокрая шероховатая почва и бетон, не идеальная лужа. При 0.10 вся наклонная долина
  // отражала один резкий texel sky-view LUT и становилась бело-голубым зеркалом.
  roughness = mix(0.72, 0.22, wet_mask);
  roughness = mix(roughness, 0.86, snow_mask);
  f0 = mix(vec3(0.025), vec3(0.045), wet_mask);
  f0 = mix(f0, vec3(0.030), snow_mask);
}

#endif
