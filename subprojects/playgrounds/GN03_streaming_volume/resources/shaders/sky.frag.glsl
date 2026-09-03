#version 450

#include "camera_block.glsl"

layout(location = 0) in vec2 in_ndc;

layout(location = 0) out vec4 out_colour;

// Небо здесь служебное: оно нужно ровно для того, чтобы туман имел цвет, в который уходит рельеф.
// Поэтому у него нет ни солнца с ореолом, ни облаков — есть градиент и диск светила, и цвет у
// горизонта тот же самый, в который уводит дальний рельеф. Пока цвет тумана и цвет неба считались
// по-разному, край окна чанков был виден как полоса другого оттенка, то есть ровно то, что площадка
// проверяет глазами, ломалось самой картинкой.
void main() {
  // Направление луча: точка ближней плоскости минус камера. Ближняя, а не дальняя, потому что
  // проекция обратная и бесконечная — у дальней плоскости w обращается в ноль.
  const vec4 near_point = camera.inverse_view_projection * vec4(in_ndc, 1.0, 1.0);
  const vec3 direction = normalize(near_point.xyz / near_point.w - camera.camera_position.xyz);

  const vec3 horizon = camera.sky_colour.rgb;
  const vec3 zenith = horizon * vec3(0.62, 0.74, 1.05);
  const vec3 ground = horizon * vec3(0.55, 0.52, 0.48);

  const float height = direction.y;
  vec3 colour = height >= 0.0 ? mix(horizon, zenith, pow(clamp(height, 0.0, 1.0), 0.65))
                              : mix(horizon, ground, pow(clamp(-height, 0.0, 1.0), 0.5));

  // Светило диском, а не точкой: точка не попадает ни в один пиксель, и «где солнце» становится
  // вопросом без ответа — а по нему читается направление тени на рельефе.
  const float alignment = dot(direction, camera.sun_direction.xyz);
  colour += vec3(1.0, 0.96, 0.86) * smoothstep(0.9985, 0.9995, alignment) * 3.0;
  colour += vec3(1.0, 0.93, 0.80) * pow(clamp(alignment, 0.0, 1.0), 64.0) * 0.35;

  out_colour = vec4(colour, 1.0);
}
