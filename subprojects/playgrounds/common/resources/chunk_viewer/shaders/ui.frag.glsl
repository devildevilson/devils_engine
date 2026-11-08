#version 450
#include <utils/shared.h>
layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;
layout(set = 1, binding = 0) uniform texture2D textures[16];
layout(set = 1, binding = 1) uniform sampler samplers[1];
layout(push_constant) uniform UiPush { uint texture_id; uint payload[6]; } ui;
#define UI_SAMPLE(index) sampler2D(textures[clamp((index), 0u, 15u)], samplers[0])
float screen_pixel_range(const uint index) {
  const vec2 unit_range = vec2(4.0) / vec2(textureSize(UI_SAMPLE(index), 0));
  const vec2 screen_texture_size = vec2(1.0) / fwidth(in_uv);
  return max(0.5 * dot(unit_range, screen_texture_size), 1.0);
}
void main() {
  if (tex_type_of(ui.texture_id) == ui_draw_msdf) {
    const uint index = tex_index_of(ui.texture_id);
    const vec3 sample_value = texture(UI_SAMPLE(index), in_uv).rgb;
    const float distance_value = median3(sample_value.r, sample_value.g, sample_value.b);
    const float range = screen_pixel_range(index) / (1.0 + uintBitsToFloat(ui.payload[3]) * 4.0);
    const float alpha = clamp(range * (distance_value - (0.5 - uintBitsToFloat(ui.payload[0]))) + 0.5, 0.0, 1.0);
    out_color = vec4(in_color.rgb, in_color.a * alpha);
  } else out_color = in_color;
}
