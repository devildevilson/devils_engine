#version 450

#include "pf07_atmosphere.glsl"

// Таблица sky-view: однократное рассеяние по всем направлениям неба, посчитанное один раз за кадр.
//
// Смысл ровно тот же, что у таблицы прохождения, но на порядок крупнее. Марш по атмосфере считался на
// КАЖДЫЙ пиксель кадра: два миллиона пикселей при 1920x1080. Небо при этом зависит только от
// направления взгляда — камера стоит на поверхности и за кадр никуда не уходит. Значит его достаточно
// посчитать в 192x108 отсчётов, то есть в сотню раз меньше, и дальше просто читать.
//
// Главное следствие: цена неба перестаёт зависеть от разрешения экрана. 1080p больше не дороже 720p.
//
// Ограничение честное: таблица построена для одной высоты наблюдателя. Пока камера стоит на земле,
// это точно; для полёта на километры вверх понадобится либо пересборка таблицы, либо третья ось.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

layout(set = 0, binding = 1) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D sky_view_image;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(sky_view_image);
  if (pixel.x >= size.x || pixel.y >= size.y) return;

  const float ground_radius = sky_data.sky.atmosphere_geometry.x;
  const float top_radius = sky_data.sky.atmosphere_geometry.y;
  const float camera_height = sky_data.sky.march_params.x;
  const int steps = int(sky_data.sky.march_params.y);

  const vec2 texel_count = vec2(size);
  const vec2 uv = (vec2(pixel) + 0.5) / texel_count;
  const vec3 direction = pf07_sky_view_direction(uv, texel_count);
  const vec3 origin = vec3(0.0, ground_radius + camera_height, 0.0);

  // Направления, упирающиеся в планету, в таблицу не пишутся: землю фрагментный шейдер считает сам,
  // её короткий луч стоит дёшево и требует собственного затенения поверхности.
  const float ground_distance = pf07_sphere_hit(origin, direction, ground_radius);
  if (ground_distance > 0.0) {
    imageStore(sky_view_image, pixel, vec4(0.0, 0.0, 0.0, 1.0));
    return;
  }

  const float march_length = pf07_sphere_hit(origin, direction, top_radius);
  vec3 view_transmittance;
  const vec3 in_scattering = pf07_march_scattering(sky_data.sky, transmittance_lut, origin, direction,
                                                   march_length, steps, view_transmittance);

  // В альфе — светимость прохождения вдоль луча. Она нужна фрагментному шейдеру, чтобы ослаблять диски
  // светил и звёзды тем же воздухом, сквозь который смотрит небо, не повторяя марш.
  const float transmittance_luma = dot(view_transmittance, vec3(0.2126, 0.7152, 0.0722));
  imageStore(sky_view_image, pixel, vec4(in_scattering, transmittance_luma));
}
