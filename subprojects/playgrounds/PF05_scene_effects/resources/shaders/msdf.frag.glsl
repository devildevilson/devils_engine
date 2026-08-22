#version 450
#include <utils/shared.h>

// Алгоритм: RGB median восстанавливает signed distance MTSDF glyph, а fwidth переводит фиксированный
// pixel range атласа в текущий экранный масштаб — поэтому один Crimson atlas работает для world glyphs
// любого размера. Alpha-канал даёт true distance для внешнего outline. Boldness, outline и softness
// повторяют смысл Visage ui.frag, так что style не зависит от способа размещения текста. Вторая bindless
// texture может модулировать fill (fixture: weathered stone), не меняя signed-distance coverage.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_fill_color;
layout(location = 2) in vec4 in_outline_color;
layout(location = 3) in vec3 in_effect; // boldness, outline width, softness
layout(location = 4) flat in uint in_atlas_index;
layout(location = 5) in vec2 in_detail_uv;
layout(location = 6) flat in uint in_detail_index;
layout(location = 7) flat in float in_detail_mix;
layout(location = 0) out vec4 frag_color;

layout(set = 1, binding = 0) uniform texture2D text_images[16];
layout(set = 1, binding = 1) uniform sampler text_samplers[1];

const float atlas_px_range = 4.0;
#define FONT_SAMPLER sampler2D(text_images[clamp(in_atlas_index, 0u, 15u)], text_samplers[0])
#define DETAIL_SAMPLER sampler2D(text_images[clamp(in_detail_index, 0u, 15u)], text_samplers[0])

float screen_px_range() {
  const vec2 unit_range = vec2(atlas_px_range) / vec2(textureSize(FONT_SAMPLER, 0));
  const vec2 screen_tex_size = vec2(1.0) / fwidth(in_uv);
  return max(0.5 * dot(unit_range, screen_tex_size), 1.0);
}

void main() {
  const vec4 sample_value = texture(FONT_SAMPLER, in_uv);
  const float signed_distance = median3(sample_value.r, sample_value.g, sample_value.b);
  const float px = screen_px_range() / (1.0 + max(in_effect.z, 0.0) * 4.0);
  const float fill_threshold = 0.5 - in_effect.x;
  const float fill = clamp(px * (signed_distance - fill_threshold) + 0.5, 0.0, 1.0);
  vec3 styled_fill = in_fill_color.rgb;
  if (in_detail_mix > 0.0) {
    const vec3 detail = texture(DETAIL_SAMPLER, fract(in_detail_uv * vec2(1.5, 2.0))).rgb;
    styled_fill *= mix(vec3(1.0), detail * 1.35, in_detail_mix);
  }

  if (in_effect.y > 0.0) {
    const float outer = clamp(px * (sample_value.a - (fill_threshold - in_effect.y)) + 0.5, 0.0, 1.0);
    frag_color = vec4(
      mix(in_outline_color.rgb, styled_fill, fill),
      max(fill * in_fill_color.a, outer * in_outline_color.a));
  } else {
    frag_color = vec4(styled_fill, in_fill_color.a * fill);
  }
}
