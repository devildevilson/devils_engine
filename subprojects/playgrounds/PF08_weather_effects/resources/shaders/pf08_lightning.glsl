#ifndef PF08_LIGHTNING_GLSL
#define PF08_LIGHTNING_GLSL

// Один источник света для weather/scripted/magic. Автор события не меняет световую модель; он нужен
// игровому коду для расписания, урона, звука и правил взаимодействия. Канал и вспышка намеренно имеют
// разные envelope: геометрия живёт миллисекунды, облако и адаптация глаза помнят её дольше.
vec3 pf08_lightning_nearest_point(const pf08_sky_block sky, const vec3 world_position) {
  const vec3 start = sky.lightning_start_channel.xyz;
  const vec3 segment = sky.lightning_end_flash.xyz - start;
  const float length_squared = dot(segment, segment);
  const float along = length_squared > 1e-6
    ? clamp(dot(world_position - start, segment) / length_squared, 0.0, 1.0) : 0.0;
  return start + segment * along;
}

vec3 pf08_lightning_illuminance(const pf08_sky_block sky, const vec3 world_position,
                                out vec3 light_direction) {
  const float flash = max(sky.lightning_end_flash.w, 0.0);
  const float intensity_cd = max(sky.lightning_colour_intensity.w, 0.0);
  if (flash <= 1e-5 || intensity_cd <= 0.0) {
    light_direction = vec3(0.0, 1.0, 0.0);
    return vec3(0.0);
  }

  const vec3 to_channel = pf08_lightning_nearest_point(sky, world_position) - world_position;
  const float distance_squared = dot(to_channel, to_channel);
  const float distance_m = sqrt(max(distance_squared, 1e-8));
  light_direction = to_channel / max(distance_m, 1e-4);

  // Радиус glow ограничивает область работы локального источника, но не утолщает канал. Небольшое
  // softening ядро предотвращает сингулярность прямо в точке удара; 200 klx — предел HDR-сигнала,
  // не попытка сделать пик молнии физически тусклым.
  const float glow_radius = max(sky.lightning_shape.z, 1.0);
  const float support = 1.0 - smoothstep(glow_radius * 0.72, glow_radius, distance_m);
  const float core_radius = max(4.0, glow_radius * 0.0125);
  const float illuminance_lx = min(intensity_cd * flash /
    max(distance_squared + core_radius * core_radius, 1e-4), 200000.0) * support;
  return sky.lightning_colour_intensity.rgb * illuminance_lx;
}

vec3 pf08_lightning_surface_illuminance(const pf08_sky_block sky, const vec3 world_position,
                                        const vec3 normal) {
  vec3 light_direction;
  const vec3 light = pf08_lightning_illuminance(sky, world_position, light_direction);
  return light * max(dot(normal, light_direction), 0.0);
}

#endif
