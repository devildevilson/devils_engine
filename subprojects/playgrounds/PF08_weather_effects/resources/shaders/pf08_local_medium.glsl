// Общий высотный профиль локальной среды. Его обязаны читать и froxel-интеграл, и поверхности:
// иначе объём покажет густой туман, а земля продолжит получать сухой прямой свет и резкие тени.

#ifndef PF08_LOCAL_MEDIUM_GLSL
#define PF08_LOCAL_MEDIUM_GLSL

float pf08_fog_density(const pf08_sky_block sky, const float world_height_m) {
  const float base_height = sky.fog_shape.x;
  const float scale_height = max(sky.fog_shape.y, 1e-4);
  return exp(-max(world_height_m - base_height, 0.0) / scale_height);
}

// Аналитическая оптическая толщина экспоненциального слоя вдоль направления к светилу. Ниже base
// плотность единична, выше падает как exp(-h/H). Поэтому вертикальный столб над точкой имеет конечную
// толщину H*rho, а наземный прямой свет можно ослабить без второго raymarch на каждый пиксель.
float pf08_fog_light_transmittance(const pf08_sky_block sky, const vec3 world_position,
                                   const vec3 light_direction) {
  const float extinction = max(sky.fog_params.x, 0.0);
  if (extinction <= 0.0) return 1.0;
  if (light_direction.y <= 0.0) return 0.0;

  const float base_height = sky.fog_shape.x;
  const float scale_height = max(sky.fog_shape.y, 1e-4);
  const float inverse_vertical = 1.0 / max(light_direction.y, 1e-4);
  const float column_m = world_position.y < base_height
    ? (base_height - world_position.y + scale_height) * inverse_vertical
    : scale_height * pf08_fog_density(sky, world_position.y) * inverse_vertical;
  return exp(-extinction * column_m);
}

#endif
