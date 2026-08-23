#version 450
#include <utils/shared.h>

// Алгоритм: packed texture id отделяет solid Nuklear shapes от Crimson MSDF. Median RGB восстанавливает signed
// distance, screen derivatives сохраняют толщину glyph edge при любом DPI; тот же UI path позже обслужит sliders.

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 frag_color;
layout(set = 1, binding = 0) uniform texture2D tex[16];
layout(set = 1, binding = 1) uniform sampler samp[1];
layout(push_constant) uniform ui_pc_block {
  uint tex_id;
  uint payload[6];
} pc;

const float px_range = 4.0;
#define S(idx) sampler2D(tex[clamp((idx), 0u, 15u)], samp[0])

float screen_px_range(uint index) {
  const vec2 unit_range = vec2(px_range) / vec2(textureSize(S(index), 0));
  const vec2 screen_tex_size = vec2(1.0) / fwidth(uv);
  return max(0.5 * dot(unit_range, screen_tex_size), 1.0);
}

void main() {
  const uint index = tex_index_of(pc.tex_id);
  const uint mode = tex_type_of(pc.tex_id);
  if (mode == ui_draw_msdf) {
    const float boldness = uintBitsToFloat(pc.payload[0]);
    const float outline_width = uintBitsToFloat(pc.payload[1]);
    const vec4 outline_color = unpackUnorm4x8(pc.payload[2]);
    const float softness = uintBitsToFloat(pc.payload[3]);
    const vec4 value = texture(S(index), uv);
    const float sd = median3(value.r, value.g, value.b);
    const float spx = screen_px_range(index) / (1.0 + softness * 4.0);
    const float threshold = 0.5 - boldness;
    const float fill = clamp(spx * (sd - threshold) + 0.5, 0.0, 1.0);
    if (outline_width > 0.0) {
      const float outer = clamp(spx * (value.a - (threshold - outline_width)) + 0.5, 0.0, 1.0);
      frag_color = vec4(mix(outline_color.rgb, color.rgb, fill), max(fill * color.a, outer * outline_color.a));
    } else {
      frag_color = vec4(color.rgb, color.a * fill);
    }
  } else {
    frag_color = color;
  }
}
