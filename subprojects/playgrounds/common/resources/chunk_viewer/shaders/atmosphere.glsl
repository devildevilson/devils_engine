// АТМОСФЕРА ОДНА НА ВСЕ ШЕЙДЕРЫ: небо, рельеф и сущности обязаны уходить в один и тот же цвет
// по одному и тому же закону, иначе край окна чанков виден полосой другого оттенка.
//
// Режим GN03 (по умолчанию): солнце, полусферическое небо, туман цвета горизонта до края окна.
// Подводный режим: солнца нет, свет — слабое рассеяние толщи и фонарь у камеры; туман — мутность
// воды плюс гарантированная полная плотность к концу `atmosphere[1].y`, а фон ровно цвета воды.

bool atmosphere_underwater() { return camera.atmosphere[0].w > 0.5; }

vec3 atmosphere_light(vec3 normal, vec3 position) {
  if (!atmosphere_underwater()) {
    const float sun = max(dot(normal, camera.sun_direction.xyz), 0.0);
    // Полусферическое небо вместо постоянной подсветки: без него дно пещеры и северный склон
    // одинаково черны, и рельефа в тени не видно вовсе.
    const float sky = 0.5 + 0.5 * normal.y;
    return vec3(1.0, 0.96, 0.88) * sun + camera.sky_colour.rgb * sky * 0.45;
  }
  const vec3 water = camera.atmosphere[0].rgb;
  // Рассеянный свет толщи: слабый, сверху чуть светлее, цвета воды.
  vec3 light = (water * 5.0 + vec3(0.018, 0.030, 0.034)) * camera.atmosphere[2].x *
               (0.65 + 0.35 * (0.5 + 0.5 * normal.y));
  // Фонарь у камеры: конус вдоль взгляда, ослабление с расстоянием.
  const vec4 centre = camera.inverse_view_projection * vec4(0.0, 0.0, 1.0, 1.0);
  const vec3 forward = normalize(centre.xyz / centre.w - camera.camera_position.xyz);
  const vec3 to_camera = camera.camera_position.xyz - position;
  const float distance = length(to_camera);
  const vec3 towards = to_camera / max(distance, 1.0e-3);
  const float cone = smoothstep(0.80, 0.94, dot(-towards, forward));
  const float range = max(camera.atmosphere[1].z, 1.0);
  const float falloff = 1.0 / (1.0 + (distance / range) * (distance / range));
  const float lamp = camera.atmosphere[1].w * cone * falloff * max(dot(normal, towards), 0.0);
  // Мягкое сжатие фонаря: вплотную к стене или станции он не выжигает материал в белый.
  light += vec3(1.0, 0.94, 0.82) * lamp / (1.0 + 0.4 * lamp);
  return light;
}

vec3 atmosphere_fog(vec3 colour, vec3 position) {
  const float distance = length(camera.camera_position.xyz - position);
  if (!atmosphere_underwater()) {
    // Туман до самой границы окна чанков: дальность приходит из окна, а цвет — тот же, что у
    // неба у горизонта. Иначе край мира виден как обрыв в пустоту.
    const float fog = clamp(pow(distance / max(camera.params.y, 1.0), 2.2), 0.0, 1.0);
    return mix(colour, camera.sky_colour.rgb, fog);
  }
  // Мутность воды плюс гарантированная полная плотность к концу: за ним — только цвет воды,
  // как у фона, поэтому край окна и ещё не подгруженные чанки не видны.
  const float murk = 1.0 - exp(-distance * camera.atmosphere[2].y);
  const float edge = smoothstep(camera.atmosphere[1].x, camera.atmosphere[1].y, distance);
  return mix(colour, camera.atmosphere[0].rgb, max(murk, edge));
}
