#version 450

// Алгоритм: fullscreen-проход дилатирует selection coverage круглой окрестностью на три экранных пикселя
// и вычитает исходную маску. Вместе с ближайшим покрытым texel'ом наружу переносится reverse-Z depth объекта:
// локальный material пишет её в gl_FragDepth и проходит обычный scene depth test. Второй pipeline variant
// намеренно отключает test и по runtime-флагу становится through-wall debug/gameplay selector'ом.

#ifndef PF04_OUTLINE_OVERLAY
#define PF04_OUTLINE_OVERLAY 0
#endif

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 debug_params;
  vec4 effect_params;
} camera_data;

layout(set = 1, binding = 0) uniform sampler2D selection_mask_image;

layout(location = 0) out vec4 out_color;

void main() {
#if PF04_OUTLINE_OVERLAY
  if (camera_data.effect_params.y < 0.5) discard;
#endif

  const vec2 size = vec2(textureSize(selection_mask_image, 0));
  const vec2 texel = 1.0 / size;
  const vec2 uv = gl_FragCoord.xy * texel;
  const vec2 center_sample = texture(selection_mask_image, uv).rg;
  const float center = center_sample.r;

  float expanded = center;
  float nearest_distance = 1e10;
  float outline_depth = 0.0;
  for (int y = -3; y <= 3; ++y) {
    for (int x = -3; x <= 3; ++x) {
      // Круглая окрестность не даёт квадратных выступов на диагональных углах.
      if ((x == 0 && y == 0) || x * x + y * y > 10) continue;
      const vec2 sample_value = texture(selection_mask_image, uv + vec2(x, y) * texel).rg;
      expanded = max(expanded, sample_value.r);
      if (sample_value.r <= 0.01) continue;

      // Coverage слегка сдвигает выбор к полноценному внутреннему texel'у при одинаковом расстоянии.
      const float distance = length(vec2(x, y)) - sample_value.r * 0.25;
      if (distance >= nearest_distance) continue;
      nearest_distance = distance;
      outline_depth = sample_value.g / sample_value.r;
    }
  }

  const float outer = smoothstep(0.05, 0.95, expanded);
  const float inner = smoothstep(0.05, 0.95, center);
  const float coverage = outer * (1.0 - inner);
  if (coverage <= 0.001 || nearest_distance > 100.0) discard;
  gl_FragDepth = outline_depth;
#if PF04_OUTLINE_OVERLAY
  out_color = vec4(1.0, 0.72, 0.10, coverage);
#else
  out_color = vec4(1.0, 0.42, 0.06, coverage);
#endif
}
