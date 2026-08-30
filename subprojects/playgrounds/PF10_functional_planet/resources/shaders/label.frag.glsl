#version 450
#include <utils/shared.h>
layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in uint in_texture;
layout(location = 0) out vec4 out_color;
layout(set = 1, binding = 0) uniform texture2D textures[16];
layout(set = 1, binding = 1) uniform sampler samplers[1];
#define LABEL_SAMPLER sampler2D(textures[clamp(in_texture, 0u, 15u)], samplers[0])
void main() {
  const vec4 sample_value = texture(LABEL_SAMPLER, in_uv);
  const float distance_value = median3(sample_value.r, sample_value.g, sample_value.b);
  const vec2 unit_range = vec2(4.0) / vec2(textureSize(LABEL_SAMPLER, 0));
  const float range = max(0.5 * dot(unit_range, vec2(1.0) / fwidth(in_uv)), 1.0);
  const float fill = clamp(range * (distance_value - 0.5) + 0.5, 0.0, 1.0);
  const float outline = clamp(range * (sample_value.a - 0.38) + 0.5, 0.0, 1.0);
  out_color = vec4(mix(vec3(0.035, 0.045, 0.055), vec3(0.94, 0.91, 0.80), fill), max(fill, outline * 0.92));
  if (out_color.a <= 1.0 / 255.0) discard;
}
