#version 450
#include <utils/shared.h>

// Алгоритм: draw command сообщает тип texture тем же packed id, что использует обычный Visage UI. Solid
// Nuklear shapes проходят без texture; MSDF восстанавливает Crimson coverage; image сэмплит произвольный UI slot.
// Почти нулевая итоговая alpha отбрасывается ДО depth write, иначе прозрачный прямоугольник текста стал бы
// невидимым occluder для других world windows.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_fade;
layout(location = 0) out vec4 frag_color;

layout(set = 2, binding = 0) uniform texture2D textures[16];
layout(set = 2, binding = 1) uniform sampler samplers[1];

layout(push_constant) uniform UiPushBlock {
  uint texture_id;
  uint payload[6];
} pc;

const float px_range = 4.0;
#define S(index) sampler2D(textures[clamp((index), 0u, 15u)], samplers[0])

float screen_px_range(const uint index) {
  const vec2 unit_range = vec2(px_range) / vec2(textureSize(S(index), 0));
  const vec2 screen_texture_size = vec2(1.0) / fwidth(in_uv);
  return max(0.5 * dot(unit_range, screen_texture_size), 1.0);
}

void main() {
  const uint index = tex_index_of(pc.texture_id);
  const uint mode = tex_type_of(pc.texture_id);
  if (mode == ui_draw_msdf) {
    const vec4 value = texture(S(index), in_uv);
    const float signed_distance = median3(value.r, value.g, value.b);
    const float coverage = clamp(screen_px_range(index) * (signed_distance - 0.5) + 0.5, 0.0, 1.0);
    frag_color = vec4(in_color.rgb, in_color.a * coverage);
  } else if (mode == ui_draw_image) {
    frag_color = texture(S(index), in_uv) * in_color;
  } else {
    frag_color = in_color;
  }
  frag_color.a *= in_fade;
  if (frag_color.a <= 1.0 / 255.0) discard;
}
