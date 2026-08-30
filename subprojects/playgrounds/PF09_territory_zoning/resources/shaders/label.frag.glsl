#version 450
#include <utils/shared.h>

// Тот же MSDF-атлас, что у экранного overlay: второй атлас ради надписей в мире не нужен, а нужен общий
// признак «это глиф или сплошная заливка». Им служит отрицательный u — координаты атласа неотрицательны,
// поэтому значение вне диапазона свободно и не отнимает ни бита у полезных данных.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) flat in uint in_font;
layout(location = 0) out vec4 out_color;

layout(set = 1, binding = 0) uniform texture2D textures[16];
layout(set = 1, binding = 1) uniform sampler samplers[1];

#define LABEL_SAMPLE(index) sampler2D(textures[clamp((index), 0u, 15u)], samplers[0])

float screen_pixel_range(const uint index) {
  const vec2 unit_range = vec2(4.0) / vec2(textureSize(LABEL_SAMPLE(index), 0));
  const vec2 screen_texture_size = vec2(1.0) / fwidth(in_uv);
  return max(0.5 * dot(unit_range, screen_texture_size), 1.0);
}

void main() {
  if (in_uv.x < 0.0) {
    out_color = in_color;
  } else {
    const vec4 value = texture(LABEL_SAMPLE(in_font), in_uv);
    const float distance_value = median3(value.r, value.g, value.b);
    const float coverage = clamp(screen_pixel_range(in_font) * (distance_value - 0.5) + 0.5, 0.0, 1.0);
    out_color = vec4(in_color.rgb, in_color.a * coverage);
  }
  if (out_color.a <= 1.0 / 255.0) discard;
}
