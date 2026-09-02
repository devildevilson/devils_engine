#version 450
#include <utils/shared.h>

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_tint;
layout(location = 0) out vec4 out_colour;

layout(set = 1, binding = 0) uniform texture2D textures[16];
layout(set = 1, binding = 1) uniform sampler samplers[1];
#define LABEL_SAMPLE sampler2D(textures[clamp(uint(in_tint.a + 0.5), 0u, 15u)], samplers[0])

void main() {
  // Атлас шрифта — MSDF, поэтому берётся медиана трёх каналов: она и есть знаковое расстояние до
  // контура буквы, и именно оно даёт резкую букву при любом размере. Обычная текстура покрытия здесь
  // расплылась бы, потому что подпись на глобусе меняет размер непрерывно.
  const vec3 sample_value = texture(LABEL_SAMPLE, in_uv).rgb;
  const float distance_value = median3(sample_value.r, sample_value.g, sample_value.b);

  const vec2 unit_range = vec2(4.0) / vec2(textureSize(LABEL_SAMPLE, 0));
  const vec2 screen_texture_size = vec2(1.0) / max(fwidth(in_uv), vec2(1e-6));
  const float range = max(0.5 * dot(unit_range, screen_texture_size), 1.0);

  const float coverage = clamp(range * (distance_value - 0.5) + 0.5, 0.0, 1.0);
  // Обводка: та же медиана с более далёким порогом даёт контур на полпикселя шире буквы. Без него
  // светлая подпись пропадает на светлой суше, а тёмная — на океане.
  const float outline = clamp(range * (distance_value - 0.32) + 0.5, 0.0, 1.0);
  if (outline <= 1.0 / 255.0) {
    discard;
  }

  const vec3 colour = mix(vec3(0.04, 0.05, 0.07), in_tint.rgb, coverage);
  out_colour = vec4(colour, outline);
}
