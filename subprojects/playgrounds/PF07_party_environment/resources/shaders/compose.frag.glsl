#version 450

#include "pf07_records.glsl"

// Временный вывод среза 2: ручная экспозиция и одна кривая. Физическая экспозиция с ночной адаптацией,
// порогами и пресетами с фиксированным EV принадлежит срезу 3 — до него сравнивать состояния можно
// только при явно заданной экспозиции, поэтому она здесь ровно один параметр и ничего не подстраивает
// автоматически.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D sky_color;
layout(set = 0, binding = 1, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

vec3 tonemap_aces(const vec3 value) {
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((value * (a * value + b)) / (value * (c * value + d) + e), 0.0, 1.0);
}

void main() {
  const vec3 radiance = texture(sky_color, in_uv).rgb;
  if (sky_data.sky.output_params.w > 0.5) {
    out_color = vec4(clamp(radiance, 0.0, 1.0), 1.0);
    return;
  }
  const float exposure = sky_data.sky.output_params.x;
  const vec3 exposed = radiance * exposure;

  vec3 mapped = tonemap_aces(exposed);
  // Вывод в sRGB. Здесь это единственное место, где линейный свет превращается в изображение.
  mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));
  out_color = vec4(mapped, 1.0);
}
