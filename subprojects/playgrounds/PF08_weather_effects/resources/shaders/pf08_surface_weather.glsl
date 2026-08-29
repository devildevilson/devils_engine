// Минимальный MATERIAL-AWARE отклик на precipitation memory. Карта хранит только историю воды;
// этот файл решает, как конкретная proxy-поверхность её показывает. Никаких выдуманных puddle masks,
// PBR-плёнки и геометрического раздувания: в PF08 для них нет ни material data, ни подходящей сцены.

#ifndef PF08_SURFACE_WEATHER_GLSL
#define PF08_SURFACE_WEATHER_GLSL

#include "pf08_precipitation.glsl"

void pf08_surface_memory_material(const pf08_sky_block sky, const vec4 memory,
                                  const vec3 world_position, const vec3 world_normal,
                                  const float material_kind, const vec3 base_albedo,
                                  out vec3 albedo, out float rain_memory, out float snow_memory) {
  albedo = base_albedo;
  rain_memory = 0.0;
  snow_memory = 0.0;
  if (sky.surface_weather.w < 0.5) return;

  const bool foliage = material_kind > 0.5;
  const bool terrain = material_kind < -0.5;
  const bool sheltered_rain = pf08_shelter_blocks_precipitation(
    sky, world_position, pf08_precipitation_velocity(sky, false));
  const bool sheltered_snow = pf08_shelter_blocks_precipitation(
    sky, world_position, pf08_precipitation_velocity(sky, true));

  // Материал важнее самого rain rate. Земля лишь немного темнеет; нейтральная каменная fixture ещё
  // слабее; листва практически не меняется, потому что капли на листьях требуют отдельной геометрии.
  const float rain_response = foliage ? 0.025 : (terrain ? 0.72 : 0.28);
  const float snow_response = foliage ? 0.035 : (terrain ? 1.0 : 0.55);
  rain_memory = sheltered_rain ? 0.0 : smoothstep(0.08, 1.8, memory.x) * rain_response;
  const float slope = smoothstep(0.30, 0.78, normalize(world_normal).y);
  snow_memory = sheltered_snow ? 0.0 : smoothstep(0.10, 1.4, memory.y) * snow_response * slope;

  // Это намеренно аркадный минимум: широкое, очень слабое потемнение и лёгкий остаток снега.
  // Детальные лужи, грязь, мокрые листья и SSR принадлежат материалам будущей реальной сцены.
  albedo *= 1.0 - rain_memory * 0.14;
  albedo = mix(albedo, vec3(0.80, 0.86, 0.91), snow_memory * 0.12);
}

float pf08_surface_glint_hash(const vec2 cell) {
  return fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453123);
}

float pf08_snow_sparkle(const vec3 world_position, const vec3 normal,
                        const vec3 view_direction, const vec3 light_direction,
                        const float snow_memory) {
  if (snow_memory <= 1e-4) return 0.0;
  // Одна world-locked микрогрань на 12.5 см. Это не normal map: направление строится из hash и
  // остаётся закреплённым за землёй. Узкий highlight даёт множество редких точек только при солнце.
  const vec2 cell = floor(world_position.xz * 8.0);
  const float azimuth = pf08_surface_glint_hash(cell) * 6.2831853;
  const float tilt = mix(0.10, 0.72, pf08_surface_glint_hash(cell + vec2(17.0, 43.0)));
  const vec3 helper = abs(normal.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  const vec3 tangent = normalize(cross(helper, normal));
  const vec3 bitangent = cross(normal, tangent);
  const vec3 micro_normal = normalize(normal * sqrt(1.0 - tilt * tilt) +
    (tangent * cos(azimuth) + bitangent * sin(azimuth)) * tilt);
  const vec3 half_direction = normalize(view_direction + light_direction);
  const float alignment = max(dot(micro_normal, half_direction), 0.0);
  return snow_memory * pow(alignment, 320.0) * 18.0;
}

#endif
