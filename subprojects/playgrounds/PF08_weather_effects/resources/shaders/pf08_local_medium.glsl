// Общий высотный профиль локальной среды. Его обязаны читать и froxel-интеграл, и поверхности:
// иначе объём покажет густой туман, а земля продолжит получать сухой прямой свет и резкие тени.

#ifndef PF08_LOCAL_MEDIUM_GLSL
#define PF08_LOCAL_MEDIUM_GLSL

float pf08_fog_hash(const vec2 cell) {
  vec3 value = fract(vec3(cell.x, cell.y, cell.x) * 0.1031);
  value += dot(value, value.yzx + 33.33);
  return fract((value.x + value.y) * value.z);
}

float pf08_fog_value_noise(const vec2 position) {
  const vec2 cell = floor(position);
  const vec2 local = fract(position);
  const vec2 blend = local * local * (3.0 - 2.0 * local);
  const float a = pf08_fog_hash(cell);
  const float b = pf08_fog_hash(cell + vec2(1.0, 0.0));
  const float c = pf08_fog_hash(cell + vec2(0.0, 1.0));
  const float d = pf08_fog_hash(cell + vec2(1.0, 1.0));
  return mix(mix(a, b, blend.x), mix(c, d, blend.x), blend.y);
}

// Ячейки живут в МИРОВЫХ xz, а не в координатах камеры, поэтому не едут за наблюдателем. Advection
// использует то же направление ветра и то же реальное время, что листва; отдельной погодной скоростью
// задаётся лишь перевод метры/секунда, поскольку amplitude качания куста измеряется в других единицах.
float pf08_fog_column_modulation(const pf08_sky_block sky, const vec3 world_position) {
  const float variation = clamp(sky.fog_noise.x, 0.0, 0.95);
  if (variation <= 0.0) return 1.0;

  const float cell_size = max(sky.fog_noise.y, 1e-3);
  const float speed = max(sky.fog_noise.z, 0.0);
  const vec2 advected = world_position.xz - sky.wind_params.xy * speed * sky.wind_params.w;
  const vec2 coordinate = advected / cell_size;
  const float broad = pf08_fog_value_noise(coordinate);
  const float detail = pf08_fog_value_noise(coordinate * 2.03 + vec2(17.1, -9.2));
  const float field = smoothstep(0.12, 0.88, broad * 0.72 + detail * 0.28);
  return max(0.05, 1.0 + variation * (field * 2.0 - 1.0));
}

float pf08_fog_density(const pf08_sky_block sky, const float world_height_m) {
  const float base_height = sky.fog_shape.x;
  const float scale_height = max(sky.fog_shape.y, 1e-4);
  return exp(-max(world_height_m - base_height, 0.0) / scale_height);
}

float pf08_fog_density(const pf08_sky_block sky, const vec3 world_position) {
  return pf08_fog_density(sky, world_position.y) * pf08_fog_column_modulation(sky, world_position);
}

// Аналитическая оптическая толщина экспоненциального слоя вдоль направления к светилу. Ниже base
// плотность единична, выше падает как exp(-h/H). Поэтому вертикальный столб над точкой имеет конечную
// толщину H*rho, а наземный прямой свет можно ослабить без второго raymarch на каждый пиксель.
float pf08_fog_light_transmittance(const pf08_sky_block sky, const vec3 world_position,
                                   const vec3 light_direction, const float column_modulation) {
  const float extinction = max(sky.fog_params.x, 0.0);
  if (extinction <= 0.0) return 1.0;
  if (light_direction.y <= 0.0) return 0.0;

  const float base_height = sky.fog_shape.x;
  const float scale_height = max(sky.fog_shape.y, 1e-4);
  const float inverse_vertical = 1.0 / max(light_direction.y, 1e-4);
  const float vertical_column_m = world_position.y < base_height
    ? (base_height - world_position.y + scale_height) * inverse_vertical
    : scale_height * pf08_fog_density(sky, world_position.y) * inverse_vertical;
  // Модель намеренно столбовая: низкочастотная xz-ячейка вытянута по вертикали. Это сохраняет
  // аналитический световой столб на surface-pass и избегает второго raymarch на каждый пиксель.
  const float column_m = vertical_column_m * column_modulation;
  return exp(-extinction * column_m);
}

float pf08_fog_light_transmittance(const pf08_sky_block sky, const vec3 world_position,
                                   const vec3 light_direction) {
  return pf08_fog_light_transmittance(
    sky, world_position, light_direction, pf08_fog_column_modulation(sky, world_position));
}

#endif
