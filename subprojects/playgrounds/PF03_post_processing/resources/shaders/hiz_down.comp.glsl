#version 450

// Алгоритм: один уровень консервативной Hi-Z пирамиды. Для каждого дочернего текселя объединяются min/max
// reverse-Z глубины родительского блока 2x2: минимум — дальняя граница, максимум — ближняя. Если размер
// родителя нечётный, последний дочерний тексель забирает ещё строку/столбец, чтобы край кадра не потерялся.
// Пара границ позволяет ray marcher доказать отсутствие пересечения для целого блока.

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D parent_image;
layout(set = 1, binding = 1, rg32f) uniform writeonly image2D hiz_image;

// Шаг понижения пирамиды глубины: .r = минимум, .g = максимум по блоку родительского уровня. В reverse-Z
// «максимум» это САМАЯ БЛИЗКАЯ поверхность блока, а «минимум» — самая далёкая (у неба глубина ровно 0, то есть
// бесконечность). Обе границы нужны: по максимуму марш пропускает блок, целиком лежащий за лучом, по минимуму —
// целиком перед ним.
void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(hiz_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const ivec2 parent_size = textureSize(parent_image, 0);
  const ivec2 base = pixel * 2;

  // НЕЧЁТНЫЙ размер родителя — не мелочь. Уровень имеет floor(N/2) текселей, блоки 2x2 покрывают N-1 элементов,
  // и последний столбец (строка) остаётся снаружи. Пирамида при этом выглядит правильной, но перестаёт быть
  // консервативной ровно у края кадра: марш решит, что там ничего нет. Поэтому последний тексель забирает
  // лишний столбец и/или строку — включая угол.
  const int extra_x = ((parent_size.x & 1) != 0 && pixel.x == size.x - 1) ? 1 : 0;
  const int extra_y = ((parent_size.y & 1) != 0 && pixel.y == size.y - 1) ? 1 : 0;

  vec2 bounds = vec2(1.0e30, -1.0e30);
  for (int y = 0; y <= 1 + extra_y; ++y) {
    for (int x = 0; x <= 1 + extra_x; ++x) {
      const ivec2 tap = clamp(base + ivec2(x, y), ivec2(0), parent_size - 1);
      const vec2 value = texelFetch(parent_image, tap, 0).rg;
      bounds.x = min(bounds.x, value.x);
      bounds.y = max(bounds.y, value.y);
    }
  }

  imageStore(hiz_image, pixel, vec4(bounds, 0.0, 0.0));
}
