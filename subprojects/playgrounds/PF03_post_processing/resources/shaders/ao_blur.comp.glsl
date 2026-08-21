#version 450

// Алгоритм: depth-aware сглаживание сырого SSAO в половинном разрешении. Для каждого пикселя берётся окно
// 5x5, но в среднее проходят только отсчёты с близкой линейной глубиной; так случайный шум AO усредняется,
// а затенение не перетекает через силуэт на другую поверхность или небо. В .g сохраняется глубина центра,
// потому что следующий полноэкранный шейдинг тем же способом выберет правильный half-resolution тексель.

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D ao_image;

layout(set = 2, binding = 0, rg16f) uniform writeonly image2D ao_blurred;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(ao_blurred);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 center = texelFetch(ao_image, pixel, 0).rg;
  const float center_depth = center.g;

  // Cross-bilateral: усредняем только те тексели, что лежат на той же поверхности. Обычный box размазал бы
  // AO через силуэты, и объекты получили бы тёмную кайму на фоне.
  float sum = 0.0;
  float weight_sum = 0.0;
  const float depth_tolerance = max(center_depth * 0.02, 0.05);

  for (int y = -2; y <= 2; ++y) {
    for (int x = -2; x <= 2; ++x) {
      const ivec2 tap = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
      const vec2 value = texelFetch(ao_image, tap, 0).rg;
      // Небо помечено глубиной 0 и в среднее не идёт
      if (value.g <= 0.0) {
        continue;
      }
      const float weight = abs(value.g - center_depth) < depth_tolerance ? 1.0 : 0.0;
      sum += value.r * weight;
      weight_sum += weight;
    }
  }

  const float result = weight_sum > 0.0 ? sum / weight_sum : center.r;
  imageStore(ao_blurred, pixel, vec4(result, center_depth, 0.0, 0.0));
}
